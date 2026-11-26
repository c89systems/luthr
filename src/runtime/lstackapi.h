/*
** Stack ownership for value-only C API operations. See lua.h for license.
*/
#ifndef lstackapi_h
#define lstackapi_h

#include "lgc.h"

LUAI_FUNC l_noret luaX_badrelease (void);

l_sinline void luaX_releaseto (lua_State *L, uintptr_t reservation,
                               uintptr_t offer) {
  global_State *g = G(L);
  uintptr_t stack = cast(uintptr_t, L);
  int managed = (reservation & LUA_GATE_MANAGED) != 0;
  uint64_t armed = managed ? L->participation.trace_armed : 0;
  uint64_t control;
  unsigned requests;
  /* Preserve waiter registration and any independent slot offer. Withdrawal
  ** into a new offer starts with an exclusive slot. */
  uintptr_t previous = __atomic_fetch_sub(&L->stack_gate.word, reservation - offer,
                                          __ATOMIC_SEQ_CST);
  if (l_unlikely((previous & ~(cast(uintptr_t, LUA_GATE_WAITING | LUA_GATE_DORMANT) |
                               LUA_GATE_OFFER_MASK)) != reservation))
    luaX_badrelease();
  lua_assert(offer == 0 || !(previous & LUA_GATE_OFFER_MASK));
  control = luaW_control(&g->world);
  requests = LUA_WORLD_STOPPED | LUA_WORLD_STACK_ROOTS | LUA_WORLD_SEAL_NURSERIES;
  if (!(reservation & LUA_GATE_STACK)) requests |= LUA_WORLD_STORAGE_WAIT;
  /* Only a managed reservation with old allocation debt can hold up an arm. */
  if (l_unlikely((previous & LUA_GATE_WAITING) ||
                 (control & requests) ||
                 ((control & LUA_WORLD_ARM_TRACE) && managed &&
                  armed != luaW_armepoch(&g->world))))
    luaW_leave(g, stack, previous & LUA_GATE_FLAGS, control);
}

l_sinline void luaX_releasegate (lua_State *L, uintptr_t reservation) {
  luaX_releaseto(L, reservation, 0);
}

/* A stack-only token carries the obligations of this admission. */
enum { LUA_VALUE_STACK = 1, LUA_VALUE_TRACING = 2, LUA_VALUE_BOUNDED = 4 };

LUAI_FUNC uintptr_t luaX_valueenterslow (lua_State *L, int reserved);
LUAI_FUNC void luaX_valueleaveslow (lua_State *L, uintptr_t owner);
LUAI_FUNC uintptr_t luaX_reservevalues (lua_State *L);
LUAI_FUNC void luaX_releasevalues (lua_State *L, uintptr_t owner);

/* Detached arguments are published one word at a time. Each word combines
** a pointer and length, a small integer, or the exact Lua tag. A string
** or userdata borrower still needs the corresponding live stack value. */
enum {
  LUA_ARGUMENT_ALIGN_SHIFT = 2,
  LUA_ARGUMENT_LENGTH_SHIFT = LUA_GATE_OFFER_SHIFT - LUA_ARGUMENT_ALIGN_SHIFT,
  LUA_ARGUMENT_USERDATA = (1u << (64 - LUA_ARGUMENT_LENGTH_SHIFT)) - 1,
  LUA_ARGUMENT_INTEGER = LUA_ARGUMENT_USERDATA - 1,
  LUA_ARGUMENT_TAG = LUA_ARGUMENT_INTEGER - 1
};

#define LUA_ARGUMENT_MASK ((cast(uintptr_t, 1) << LUA_ARGUMENT_LENGTH_SHIFT) - 1)
#define LUA_ARGUMENT_SIGN (cast(uintptr_t, 1) << (LUA_ARGUMENT_LENGTH_SHIFT - 1))

l_sinline uintptr_t luaX_argument (lua_State *L, int idx) {
  unsigned slot = cast_uint(idx) - 1u;
  return slot < LUA_NATIVE_ARGUMENTS
    ? __atomic_load_n(&L->stack_gate.arguments[slot], __ATOMIC_ACQUIRE) : 0;
}

