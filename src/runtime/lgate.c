/*
** Per-coroutine stack ownership and VM safepoints.
** See Copyright Notice in lua.h
*/
#define lgate_c
#define LUA_CORE
#include "lprefix.h"

#include <stdio.h>
#include <stdlib.h>
#include "laccess.h"
#include "lgate.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lstate.h"
#include "lstackapi.h"
#include "ltask.h"
#include "ltablestorage.h"
#include "lscheduler.h"

/* Keep physical TLS private to entry points that resolve it on the current
** carrier. Inlining or IPA could preserve its address across fiber migration. */
#define carrierentry __attribute__((noipa))
static __thread LuaThreadContext thread_context;

carrierentry LuaThreadContext *luaE_threadcontext (void) {
  return &thread_context;
}

carrierentry NativeStackContext *luaE_getnativestackcontext (void) {
  return luaE_resolvestack(&thread_context);
}

carrierentry NativeStackContext *luaE_setnativestackcontext (
    NativeStackContext *context) {
  NativeStackContext *previous = thread_context.override;
  if (context != NULL && context->execution == NULL)
    context->host = &thread_context;
  thread_context.override = context;
  return previous;
}

void luaX_checksync (int status) {
  if (status != 0) {
    fprintf(stderr, "Luthr internal synchronization failure: %d\n", status);
    abort();
  }
}

typedef struct LuaGateWaiter {
  struct LuaGateWaiter *next;
} LuaGateWaiter;

_Static_assert(_Alignof(NativeStackContext) > LUA_GATE_FLAGS,
               "stack gate owner tokens must leave space for the flags");
_Static_assert(offsetof(LuaThreadContext, native) == 0,
               "host token must point to the beginning of its TLS record");

void luaX_initgate (LuaStackGate *gate, int dormant) {
  gate->word = dormant ? LUA_GATE_DORMANT : 0;
  gate->head = NULL;
  gate->tail = &gate->head;
  gate->ready = 1;
}

void luaX_destroygate (LuaStackGate *gate) {
  if (!gate->ready)
    return;
  lua_assert((gate->word == 0 || gate->word == LUA_GATE_DORMANT) &&
             gate->head == NULL &&
             gate->tail == &gate->head);
  gate->ready = 0;
}

