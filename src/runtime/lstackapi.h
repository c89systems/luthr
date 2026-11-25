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
    luaX_joinvalues(&L->stack_gate);
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

/* The two-sided claim uses sequentially consistent publication and checks.
** Either the full owner or collector sees this scope, or this scope sees
** its request and withdraws before reading the stack. Its release has no
** waiters to notify and never changes the scheduler's slot offer. */
l_sinline int luaX_tryboundedscope (lua_State *L, unsigned requests) {
  LuaStackGate *gate = &L->stack_gate;
  if (l_likely(__atomic_exchange_n(&gate->values, 1, __ATOMIC_SEQ_CST) == 0)) {
    if (l_likely((__atomic_load_n(&gate->word, __ATOMIC_SEQ_CST) &
                  ~LUA_GATE_OFFER_MASK) == 0 &&
                 !(luaW_control(&G(L)->world) &
                   (LUA_WORLD_STOPPED | LUA_MARK_ACTIVE | LUA_WORLD_STACK_ROOTS |
                    LUA_WORLD_SEAL_NURSERIES | requests))))
      return 1;
    luaX_leavebounded(L);
  }
  return 0;
}

l_sinline int luaX_trybounded (lua_State *L) {
  return luaX_tryboundedscope(L, 0);
}

l_sinline uintptr_t luaX_enterbounded (lua_State *L, int stackonly) {
  if (!stackonly || l_unlikely(__atomic_load_n(&L->stack_gate.word,
                           __ATOMIC_RELAXED) & ~LUA_GATE_OFFER_MASK))
    return luaX_valueenterslow(L, 0);
  if (luaX_trybounded(L)) return LUA_VALUE_BOUNDED;
  return luaX_reservevalues(L);
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
