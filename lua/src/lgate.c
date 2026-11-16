/*
** Per-coroutine stack ownership and VM safepoints.
** See Copyright Notice in lua.h
*/
#define lgate_c
#define LUA_CORE
#include "lprefix.h"

#include <stdio.h>
#include <stdlib.h>
#include "lgate.h"
#include "ldo.h"
#include "lstate.h"
#include "ltask.h"

void luaX_checksync (int status) {
  if (status != 0) {
    fprintf(stderr, "Luthr internal synchronization failure: %d\n", status);
    abort();
  }
}

int luaX_initgate (LuaStackGate *gate) {
  int status;
  gate->ready = gate->owned = 0;
  gate->next_ticket = gate->serving = 0;
  gate->waiters = NULL;
  status = pthread_mutex_init(&gate->mutex, NULL);
  if (status != 0)
    return status;
  status = pthread_cond_init(&gate->changed, NULL);
  if (status != 0) {
    luaX_checksync(pthread_mutex_destroy(&gate->mutex));
    return status;
  }
  gate->ready = 1;
  return 0;
}

void luaX_destroygate (LuaStackGate *gate) {
  if (!gate->ready)
    return;
  lua_assert(!gate->owned && gate->next_ticket == gate->serving &&
             gate->waiters == NULL);
  luaX_checksync(pthread_cond_destroy(&gate->changed));
  luaX_checksync(pthread_mutex_destroy(&gate->mutex));
  gate->ready = 0;
}

static void reserve (lua_State *L) {
  LuaStackGate *gate = &L->stack_gate;
  unsigned long ticket;
  luaX_checksync(pthread_mutex_lock(&gate->mutex));
  /* Recursive entry is an internal boundary error, not a wait for ourselves. */
  if (gate->owned && luaQ_isowner(&gate->owner)) {
    fputs("Luthr recursive coroutine stack gate entry\n", stderr);
    abort();
  }
  ticket = gate->next_ticket++;
  while (ticket != gate->serving)
    luaQ_wait(L, &gate->mutex, &gate->changed, &gate->waiters);
  lua_assert(!gate->owned);
  luaQ_setowner(&gate->owner);
  gate->owned = 1;
  luaX_checksync(pthread_mutex_unlock(&gate->mutex));
}

static void release (lua_State *L) {
  LuaStackGate *gate = &L->stack_gate;
  lua_assert(L->world_depth == 0);
  luaX_checksync(pthread_mutex_lock(&gate->mutex));
  if (!gate->owned || !luaQ_isowner(&gate->owner)) {
    fputs("Luthr coroutine stack gate released by a non-owner\n", stderr);
    abort();
  }
  gate->owned = 0;
  gate->serving++;
  luaQ_wake(&gate->changed, &gate->waiters);
  luaX_checksync(pthread_mutex_unlock(&gate->mutex));
}

void luaX_lockstate (lua_State *L) {
  luaQ_entermanaged(L);
  reserve(L);
  luaW_enter(L);
  if (L->errorJmp == NULL && luaQ_current() != NULL)
    luaE_refreshnativestack(L);
}

void luaX_unlockstate (lua_State *L) {
  /* Runtime stack descriptors can be temporary callback storage. Ordinary
  ** embedding callers retain lazy per-thread discovery at their VM boundary. */
  if (L->errorJmp == NULL && luaQ_current() != NULL) {
    L->luthr_stack_context = NULL;
    L->luthr_stack_limit = 0;
  }
  /* Keep participation until all physical gate accesses have finished. A
  ** collector may reclaim this state after the final leave; do not touch it. */
  release(L);
  luaW_leave(L);
  luaQ_leavemanaged();  /* no state accesses after the final world leave */
}

/* Reserve both stacks before joining managed execution. A blocked reservation
** must not count as a mutator or prevent the other owner from collecting.
** No managed stack/CI pointer is borrowed until both reservations are ready. */
void luaX_lockpair (lua_State *first, lua_State *second) {
  luaQ_entermanaged(first);
  if (cast(uintptr_t, first) > cast(uintptr_t, second)) {
    lua_State *swap = first;
    first = second;
    second = swap;
  }
  reserve(first);
  if (second != first) reserve(second);
  luaW_enter(first);
  if (first->errorJmp == NULL && luaQ_current() != NULL)
    luaE_refreshnativestack(first);
  if (second != first && second->errorJmp == NULL && luaQ_current() != NULL)
    luaE_refreshnativestack(second);
}

void luaX_unlockpair (lua_State *first, lua_State *second) {
  if (cast(uintptr_t, first) > cast(uintptr_t, second)) {
    lua_State *swap = first;
    first = second;
    second = swap;
  }
  if (first->errorJmp == NULL && luaQ_current() != NULL) {
    first->luthr_stack_context = NULL;
    first->luthr_stack_limit = 0;
  }
  if (second != first && second->errorJmp == NULL && luaQ_current() != NULL) {
    second->luthr_stack_context = NULL;
    second->luthr_stack_limit = 0;
  }
  if (second != first) release(second);
  release(first);
  luaW_leave(first);
  luaQ_leavemanaged();
}

l_uint32 luaX_parentcalls (lua_State *from) {
  l_uint32 calls = 0;
  if (from != NULL) {
    luaX_lockstate(from);
    calls = getCcalls(from);
    luaX_unlockstate(from);
  }
  return calls;
}

void luaX_poll (lua_State *L) {
  LuaWorldScope depth;
  luaQ_poll(L);
  /* Internal poll handlers execute as managed VM code, with stack ownership.
  ** They are not ordinary C bindings and do not detach automatically. */
  if (L->state_poll != NULL)
    L->state_poll(L, L->state_poll_context);
  depth = luaW_detach(L);
  luaW_attach(L, depth);
  luaTK_checkstop(L);
}