l_sinline unsigned luaX_argumentkind (uintptr_t argument) {
  return cast_uint(argument >> LUA_ARGUMENT_LENGTH_SHIFT);
}

l_sinline int luaX_argumenttag (uintptr_t argument) {
  if (argument == 0) return LUA_TNONE;
  switch (luaX_argumentkind(argument)) {
    case LUA_ARGUMENT_TAG: return cast_int(argument & LUA_ARGUMENT_MASK);
    case LUA_ARGUMENT_INTEGER: return LUA_VNUMINT;
    case LUA_ARGUMENT_USERDATA: return ctb(LUA_VUSERDATA);
    default: return ctb(LUA_VLNGSTR);
  }
}

l_sinline void *luaX_argumentaddress (uintptr_t argument) {
  return cast(void *, (argument & LUA_ARGUMENT_MASK) << LUA_ARGUMENT_ALIGN_SHIFT);
}

l_sinline lua_Integer luaX_argumentinteger (uintptr_t argument) {
  return l_castU2S(((argument & LUA_ARGUMENT_MASK) ^ LUA_ARGUMENT_SIGN) -
                  LUA_ARGUMENT_SIGN);
}

l_sinline void luaX_invalidatearguments (LuaStackGate *gate) {
  if (l_unlikely(__atomic_load_n(&gate->arguments[0], __ATOMIC_RELAXED)))
    luaX_cleararguments(gate);
}

/* After reserving the gate, retire the view before touching stack slots. */
l_sinline void luaX_claimstack (LuaStackGate *gate) {
  luaX_joinvalues(gate);
  luaX_invalidatearguments(gate);
}

