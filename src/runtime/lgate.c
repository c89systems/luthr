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

_Static_assert(_Alignof(LuaExecution) > LUA_GATE_FLAGS &&
               _Alignof(NativeStackContext) > LUA_GATE_FLAGS,
               "stack gate owner tokens must leave space for the flags");
_Static_assert(offsetof(LuaThreadContext, native) == 0,
               "host token must point to the beginning of its TLS record");

void luaX_initgate (LuaStackGate *gate) {
  gate->word = 0;
  gate->context = NULL;
  gate->head = NULL;
  gate->tail = &gate->head;
  gate->ready = 1;
}

void luaX_destroygate (LuaStackGate *gate) {
  if (!gate->ready)
    return;
  lua_assert(gate->word == 0 && gate->head == NULL &&
             gate->tail == &gate->head && gate->context == NULL);
  gate->ready = 0;
}

static void reserve (lua_State *L, NativeStackContext *context,
                      void *token, uintptr_t active) {
  LuaStackGate *gate = &L->stack_gate;
  uintptr_t expected = 0;
  uintptr_t desired = cast(uintptr_t, token) | active;
  if (!__atomic_compare_exchange_n(&gate->word, &expected, desired, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
    LuaWorld *world = &G(L)->world;
    LuaGateWaiter wait = {NULL};
    /* Recursive entry is a boundary error. Waiting would deadlock. */
    if (luaX_gateowner(expected) == token) {
      fputs("Luthr recursive coroutine stack gate entry\n", stderr);
      abort();
    }
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    *gate->tail = &wait;
    gate->tail = &wait.next;
    /* Release preserves this bit. Either it sees our registration
    ** and wakes the world queue, or our recheck sees an already free gate. */
    __atomic_fetch_or(&gate->word, LUA_GATE_WAITING, __ATOMIC_SEQ_CST);
    for (;;) {
      expected = LUA_GATE_WAITING;
      if (gate->head == &wait &&
          __atomic_compare_exchange_n(&gate->word, &expected,
            desired | (wait.next != NULL ? LUA_GATE_WAITING : 0), 0,
            __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) break;
      luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
    }
    gate->head = wait.next;
    if (gate->head == NULL) gate->tail = &gate->head;
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
  }
  gate->context = context;
}

l_sinline void leavegate (lua_State *L) {
  global_State *g = G(L);
  uintptr_t stack = cast(uintptr_t, L);
  /* Retire ownership and participation together. After publication a
  ** collector can reclaim L; only the world directory is still accessible. */
  uintptr_t previous = __atomic_fetch_and(&L->stack_gate.word, LUA_GATE_WAITING,
                                           __ATOMIC_SEQ_CST);
  luaW_leave(g, stack, previous & LUA_GATE_FLAGS);
}

static void release (lua_State *L, void *token) {
  LuaStackGate *gate = &L->stack_gate;
  lua_assert(L->world_depth == 0);
  if (luaX_gateowner(__atomic_load_n(&gate->word, __ATOMIC_RELAXED)) != token) {
    fputs("Luthr coroutine stack gate released by a non-owner\n", stderr);
    abort();
  }
  gate->context = NULL;
  leavegate(L);
}

int luaX_tryreserve (lua_State *target) {
  LuaStackGate *gate = &target->stack_gate;
  LuaThreadContext *thread = &luaE_thread_context;
  NativeStackContext *context = thread->override != NULL
                                 ? thread->override : &thread->native;
  void *token = context->execution != NULL ? cast(void *, context->execution)
                                          : cast(void *, &thread->native);
  uintptr_t expected = 0;
  if (!__atomic_compare_exchange_n(&gate->word, &expected, cast(uintptr_t, token), 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return luaX_gateowner(expected) == token ? 2 : 0;
  gate->context = context;
  return 1;
}

void luaX_endreserve (lua_State *target, int reservation) {
  if (reservation == 1) release(target, luaQ_ownertoken());
}

static void preparecall (lua_State *L, LuaExecution *execution) {
  luaCP_apply(L);
  if (L->errorJmp == NULL && execution != NULL) luaE_refreshnativestack(L);
}

static void *lockstate (lua_State *L, LuaThreadContext *thread,
                        NativeStackContext *context) {
  LuaExecution *execution = context->execution;
  void *token = execution != NULL ? cast(void *, execution)
                                  : cast(void *, &thread->native);
  if (execution != NULL) luaQ_entermanaged(L, execution);
  reserve(L, context, token, LUA_GATE_MANAGED);
  luaW_enter(L);
  return token;
}

void luaX_lockstate (lua_State *L) {
  LuaThreadContext *thread = &luaE_thread_context;
  NativeStackContext *context = thread->override != NULL
                                 ? thread->override : &thread->native;
  lockstate(L, thread, context);
  preparecall(L, context->execution);
}

static void unlockstate (lua_State *L, void *token, int values) {
  LuaExecution *execution = L->stack_gate.context->execution;
  /* Pure value operations cannot create reclamation work. Work inherited
  ** from a collector snapshot remains serviceable by the ordinary workers
  ** and the next allocating/calling API, instead of extending every read. */
  if (!values) {
    if (!L->reclaiming && L->storage_retired != NULL) luaHS_reclaim(L, LUAHS_WITHDRAW);
    if (!L->reclaiming && (luaC_retired(L) != NULL || L->reclaim_pages))
      luaC_reclaim(L);
  }
  if (L->allocation_debt != 0 || L->reclaim_credit != 0 ||
      (!values && L->stack_peer != NULL))
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
  if (execution != NULL) luaQ_leavemanaged(execution);
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

/* Entry tokens carry the obligations of this scope on their low bits. */
enum { LUA_VALUE_STACK = 1, LUA_VALUE_TRACING = 2 };

/* Enter the storage epoch before borrowing any table buffers. A grace
** scan that still sees a stack-only gate covers only older retirements
** than the control word sampled after this promotion. */
static void managestack (lua_State *L, NativeStackContext *context) {
  LuaStackGate *gate = &L->stack_gate;
  /* The caller owns the stack-only gate. Change its mode in one
  ** transaction while preserving any concurrent waiter registration. */
  uintptr_t old = __atomic_fetch_xor(&gate->word, LUA_GATE_ACTIVE,
                                    __ATOMIC_SEQ_CST);
  lua_assert((old & LUA_GATE_ACTIVE) == LUA_GATE_STACK);
  UNUSED(old);
  gate->context = context;
  luaW_enter(L);
}

l_sinline uintptr_t apienter (lua_State *L, int stackonly) {
  LuaThreadContext *thread = &luaE_thread_context;
  NativeStackContext *context = thread->override != NULL
                                 ? thread->override : &thread->native;
  LuaExecution *execution = context->execution;
  if (attachedapi(L, context)) {
    luaX_pollrequests(L);
    return 0;
  }
  if (stackonly && (execution == NULL ||
      (execution->native_depth != 0 && execution->managed_depth == 0))) {
    uintptr_t owner = cast(uintptr_t, execution != NULL ? cast(void *, execution)
                                                       : cast(void *, &thread->native));
    uintptr_t expected = 0;
    if (__atomic_compare_exchange_n(&L->stack_gate.word, &expected,
          owner | LUA_GATE_STACK, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
      /* A stop either observes this active reservation or precedes the
      ** check below. A stack-only scope borrows no table storage. */
      if (l_likely(!luaW_stopped(&G(L)->world))) {
        if (l_unlikely(luaC_tracing(L))) {
          luaC_checksnapshot(L);
          return owner | LUA_VALUE_STACK | LUA_VALUE_TRACING;
        }
        return owner | LUA_VALUE_STACK;
      }
      if (execution == NULL) {  /* the host already owns the stack */
        managestack(L, context);
        return owner;
      }
      /* Managed capacity can park. Release before asking for a slot. */
      leavegate(L);
    }
  }
  return cast(uintptr_t, lockstate(L, thread, context));
}

void luaX_apienter (lua_State *L) {
  if (apienter(L, 0) != 0) preparecall(L, L->stack_gate.context->execution);
}

void luaX_apileave (lua_State *L) {
  LuaThreadContext *thread = &luaE_thread_context;
  NativeStackContext *context = thread->override != NULL
                                 ? thread->override : &thread->native;
  if (!attachedapi(L, context)) {
    void *token = context->execution != NULL ? cast(void *, context->execution)
                                            : cast(void *, &thread->native);
    unlockstate(L, token, 0);
  }
}

uintptr_t luaX_valueenter (lua_State *L, int stackonly) {
  return apienter(L, stackonly);
}

void luaX_valueleave (lua_State *L, uintptr_t owner) {
  if (owner & LUA_VALUE_STACK) {
    if ((owner & LUA_VALUE_TRACING) &&
        __atomic_load_n(&G(L)->barrier_ready, __ATOMIC_ACQUIRE) != 0)
      luaSC_notifygc(G(L));
    leavegate(L);
  }
  else if (owner != 0) unlockstate(L, cast(void *, owner), 1);
}

int luaX_valuecall (lua_State *L, uintptr_t owner) {
  if (owner & LUA_VALUE_STACK) {
    NativeStackContext *context = luaE_getnativestackcontext();
    if (context->execution != NULL) {
      luaX_valueleave(L, owner);
      return 0;
    }
    managestack(L, context);
  }
  if (owner != 0) preparecall(L, L->stack_gate.context->execution);
  return 1;
}

/* Reserve both stacks before joining managed execution. A blocked reservation
** must not count as a mutator or prevent the other owner from collecting.
** No managed stack/CI pointer is borrowed until both reservations are ready. */
void luaX_lockpair (lua_State *first, lua_State *second) {
  LuaThreadContext *thread = &luaE_thread_context;
  NativeStackContext *context = thread->override != NULL
                                 ? thread->override : &thread->native;
  LuaExecution *execution = context->execution;
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
  reserve(first, context, token, 0);
  if (second != first) reserve(second, context, token, 0);
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
  uint64_t control;
  if (L->object_scopes == NULL &&
      __atomic_load_n(&G(L)->barrier_ready, __ATOMIC_ACQUIRE) != 0)
    luaSC_notifygc(G(L));
  control = luaW_control(&G(L)->world);
  if ((control & LUA_WORLD_STOPPED) ||
      (__atomic_load_n(&L->stack_gate.word, __ATOMIC_ACQUIRE) &
        LUA_GATE_WAITING) != 0) {
    /* A real request publishes roots and hands off ownership. The successful
    ** no-request path above reads request words without a gate transaction. */
    L->native_depth++;
    luaX_unlockstate(L);
    luaX_lockstate(L);
    L->native_depth--;
  }
  else {
    if (l_unlikely(control & LUA_WORLD_ARM_TRACE)) luaW_acktrace(L);
    if (l_unlikely(luaC_scanready(G(L)) &&
        __atomic_load_n(&L->trace_snapshot, __ATOMIC_ACQUIRE) !=
          luaC_traceepoch(G(L)))) luaC_checksnapshot(L);
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
