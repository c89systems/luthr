/* Private task execution transitions. See Copyright Notice in lua.h */
#define lexec_c
#define LUA_CORE
#include "lprefix.h"
#include "lapi.h"
#include "lexec.h"
#include "lstate.h"
#include "ltask.h"
#include <stdlib.h>

LuaExecution *luaQ_current (void) {
  return luaE_getnativestackcontext()->execution;
}

static void checkcarrier (LuaExecution *execution) {
  /* A driver that ignores an accepted routing reservation must never enter
  ** foreign code on a different carrier. This is a private driver violation. */
  if (execution->pin_depth != 0 &&
      !pthread_equal(execution->pinned_carrier, pthread_self())) abort();
}

int luaQ_pincarrier (LuaCarrierPin *pin) {
  LuaExecution *execution = luaQ_current();
  if (pin->active) return LUA_PIN_INVALID;
  if (execution != NULL) {
    if (execution->pin_depth == ~0u) return LUA_PIN_OVERFLOW;
    if (execution->pin_depth != 0) {
      if (!pthread_equal(execution->pinned_carrier, pthread_self()))
        return LUA_PIN_INVALID;
    }
    else {
      if (execution->pin_carrier == NULL || execution->unpin_carrier == NULL ||
          !execution->pin_carrier(execution)) return LUA_PIN_UNAVAILABLE;
      execution->pinned_carrier = pthread_self();
    }
    execution->pin_depth++;
  }
  pin->execution = execution;
  pin->carrier = pthread_self();
  pin->active = 1;
  return LUA_OK;
}

int luaQ_pinowner (const LuaCarrierPin *pin) {
  return pin->active && pin->execution == luaQ_current() &&
         pthread_equal(pin->carrier, pthread_self());
}

int luaQ_unpincarrier (LuaCarrierPin *pin) {
  LuaExecution *execution = luaQ_current();
  if (!luaQ_pinowner(pin)) return LUA_PIN_INVALID;
  if (execution != NULL) {
    lua_assert(execution->pin_depth != 0);
    checkcarrier(execution);
    if (--execution->pin_depth == 0) execution->unpin_carrier(execution);
  }
  pin->active = 0;
  return LUA_OK;
}

