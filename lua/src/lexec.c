/* Private task execution transitions. See Copyright Notice in lua.h */
#define lexec_c
#define LUA_CORE
#include "lprefix.h"
#include "lapi.h"
#include "lexec.h"
#include "lstate.h"

LuaExecution *luaQ_current (void) {
  return luaE_getnativestackcontext()->execution;
}

void luaQ_entermanaged (lua_State *L) {
  LuaExecution *execution = luaQ_current();
  if (execution != NULL) {
    /* The callback may park before any state gate/world participation exists. */
    if (execution->managed_depth == 0 && execution->managed_enter != NULL)
      execution->managed_enter(execution, L);
    execution->managed_depth++;
  }
}

void luaQ_leavemanaged (void) {
  LuaExecution *execution = luaQ_current();
  if (execution != NULL) {
    lua_assert(execution->managed_depth != 0);
    if (--execution->managed_depth == 0 && execution->managed_leave != NULL)
      execution->managed_leave(execution);
  }
}

void luaQ_beginnative (lua_State *L) {
  LuaExecution *execution = luaQ_current();
  if (execution != NULL) {
    /* Admission runs before the foreign activation exists. */
    if (execution->native_enter != NULL)
      execution->native_enter(execution, L);
    execution->native_depth++;
  }
}

void luaQ_endnative (lua_State *L) {
  LuaExecution *execution = luaQ_current();
  if (execution != NULL) {
    lua_assert(execution->native_depth != 0);
    execution->native_depth--;
    if (execution->native_leave != NULL)
      execution->native_leave(execution, L);
  }
}

void luaQ_restorenative (LuaExecution *execution, lua_State *L, unsigned depth) {
  if (execution != NULL) {
    unsigned previous = execution->native_depth;
    execution->native_depth = depth;
    if (execution->native_restore != NULL)
      execution->native_restore(execution, L, previous);
  }
}

void luaQ_poll (lua_State *L) {
  LuaExecution *execution = luaQ_current();
  if (execution != NULL && execution->poll != NULL)
    execution->poll(execution, L);
}

void luaQ_setmanagedhook (lua_State *L, lua_Hook hook) {
  lua_lock(L);
  __atomic_store_n(&G(L)->managed_hook, hook, __ATOMIC_SEQ_CST);
  lua_unlock(L);
}

int luaQ_ismanagedhook (lua_State *L, lua_Hook hook) {
  return hook == __atomic_load_n(&G(L)->managed_hook, __ATOMIC_SEQ_CST);
}

void luaQ_setowner (LuaOwner *owner) {
  owner->execution = luaQ_current();
  owner->carrier = pthread_self();
}

int luaQ_isowner (const LuaOwner *owner) {
  LuaExecution *execution = luaQ_current();
  return owner->execution == execution &&
         (execution != NULL || pthread_equal(owner->carrier, pthread_self()));
}

/* The caller has published roots and withdrawn collection participation where
** necessary. Registration and wake selection share the predicate's mutex.
** The wait callback handles notification before outgoing context publication.
** Reacquire execution capacity without retaining the predicate mutex. */
void luaQ_wait (lua_State *L, pthread_mutex_t *mutex, pthread_cond_t *changed,
                 LuaWaiter **waiters) {
  LuaExecution *execution = luaQ_current();
  if (execution != NULL && execution->wait != NULL && execution->wake != NULL) {
    LuaWaiter waiter;
    unsigned depth = execution->managed_depth;
    lua_assert(depth != 0);
    waiter.execution = execution;
    waiter.next = *waiters;
    *waiters = &waiter;
    execution->managed_depth = 0;
    if (execution->managed_leave != NULL)
      execution->managed_leave(execution);
    execution->wait(execution, mutex, &waiter);
    luaQ_entermanaged(L);
    execution->managed_depth = depth;
    luaX_checksync(pthread_mutex_lock(mutex));
  }
  else
    luaX_checksync(pthread_cond_wait(changed, mutex));
}

void luaQ_wake (pthread_cond_t *changed, LuaWaiter **waiters) {
  LuaWaiter *waiter = *waiters;
  *waiters = NULL;
  luaX_checksync(pthread_cond_broadcast(changed));
  while (waiter != NULL) {
    LuaWaiter *next = waiter->next;
    waiter->execution->wake(waiter->execution, waiter);
    waiter = next;
  }
}