static void reserve (lua_State *L, NativeStackContext *context,
                      uintptr_t active) {
  LuaStackGate *gate = &L->stack_gate;
  uintptr_t expected = 0;
  uintptr_t desired = cast(uintptr_t, context) | active;
  if (!__atomic_compare_exchange_n(&gate->word, &expected, desired, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
    LuaWorld *world = &G(L)->world;
    LuaGateWaiter wait = {NULL};
    /* Recursive entry is a boundary error. Waiting would deadlock. */
    if (luaX_gateowner(expected) == context) {
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
      uintptr_t dormant = __atomic_load_n(&gate->word, __ATOMIC_RELAXED) &
                          LUA_GATE_DORMANT;
      uintptr_t next = (desired & ~(dormant ? active : 0)) | dormant |
                       (wait.next != NULL ? LUA_GATE_WAITING : 0);
      expected = LUA_GATE_WAITING | dormant;
      if (gate->head == &wait &&
          __atomic_compare_exchange_n(&gate->word, &expected, next, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
        gate->head = wait.next;
        if (gate->head == NULL) gate->tail = &gate->head;
        if (dormant) {
          /* A stopped collector walks the stable directory without its
          ** mutex. Hold only an inactive gate until enrollment is legal. */
          luaW_enroll(L);
          __atomic_fetch_xor(&gate->word, dormant | active, __ATOMIC_SEQ_CST);
        }
        break;
      }
      luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
    }
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
  }
}

l_noret luaX_badrelease (void) {
  fputs("Luthr coroutine stack gate released without its reservation\n", stderr);
  abort();
}

static void release (lua_State *L, void *token, uintptr_t active) {
  lua_assert(L->world_depth == 0);
  luaX_releasegate(L, cast(uintptr_t, token) | active);
}

carrierentry int luaX_tryreserve (lua_State *target) {
  LuaStackGate *gate = &target->stack_gate;
  LuaThreadContext *thread = &thread_context;
  NativeStackContext *context = luaE_resolvestack(thread);
  uintptr_t expected = 0;
  uintptr_t owner = cast(uintptr_t, context);
  if (__atomic_compare_exchange_n(&gate->word, &expected, owner, 0,
                                   __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
  if (expected == LUA_GATE_DORMANT &&
      __atomic_compare_exchange_n(&gate->word, &expected, owner | LUA_GATE_DORMANT,
                                  0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
  return luaX_gateowner(expected) == context ? 2 : 0;
}

void luaX_endreserve (lua_State *target, int reservation) {
  if (reservation == 1) {
    /* A gate reserved before tentative admission can already be active. */
    uintptr_t word = __atomic_load_n(&target->stack_gate.word, __ATOMIC_RELAXED);
    release(target, luaQ_ownertoken(), word & LUA_GATE_MANAGED);
  }
}

static void preparecall (lua_State *L, NativeStackContext *context) {
  LuaPolicy *policy = luaX_contextpolicy(context);
  if (policy != NULL || L->policy != NULL) luaCP_apply(L, policy);
}

static void lockstate (lua_State *L, NativeStackContext *context) {
  LuaExecution *execution = context->execution;
  if (execution != NULL) luaQ_entermanaged(L, execution);
  reserve(L, context, LUA_GATE_MANAGED);
  luaW_enter(L);
}

carrierentry void luaX_lockstate (lua_State *L) {
  LuaThreadContext *thread = &thread_context;
  NativeStackContext *context = luaE_resolvestack(thread);
  lockstate(L, context);
  preparecall(L, context);
}

static void unlockstate (lua_State *L, NativeStackContext *context, int values) {
  LuaExecution *execution = context->execution;
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
  /* Keep participation until all physical gate accesses have finished. A
  ** collector may reclaim this state after the final leave; do not touch it. */
  release(L, context, LUA_GATE_MANAGED);
  if (execution != NULL) luaQ_leavemanaged(execution);
}

void luaX_unlockstate (lua_State *L) {
  unlockstate(L, luaQ_ownertoken(), 0);
}

/* Leaving managed execution retains the stack as a root. The owner is
** already in the gate; resolve the current carrier only on reentry. */
LuaWorldScope luaW_detach (lua_State *L) {
  NativeStackContext *context = luaX_boundcontext(L);
  LuaWorldScope scope;
#if defined(LUAI_ASSERT)
  LuaObjectScope *object;
  for (object = L->object_scopes; object != NULL; object = object->previous)
    lua_assert(!object->held);
#endif
  lua_assert(context != NULL && context == luaQ_ownertoken());
  L->native_depth++;
  scope = luaW_ownsheap(L) ? luaW_suspend(L) : (LuaWorldScope){0};
  scope.api_region = context->api_region;
  context->api_region = NULL;
  unlockstate(L, context, 0);
  return scope;
}

static void attach (lua_State *L, LuaWorldScope scope,
                    NativeStackContext *context) {
  lockstate(L, context);
  preparecall(L, context);
  if (scope.collection != 0 || scope.heap != 0 || luaW_ownsheap(L))
    luaW_restore(L, scope, L->native_depth);
  lua_assert(L->native_depth != 0);
  L->native_depth--;
  context->api_region = scope.api_region;
}

carrierentry void luaW_attach (lua_State *L, LuaWorldScope scope) {
  attach(L, scope, luaE_resolvestack(&thread_context));
}

void luaW_returnnative (lua_State *L, LuaWorldScope scope,
                         NativeStackContext *context) {
  lua_assert(context == luaQ_ownertoken());
  attach(L, scope, context);
  if (context->execution != NULL) luaQ_endnative(L, context->execution);
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
  if (region->state != L || luaX_boundcontext(L) != context) {
    fputs("Luthr API crossed an attached runtime activation\n", stderr);
    abort();
  }
  return 1;
}

/* Enter the storage epoch before borrowing any table buffers. A grace
** scan that still sees a stack-only gate covers only older retirements
** than the control word sampled after this promotion. */
static void managestack (lua_State *L, NativeStackContext *context) {
  LuaStackGate *gate = &L->stack_gate;
  /* The caller owns the stack-only gate. Change its mode in one
  ** transaction while preserving any concurrent waiter registration. */
  uintptr_t owner = cast(uintptr_t, context);
  uintptr_t old = __atomic_fetch_xor(&gate->word, owner | LUA_GATE_ACTIVE,
                                    __ATOMIC_SEQ_CST);
  lua_assert((old & ~cast(uintptr_t, LUA_GATE_WAITING)) == LUA_GATE_STACK);
  UNUSED(old);
  luaW_enter(L);
}

l_sinline uintptr_t apienter (lua_State *L, NativeStackContext *context) {
  if (attachedapi(L, context)) {
    luaX_pollrequests(L);
    return 0;
  }
  lockstate(L, context);
  return cast(uintptr_t, context);
}

carrierentry void luaX_apienter (lua_State *L) {
  uintptr_t owner = apienter(L, luaE_resolvestack(&thread_context));
  if (owner != 0) preparecall(L, cast(NativeStackContext *, owner));
}

carrierentry uintptr_t luaX_callenter (lua_State *L) {
  NativeStackContext *context = luaE_resolvestack(&thread_context);
  LuaExecution *execution = context->execution;
  uintptr_t owner;
  if (execution != NULL && execution->native_depth != 0 &&
      context->api_region == NULL) {
    uintptr_t expected = 0;
    uintptr_t reservation = cast(uintptr_t, context) | LUA_GATE_MANAGED;
    /* Publish an owner once. This first phase can inspect the stack, but
    ** entering the storage epoch and borrowing heap data requires a slot. */
    if (__atomic_compare_exchange_n(&L->stack_gate.word, &expected, reservation,
                                     0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
      if (l_likely(!luaW_stopped(&G(L)->world))) {
        if (luaC_tracing(L)) {
          luaC_checksnapshot(L);
          return reservation | LUA_VALUE_TRACING;
        }
        return reservation;
      }
      release(L, context, LUA_GATE_MANAGED);
    }
  }
  owner = apienter(L, context);
  if (owner != 0) preparecall(L, cast(NativeStackContext *, owner));
  return 0;
}

void luaX_releaseaftertrace (lua_State *L, uintptr_t reservation) {
  if (__atomic_load_n(&G(L)->barrier_ready, __ATOMIC_ACQUIRE) != 0)
    luaSC_notifygc(G(L));
  luaX_releasegate(L, reservation);
}

int luaX_callpromote (lua_State *L, NativeStackContext *context) {
  if (!luaQ_tryentermanaged(context->execution)) return 0;
  luaW_enter(L);
  preparecall(L, context);
  return 1;
}

void luaX_apileave (lua_State *L) {
  NativeStackContext *context = luaX_boundcontext(L);
  lua_assert(context != NULL && context == luaQ_ownertoken());
  if (context->api_region == NULL) unlockstate(L, context, 0);
}

carrierentry uintptr_t luaX_valueenterslow (lua_State *L, int reserved) {
  if (reserved) {
    if (l_likely(!luaW_stopped(&G(L)->world))) {
      if (luaC_tracing(L)) {
        luaC_checksnapshot(L);
        return LUA_VALUE_STACK | LUA_VALUE_TRACING;
      }
      return LUA_VALUE_STACK;
    }
    luaX_releasegate(L, LUA_GATE_STACK);
  }
  return apienter(L, luaE_resolvestack(&thread_context));
}

void luaX_valueleaveslow (lua_State *L, uintptr_t owner) {
  if (owner & LUA_VALUE_STACK) {
    lua_assert(owner == (LUA_VALUE_STACK | LUA_VALUE_TRACING));
    luaX_releaseaftertrace(L, LUA_GATE_STACK);
  }
  else unlockstate(L, cast(NativeStackContext *, owner), 1);
}

carrierentry int luaX_valuecall (lua_State *L, uintptr_t owner) {
  if (owner & LUA_VALUE_STACK) {
    LuaThreadContext *thread = &thread_context;
    NativeStackContext *context = luaE_resolvestack(thread);
    if (context->execution != NULL) {
      luaX_valueleave(L, owner);
      return 0;
    }
    managestack(L, context);
  }
  if (owner != 0) preparecall(L, luaX_boundcontext(L));
  return 1;
}

/* Reserve both stacks before joining managed execution. A blocked reservation
** must not count as a mutator or prevent the other owner from collecting.
** No managed stack/CI pointer is borrowed until both reservations are ready. */
carrierentry void luaX_lockpair (lua_State *first, lua_State *second) {
  LuaThreadContext *thread = &thread_context;
  NativeStackContext *context = luaE_resolvestack(thread);
  LuaExecution *execution = context->execution;
  luaQ_entermanaged(first, execution);
  if (cast(uintptr_t, first) > cast(uintptr_t, second)) {
    lua_State *swap = first;
    first = second;
    second = swap;
  }
  /* Never publish participation while waiting for the second gate. A
  ** collector holding it must be able to complete its rendezvous. */
  reserve(first, context, 0);
  if (second != first) reserve(second, context, 0);
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
  preparecall(first, context);
  if (second != first) preparecall(second, context);
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
  if (second != first) release(second, token, 0);
  release(first, token, LUA_GATE_MANAGED);
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
  else if (L->gc_resize) {
    L->gc_resize = 0;
    luaC_checkGC(L);
  }
  luaTK_checkstop(L);
  luaCP_checklimit(L);
}
