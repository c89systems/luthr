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
#include "lmem.h"
#include "lstate.h"
#include "ltask.h"
#include "ltablestorage.h"
#include "lscheduler.h"

void luaX_checksync (int status) {
  if (status != 0) {
    fprintf(stderr, "Luthr internal synchronization failure: %d\n", status);
    abort();
  }
}

typedef struct LuaGateWaiter {
  struct LuaGateWaiter *next;
} LuaGateWaiter;

void luaX_initgate (LuaStackGate *gate) {
  gate->owner.token = NULL;
  gate->context = NULL;
  gate->host = NULL;
  gate->head = NULL;
  gate->tail = &gate->head;
  gate->status = 0;
  gate->ready = 1;
}

void luaX_destroygate (LuaStackGate *gate) {
  if (!gate->ready)
    return;
  lua_assert(gate->owner.token == NULL && gate->status == 0 &&
             gate->head == NULL && gate->tail == &gate->head &&
             gate->context == NULL && gate->host == NULL);
  gate->ready = 0;
}

static void bindcontext (LuaStackGate *gate, NativeStackContext *context,
                         LuaThreadContext *thread, void *token) {
  gate->context = context;
  gate->host = context->execution == NULL ? thread : NULL;
  __atomic_store_n(&gate->owner.token, token, __ATOMIC_RELEASE);
}

