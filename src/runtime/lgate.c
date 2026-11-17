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
#include "lgc.h"
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
  gate->ready = 0;
  gate->owner.token = NULL;
  gate->next_ticket = gate->serving = 0;
  gate->waiting = 0;
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
  lua_assert(gate->owner.token == NULL && gate->next_ticket == gate->serving &&
             gate->waiters == NULL);
  luaX_checksync(pthread_cond_destroy(&gate->changed));
  luaX_checksync(pthread_mutex_destroy(&gate->mutex));
  gate->ready = 0;
}

static void reserve (lua_State *L) {
  LuaStackGate *gate = &L->stack_gate;
  void *token = luaQ_ownertoken();
  unsigned long ticket;
  /* Recursive entry is an internal boundary error, not a wait for ourselves. */
  if (__atomic_load_n(&gate->owner.token, __ATOMIC_ACQUIRE) == token) {
    fputs("Luthr recursive coroutine stack gate entry\n", stderr);
    abort();
  }
  ticket = __atomic_fetch_add(&gate->next_ticket, 1, __ATOMIC_RELAXED);
  if (ticket != __atomic_load_n(&gate->serving, __ATOMIC_ACQUIRE)) {
    luaX_checksync(pthread_mutex_lock(&gate->mutex));
    __atomic_fetch_add(&gate->waiting, 1, __ATOMIC_SEQ_CST);
    while (ticket != __atomic_load_n(&gate->serving, __ATOMIC_SEQ_CST)) {
      luaQ_wait(L, &gate->mutex, &gate->changed, &gate->waiters);
    }
    __atomic_fetch_sub(&gate->waiting, 1, __ATOMIC_SEQ_CST);
    luaX_checksync(pthread_mutex_unlock(&gate->mutex));
  }
  lua_assert(__atomic_load_n(&gate->owner.token, __ATOMIC_RELAXED) == NULL);
  __atomic_store_n(&gate->owner.token, token, __ATOMIC_RELEASE);
}

static void release (lua_State *L) {
  LuaStackGate *gate = &L->stack_gate;
  lua_assert(L->world_depth == 0);
  if (!luaQ_isowner(&gate->owner)) {
    fputs("Luthr coroutine stack gate released by a non-owner\n", stderr);
    abort();
  }
  __atomic_store_n(&gate->owner.token, NULL, __ATOMIC_RELEASE);
  /* Either a waiter sees its turn before parking, or this observer check
  ** sees its registration and wakes it. Uncontended entry/exit stays local. */
  __atomic_fetch_add(&gate->serving, 1, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(&gate->waiting, __ATOMIC_SEQ_CST) != 0) {
    luaX_checksync(pthread_mutex_lock(&gate->mutex));
    luaQ_wake(&gate->changed, &gate->waiters);
    luaX_checksync(pthread_mutex_unlock(&gate->mutex));
  }
}

int luaX_tryreserve (lua_State *target) {
  LuaStackGate *gate = &target->stack_gate;
  unsigned long turn;
  if (luaQ_isowner(&gate->owner)) return 2;
  turn = __atomic_load_n(&gate->serving, __ATOMIC_ACQUIRE);
  if (!__atomic_compare_exchange_n(&gate->next_ticket, &turn, turn + 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return 0;
  lua_assert(__atomic_load_n(&gate->owner.token, __ATOMIC_RELAXED) == NULL);
  luaQ_setowner(&gate->owner);
  return 1;
}

void luaX_endreserve (lua_State *target, int reservation) {
  if (reservation == 1) release(target);
}

void luaX_lockstate (lua_State *L) {
  luaQ_entermanaged(L);
  reserve(L);
  luaW_enter(L);
  luaCP_apply(L);
  if (L->errorJmp == NULL && luaQ_current() != NULL)
    luaE_refreshnativestack(L);
}

void luaX_unlockstate (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  LuaParticipation *participant = L->admission;
  if (!L->reclaiming && (luaC_retired(L) != NULL || L->reclaim_pages))
    luaC_reclaim(L);
  lua_assert(participant == &L->participation);  /* paired operations retire together */
  /* Runtime stack descriptors can be temporary callback storage. Ordinary
  ** embedding callers retain lazy per-thread discovery at their VM boundary. */
  if (L->errorJmp == NULL && luaQ_current() != NULL) {
    L->luthr_stack_context = NULL;
    L->luthr_stack_limit = 0;
  }
  /* Keep participation until all physical gate accesses have finished. A
  ** collector may reclaim this state after the final leave; do not touch it. */
  release(L);
  luaW_leave(world, participant);
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
  lua_assert(first->admission == &first->participation &&
             second->admission == &second->participation);
  lua_assert(first->stack_peer == NULL && second->stack_peer == NULL);
  if (second != first) {
    first->stack_peer = second;
    second->stack_peer = first;
  }
  second->admission = &first->participation;
  luaW_enter(first);
  if (second != first) luaC_checksnapshot(second);
  luaCP_apply(first);
  if (second != first) luaCP_apply(second);
  if (first->errorJmp == NULL && luaQ_current() != NULL)
    luaE_refreshnativestack(first);
  if (second != first && second->errorJmp == NULL && luaQ_current() != NULL)
    luaE_refreshnativestack(second);
}

void luaX_unlockpair (lua_State *first, lua_State *second) {
  LuaWorld *world = &G(first)->world;
  LuaParticipation *participant;
  if (cast(uintptr_t, first) > cast(uintptr_t, second)) {
    lua_State *swap = first;
    first = second;
    second = swap;
  }
  participant = first->admission;
  first->stack_peer = second->stack_peer = NULL;
  second->admission = &second->participation;
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
  luaW_leave(world, participant);
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

void luaX_pollrequests (lua_State *L) {
  if (__atomic_load_n(&G(L)->world.stopped, __ATOMIC_SEQ_CST) ||
      __atomic_load_n(&L->stack_gate.waiting, __ATOMIC_ACQUIRE) != 0) {
    /* A real request publishes roots and hands off ownership. The successful
    ** no-request path above reads request words without a gate transaction. */
    L->native_depth++;
    luaX_unlockstate(L);
    luaX_lockstate(L);
    L->native_depth--;
  }
}

void luaX_poll (lua_State *L) {
  luaQ_poll(L);
  luaC_trimstack(L);
  /* Internal poll handlers execute as managed VM code, with stack ownership.
  ** They are not ordinary C bindings and do not detach automatically. */
  if (L->state_poll != NULL)
    L->state_poll(L, L->state_poll_context);
  luaX_pollrequests(L);
  /* Complete the existing task handoff before requesting a GC phase change. */
  if (luaC_tracing(L)) luaC_polltrace(L);
  luaTK_checkstop(L);
  luaCP_checklimit(L);
}