/* Stack-only scopes can borrow stack slots, immutable payloads, and atomic
** snapshots from inline heap fields. Retired storage, heap mutation,
** allocation, parking, and other stack reservations require managed admission. */
l_sinline uintptr_t luaX_valueenter (lua_State *L, int stackonly) {
  if (stackonly) {
    uintptr_t expected = __atomic_load_n(&L->stack_gate.word, __ATOMIC_RELAXED);
    if ((expected & ~LUA_GATE_OFFER_MASK) != 0 ||
        !__atomic_compare_exchange_n(&L->stack_gate.word, &expected,
          expected | LUA_GATE_STACK, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
      return luaX_valueenterslow(L, 0);
    luaX_claimstack(&L->stack_gate);
    /* A stop sees this reservation or precedes the control-word check. */
    if (l_likely(!(luaW_control(&G(L)->world) &
                   (LUA_WORLD_STOPPED | LUA_MARK_ACTIVE))))
      return LUA_VALUE_STACK;
    return luaX_valueenterslow(L, 1);
  }
  return luaX_valueenterslow(L, 0);
}

l_sinline void luaX_leavebounded (lua_State *L) {
  __atomic_store_n(&L->stack_gate.values, 0, __ATOMIC_RELEASE);
}

/* A full gate joins this claim before touching the stack. A C frame
** whose roots have been published can rearrange them and read strong
** fields. Lua register shadows can still require a read barrier. */
l_sinline int luaX_boundedready (lua_State *L, unsigned requests) {
  if (l_likely((__atomic_load_n(&L->stack_gate.word, __ATOMIC_SEQ_CST) &
                ~LUA_GATE_OFFER_MASK) == 0)) {
    uint64_t control = luaW_control(&G(L)->world);
    return !(control & (LUA_WORLD_STOPPED | LUA_WORLD_STACK_ROOTS |
                        LUA_WORLD_SEAL_NURSERIES | requests)) &&
      (!(control & LUA_MARK_ACTIVE) ||
       (!isLua(L->ci) &&
        __atomic_load_n(&L->trace_snapshot, __ATOMIC_ACQUIRE) ==
          luaC_traceepoch(G(L))));
  }
  return 0;
}

l_sinline int luaX_claimvalues (LuaStackGate *gate, uint64_t ticket) {
  uint64_t expected = 0;
  return __atomic_compare_exchange_n(&gate->values, &expected, ticket, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
}

l_sinline int luaX_tryobserve (lua_State *L, unsigned requests) {
  if (!luaX_claimvalues(&L->stack_gate, LUA_GATE_VALUES)) return 0;
  if (l_likely(luaX_boundedready(L, requests))) return 1;
  luaX_leavebounded(L);
  return 0;
}

l_sinline int luaX_tryboundedscope (lua_State *L, unsigned requests) {
  if (!luaX_tryobserve(L, requests)) return 0;
  luaX_invalidatearguments(&L->stack_gate);
  return 1;
}

l_sinline void luaX_leavesharedread (lua_State *L) {
  global_State *g = G(L);
  uint64_t control;
  uintptr_t stack = cast(uintptr_t, L);
  /* A waiter registers before testing the ticket. It must observe our
  ** withdrawal or we must observe its request and wake it. */
  __atomic_store_n(&L->stack_gate.values, 0, __ATOMIC_SEQ_CST);
  control = luaW_control(&g->world);
  if (l_unlikely(control & LUA_WORLD_STORAGE_WAIT))
    luaW_leave(g, stack, 0, control);
}

/* Publish the epoch with the stack claim, then acquire the current control
** word before loading a layout. A reclaimer either sees the earlier ticket
** or its replacement preceded the reader's final control-word load. */
l_sinline int luaX_trysharedaccess (lua_State *L, unsigned requests) {
  uint64_t control;
  if (__atomic_load_n(&L->stack_gate.word, __ATOMIC_RELAXED) &
      ~LUA_GATE_OFFER_MASK) return 0;
  control = luaW_control(&G(L)->world);
  requests |= LUA_WORLD_STORAGE_WAIT;
  if (control & requests) return 0;
  if (!luaX_claimvalues(&L->stack_gate,
                        luaW_epoch(control) | LUA_GATE_SHARED_READ)) return 0;
  if (l_likely(luaX_boundedready(L, requests))) return 1;
  luaX_leavesharedread(L);
  return 0;
}

l_sinline int luaX_trysharedread (lua_State *L) {
  return luaX_trysharedaccess(L, 0);
}

l_sinline int luaX_tryunmarked (lua_State *L) {
  return luaX_tryboundedscope(L, LUA_MARK_ACTIVE);
}

/* Reading the frame and appending to it leave the observed slots intact. */
l_sinline uintptr_t luaX_enterobservation (lua_State *L, int stackonly) {
  if (!stackonly || l_unlikely(__atomic_load_n(&L->stack_gate.word,
                           __ATOMIC_RELAXED) & ~LUA_GATE_OFFER_MASK))
    return luaX_valueenterslow(L, 0);
  if (luaX_tryobserve(L, 0)) return LUA_VALUE_BOUNDED;
  return luaX_reservevalues(L);
}

l_sinline uintptr_t luaX_enterbounded (lua_State *L, int stackonly) {
  uintptr_t owner = luaX_enterobservation(L, stackonly);
  if (owner == LUA_VALUE_BOUNDED) luaX_invalidatearguments(&L->stack_gate);
  return owner;
}

l_sinline void luaX_valueleave (lua_State *L, uintptr_t owner) {
  if (owner == LUA_VALUE_STACK) luaX_releasegate(L, LUA_GATE_STACK);
  else if (owner != 0) luaX_valueleaveslow(L, owner);
}

l_sinline void luaX_leaveboundedvalue (lua_State *L, uintptr_t owner) {
  if (l_likely(owner == LUA_VALUE_BOUNDED)) luaX_leavebounded(L);
  else if (owner != 0) luaX_releasevalues(L, owner);
}

/* If managed capacity requires a new reservation, release the stack so the
** caller can retry before changing it or invoking a user function. */
LUAI_FUNC int luaX_valuecall (lua_State *L, uintptr_t owner);

#endif