static void reserve (lua_State *L, NativeStackContext *context,
                      LuaThreadContext *thread, void *token, unsigned active) {
  LuaStackGate *gate = &L->stack_gate;
  unsigned expected = 0;
  unsigned desired = LUA_GATE_HELD | active;
  /* Recursive entry is an internal boundary error, not a wait for ourselves. */
  if (__atomic_load_n(&gate->owner.token, __ATOMIC_ACQUIRE) == token) {
    fputs("Luthr recursive coroutine stack gate entry\n", stderr);
    abort();
  }
  if (!__atomic_compare_exchange_n(&gate->status, &expected, desired, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
    LuaWorld *world = &G(L)->world;
    LuaGateWaiter wait = {NULL};
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    *gate->tail = &wait;
    gate->tail = &wait.next;
    /* The release CAS preserves this bit. Either it sees our registration
    ** and wakes the world queue, or our recheck sees an already free gate. */
    __atomic_fetch_or(&gate->status, LUA_GATE_WAITING, __ATOMIC_SEQ_CST);
    for (;;) {
      expected = LUA_GATE_WAITING;
      if (gate->head == &wait &&
          __atomic_compare_exchange_n(&gate->status, &expected,
            desired | (wait.next != NULL ? LUA_GATE_WAITING : 0), 0,
            __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) break;
      luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
    }
    gate->head = wait.next;
    if (gate->head == NULL) gate->tail = &gate->head;
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
  }
  lua_assert(__atomic_load_n(&gate->owner.token, __ATOMIC_RELAXED) == NULL);
  bindcontext(gate, context, thread, token);
}

static void release (lua_State *L, void *token) {
  LuaStackGate *gate = &L->stack_gate;
  LuaWorld *world = &G(L)->world;
  unsigned previous;
  lua_assert(L->world_depth == 0);
  if (__atomic_load_n(&gate->owner.token, __ATOMIC_ACQUIRE) != token) {
    fputs("Luthr coroutine stack gate released by a non-owner\n", stderr);
    abort();
  }
  gate->context = NULL;
  gate->host = NULL;
  __atomic_store_n(&gate->owner.token, NULL, __ATOMIC_RELEASE);
  previous = __atomic_load_n(&gate->status, __ATOMIC_RELAXED);
  lua_assert(previous & LUA_GATE_HELD);
  /* Stack release and participation retirement are one publication. Only
  ** waiter registration can change this word while we own it. After success
  ** neither this gate nor L is touched: a collector can reclaim it at once.
  ** A queued gate retains WAITING so a fresh caller cannot steal the grant. */
  while (!__atomic_compare_exchange_n(&gate->status, &previous,
           previous & LUA_GATE_WAITING, 0,
           __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) { }
  luaW_leave(world, previous & LUA_GATE_WAITING);
}

int luaX_tryreserve (lua_State *target) {
  LuaStackGate *gate = &target->stack_gate;
  LuaThreadContext *thread = &luaE_thread_context;
  NativeStackContext *context = thread->override != NULL
                                 ? thread->override : &thread->native;
  void *token = context->execution != NULL ? cast(void *, context->execution)
                                          : cast(void *, &thread->native);
  unsigned expected = 0;
  if (__atomic_load_n(&gate->owner.token, __ATOMIC_ACQUIRE) == token) return 2;
  if (!__atomic_compare_exchange_n(&gate->status, &expected, LUA_GATE_HELD, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return 0;
  lua_assert(__atomic_load_n(&gate->owner.token, __ATOMIC_RELAXED) == NULL);
  bindcontext(gate, context, thread, token);
  return 1;
}

void luaX_endreserve (lua_State *target, int reservation) {
  if (reservation == 1) release(target, luaQ_ownertoken());
}

static void lockstate (lua_State *L, LuaThreadContext *thread,
                       NativeStackContext *context, int values) {
  LuaExecution *execution = context->execution;
  LuaThreadContext *host = execution == NULL ? thread : NULL;
  void *token = execution != NULL ? cast(void *, execution)
                                  : cast(void *, &thread->native);
  luaQ_entermanaged(L, execution);
  reserve(L, context, host, token, LUA_GATE_ACTIVE);
  luaW_enter(L);
  if (!values) {
    luaCP_apply(L);
    if (L->errorJmp == NULL && execution != NULL)
      luaE_refreshnativestack(L);
  }
}

void luaX_lockstate (lua_State *L) {
  LuaThreadContext *thread = &luaE_thread_context;
  NativeStackContext *context = thread->override != NULL
                                 ? thread->override : &thread->native;
  lockstate(L, thread, context, 0);
}

static void unlockstate (lua_State *L, void *token, int values) {
  LuaExecution *execution = luaX_execution(L);
  /* Pure value operations cannot create reclamation work. Work inherited
  ** from a collector snapshot remains serviceable by the ordinary workers
  ** and the next allocating/calling API, instead of extending every read. */
  if (!values) {
    if (!L->reclaiming && L->storage_retired != NULL) luaHS_reclaim(L, LUAHS_WITHDRAW);
    if (!L->reclaiming && (luaC_retired(L) != NULL || L->reclaim_pages))
      luaC_reclaim(L);
  }
  if (!values || L->allocation_debt != 0 || L->reclaim_credit != 0)
    luaM_publishdebt(L);
  if (L->object_scopes == NULL &&
      __atomic_load_n(&G(L)->barrier_ready, __ATOMIC_ACQUIRE) != 0)
    luaSC_notifygc(G(L));
  lua_assert(L->admission == &L->participation);
  /* Runtime stack descriptors can be temporary callback storage. Ordinary
  ** embedding callers retain lazy per-thread discovery at their VM boundary. */
  if (L->errorJmp == NULL && execution != NULL) {
    L->luthr_stack_context = NULL;
    L->luthr_stack_limit = 0;
  }
  /* Keep participation until all physical gate accesses have finished. A
  ** collector may reclaim this state after the final leave; do not touch it. */
  release(L, token);
  luaQ_leavemanaged(execution);  /* no state accesses after the final world leave */
}

void luaX_unlockstate (lua_State *L) {
  unlockstate(L, luaQ_ownertoken(), 0);
}

int luaX_tryworker (lua_State *L) {
  LuaExecution *execution = luaQ_current();
  int reservation;
  lua_assert(L->participation.collector && execution != NULL &&
             execution->managed_depth == 0 && execution->task == NULL);
  luaQ_entermanaged(L, execution);
  reservation = luaX_tryreserve(L);
  if (reservation == 1) {
    if (luaW_tryenter(L)) return 1;
    luaX_endreserve(L, reservation);
  }
  luaQ_leavemanaged(execution);
  return 0;
}

static int attachedapi (lua_State *L, NativeStackContext *context) {
  LuaApiRegion *region = context->api_region;
  if (region == NULL) return 0;
  if (region->state != L || L->stack_gate.context != context) {
    fputs("Luthr API crossed an attached runtime activation\n", stderr);
    abort();
  }
  return 1;
}

static void apienter (lua_State *L, int values) {
  LuaThreadContext *thread = &luaE_thread_context;
  NativeStackContext *context = thread->override != NULL
                                 ? thread->override : &thread->native;
  if (!attachedapi(L, context)) lockstate(L, thread, context, values);
  else luaX_pollrequests(L);
}

static void apileave (lua_State *L, int values) {
  LuaThreadContext *thread = &luaE_thread_context;
  NativeStackContext *context = thread->override != NULL
                                 ? thread->override : &thread->native;
  if (!attachedapi(L, context)) {
    void *token = context->execution != NULL ? cast(void *, context->execution)
                                            : cast(void *, &thread->native);
    unlockstate(L, token, values);
  }
}

void luaX_apienter (lua_State *L) { apienter(L, 0); }

void luaX_apileave (lua_State *L) { apileave(L, 0); }

void luaX_valueenter (lua_State *L) { apienter(L, 1); }

void luaX_valueleave (lua_State *L) { apileave(L, 1); }

void luaX_valuecall (lua_State *L) {
  if (attachedapi(L, luaX_context(L))) return;
  luaCP_apply(L);
  if (L->errorJmp == NULL && luaX_execution(L) != NULL)
    luaE_refreshnativestack(L);
}

/* Reserve both stacks before joining managed execution. A blocked reservation
** must not count as a mutator or prevent the other owner from collecting.
** No managed stack/CI pointer is borrowed until both reservations are ready. */
void luaX_lockpair (lua_State *first, lua_State *second) {
  LuaThreadContext *thread = &luaE_thread_context;
  NativeStackContext *context = thread->override != NULL
                                 ? thread->override : &thread->native;
  LuaExecution *execution = context->execution;
  LuaThreadContext *host = execution == NULL ? thread : NULL;
  void *token = execution != NULL ? cast(void *, execution)
                                  : cast(void *, &thread->native);
  luaQ_entermanaged(first, execution);
  if (cast(uintptr_t, first) > cast(uintptr_t, second)) {
    lua_State *swap = first;
    first = second;
    second = swap;
  }
  /* Never publish participation while waiting for the second gate. A
  ** collector holding it must be able to complete its rendezvous. */
  reserve(first, context, host, token, 0);
  if (second != first) reserve(second, context, host, token, 0);
  lua_assert(first->admission == &first->participation &&
             second->admission == &second->participation);
  lua_assert(first->stack_peer == NULL && second->stack_peer == NULL);
  if (second != first) {
    first->stack_peer = second;
    second->stack_peer = first;
  }
  second->admission = &first->participation;
  luaW_activate(first);
  luaW_enter(first);
  if (second != first) luaC_checksnapshot(second);
  luaCP_apply(first);
  if (second != first) luaCP_apply(second);
  if (first->errorJmp == NULL && execution != NULL)
    luaE_refreshnativestack(first);
  if (second != first && second->errorJmp == NULL && execution != NULL)
    luaE_refreshnativestack(second);
}

void luaX_unlockpair (lua_State *first, lua_State *second) {
  LuaExecution *execution = luaX_execution(first);
  void *token = luaQ_ownertoken();
  if (cast(uintptr_t, first) > cast(uintptr_t, second)) {
    lua_State *swap = first;
    first = second;
    second = swap;
  }
  luaM_publishdebt(first);
  first->stack_peer = second->stack_peer = NULL;
  second->admission = &second->participation;
  if (first->errorJmp == NULL && execution != NULL) {
    first->luthr_stack_context = NULL;
    first->luthr_stack_limit = 0;
  }
  if (second != first && second->errorJmp == NULL && execution != NULL) {
    second->luthr_stack_context = NULL;
    second->luthr_stack_limit = 0;
  }
  if (second != first) release(second, token);
  release(first, token);
  luaQ_leavemanaged(execution);
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
  if (L->object_scopes == NULL &&
      __atomic_load_n(&G(L)->barrier_ready, __ATOMIC_ACQUIRE) != 0)
    luaSC_notifygc(G(L));
  if (__atomic_load_n(&G(L)->world.stopped, __ATOMIC_SEQ_CST) ||
      (__atomic_load_n(&L->stack_gate.status, __ATOMIC_ACQUIRE) &
        LUA_GATE_WAITING) != 0) {
    /* A real request publishes roots and hands off ownership. The successful
    ** no-request path above reads request words without a gate transaction. */
    L->native_depth++;
    luaX_unlockstate(L);
    luaX_lockstate(L);
    L->native_depth--;
  }
}

void luaX_poll (lua_State *L) {
  luaHS_quiescent(L);
  luaHS_reclaim(L, LUAHS_ASSIST);
  luaQ_poll(L);
  luaC_trimstack(L);
  /* Internal poll handlers execute as managed VM code, with stack ownership.
  ** They are not ordinary C bindings and do not detach automatically. */
  if (L->state_poll != NULL)
    L->state_poll(L, L->state_poll_context);
  luaX_pollrequests(L);
  /* Complete the existing task handoff before requesting a GC phase change. */
  if (luaC_tracing(L)) luaC_polltrace(L);
  else if (luaC_sweeping(L)) luaC_pollsweep(L);
  luaTK_checkstop(L);
  luaCP_checklimit(L);
}