void luaQ_entermanaged (lua_State *L) {
  LuaExecution *execution = luaQ_current();
  if (execution != NULL) {
    checkcarrier(execution);
    /* The callback may park before any state gate/world participation exists. */
    if (execution->managed_depth == 0 && execution->managed_enter != NULL)
      execution->managed_enter(execution, L);
    checkcarrier(execution);
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
    checkcarrier(execution);
    /* Admission runs before the foreign activation exists. */
    if (execution->native_enter != NULL)
      execution->native_enter(execution, L);
    checkcarrier(execution);
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
  if (execution != NULL) {
    checkcarrier(execution);
    if (execution->poll != NULL) execution->poll(execution, L);
    checkcarrier(execution);
  }
}

void luaQ_work (lua_State *L, unsigned *work) {
  luaQ_workn(L, work, 1);
}

void luaQ_workn (lua_State *L, unsigned *work, size_t amount) {
  while (amount >= 1024u - *work) {
    amount -= 1024u - *work;
    *work = 0;
    lua_lock(L);
    luaX_poll(L);
    lua_unlock(L);
  }
  *work += cast_uint(amount);
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

void *luaQ_ownertoken (void) {
  LuaExecution *execution = luaQ_current();
  return execution != NULL ? cast(void *, execution)
                           : cast(void *, &luaE_native_stack_context);
}

/* The caller has published roots and withdrawn collection participation where
** necessary. Registration and wake selection share the predicate's mutex.
** The wait callback handles notification before outgoing context publication.
** Reacquire execution capacity without retaining the predicate mutex. */
static void linkwaiter (LuaWaiter *waiter, LuaExecution *execution,
                        LuaWaiter **waiters) {
  waiter->execution = execution;
  waiter->next = *waiters;
  waiter->link = waiters;
  if (waiter->next != NULL) waiter->next->link = &waiter->next;
  *waiters = waiter;
}

static void unlinkwaiter (LuaWaiter *waiter) {
  if (waiter->link != NULL) {
    *waiter->link = waiter->next;
    if (waiter->next != NULL) waiter->next->link = waiter->link;
    waiter->link = NULL;
  }
}

static void waitregistered (lua_State *L, pthread_mutex_t *mutex,
                            pthread_cond_t *changed, LuaWaiter *waiter) {
  LuaExecution *execution = luaQ_current();
  if (waiter->execution != NULL) {
    unsigned depth = execution->managed_depth;
    execution->managed_depth = 0;
    if (depth != 0 && execution->managed_leave != NULL)
      execution->managed_leave(execution);
    execution->wait(execution, mutex, waiter);
    luaQ_entermanaged(L);
    if (depth == 0)
      luaQ_leavemanaged();  /* An opaque native resource needs no VM permit. */
    else
      execution->managed_depth = depth;
    luaX_checksync(pthread_mutex_lock(mutex));
  }
  else {
    while (waiter->link != NULL)
      luaX_checksync(pthread_cond_wait(changed, mutex));
  }
  lua_assert(waiter->link == NULL);
}

void luaQ_wait (lua_State *L, pthread_mutex_t *mutex, pthread_cond_t *changed,
                 LuaWaiter **waiters) {
  LuaExecution *execution = luaQ_current();
  LuaWaiter waiter;
  linkwaiter(&waiter,
    execution != NULL && execution->wait != NULL && execution->wake != NULL
      ? execution : NULL, waiters);
  if (waiter.execution != NULL)
    waitregistered(L, mutex, changed, &waiter);
  else {
    luaX_checksync(pthread_cond_wait(changed, mutex));
    unlinkwaiter(&waiter);  /* also retire after a spurious/direct POSIX signal */
  }
}

int luaQ_waitstop (lua_State *L, pthread_mutex_t *mutex, pthread_cond_t *changed,
                    LuaWaiter **waiters) {
  LuaExecution *execution = luaQ_current();
  LuaTaskRoot *task;
  LuaWorld *world;
  LuaStopWait wait;
  int same, eligible, stopped;
  if (execution == NULL) goto ordinary;
  task = __atomic_load_n(&execution->task, __ATOMIC_ACQUIRE);
  if (task == NULL || task->global != G(L)) goto ordinary;
  world = &task->global->world;
  same = mutex == &world->mutex;
  if (!same) luaX_checksync(pthread_mutex_lock(&world->mutex));
  eligible = L->close_active == 0 &&
    ((task->phase == LUA_TASK_RUNNING && L->errorJmp != NULL &&
      L->errorJmp == execution->protected) ||
     (task->phase == LUA_TASK_ADMITTED && task->thread == L &&
      execution->protected == NULL && execution->native_depth == 0 &&
      execution->managed_depth == 0 &&
      luaE_getnativestackcontext()->coroutine_scopes == NULL));
  if (!eligible) {
    if (!same) luaX_checksync(pthread_mutex_unlock(&world->mutex));
    goto ordinary;
  }
  if (__atomic_load_n(&task->stop_requested, __ATOMIC_ACQUIRE)) {
    if (!same) luaX_checksync(pthread_mutex_unlock(&world->mutex));
    return 1;
  }
  lua_assert(task->stop_wait == NULL);
  wait.mutex = mutex;
  wait.changed = changed;
  wait.observers = 0;
  task->stop_wait = &wait;
  linkwaiter(&wait.waiter,
    execution->wait != NULL && execution->wake != NULL ? execution : NULL,
    waiters);
  if (!same) luaX_checksync(pthread_mutex_unlock(&world->mutex));
  waitregistered(L, mutex, changed, &wait.waiter);
  if (!same) luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(task->stop_wait == &wait);
  task->stop_wait = NULL;
  if (!same) {
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
    luaX_checksync(pthread_mutex_unlock(mutex));
    luaX_checksync(pthread_mutex_lock(&world->mutex));
  }
  /* Requesters own a borrow, never a world-to-resource lock nesting. The
  ** resource lease remains live until the last borrower has retired. */
  while (wait.observers != 0)
    luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
  stopped = __atomic_load_n(&task->stop_requested, __ATOMIC_ACQUIRE);
  if (!same) {
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
    luaX_checksync(pthread_mutex_lock(mutex));
  }
  return stopped;
 ordinary:
  luaQ_wait(L, mutex, changed, waiters);
  return 0;
}

void luaQ_interrupt (LuaStopWait *wait) {
  LuaWaiter *waiter = &wait->waiter;
  luaX_checksync(pthread_mutex_lock(wait->mutex));
  if (waiter->link != NULL) {
    unlinkwaiter(waiter);
    luaX_checksync(pthread_cond_broadcast(wait->changed));
    if (waiter->execution != NULL)
      waiter->execution->wake(waiter->execution, waiter);
  }
  luaX_checksync(pthread_mutex_unlock(wait->mutex));
}

void luaQ_wake (pthread_cond_t *changed, LuaWaiter **waiters) {
  LuaWaiter *waiter = *waiters;
  if (waiter == NULL) return;
  *waiters = NULL;
  luaX_checksync(pthread_cond_broadcast(changed));
  while (waiter != NULL) {
    LuaWaiter *next = waiter->next;
    waiter->link = NULL;
    if (waiter->execution != NULL)
      waiter->execution->wake(waiter->execution, waiter);
    waiter = next;
  }
}
