/*
** Stack ownership for value-only C API operations. See lua.h for license.
*/
#ifndef lstackapi_h
#define lstackapi_h

#include "lgc.h"

LUAI_FUNC l_noret luaX_badrelease (void);

l_sinline void luaX_releasegate (lua_State *L, uintptr_t reservation) {
  global_State *g = G(L);
  uintptr_t stack = cast(uintptr_t, L);
  int managed = (reservation & LUA_GATE_MANAGED) != 0;
  uint64_t armed = managed ? L->participation.trace_armed : 0;
  uint64_t control;
  unsigned requests;
  /* Only a waiter can add its bit to a held reservation. Preserve that bit.
  ** After the subtraction, the collector can reclaim L. */
  uintptr_t previous = __atomic_fetch_sub(&L->stack_gate.word, reservation,
                                          __ATOMIC_SEQ_CST);
  if (l_unlikely((previous & ~cast(uintptr_t, LUA_GATE_WAITING | LUA_GATE_DORMANT))
                 != reservation))
    luaX_badrelease();
  control = luaW_control(&g->world);
  requests = LUA_WORLD_STOPPED | LUA_WORLD_STACK_ROOTS;
  if (!(reservation & LUA_GATE_STACK)) requests |= LUA_WORLD_STORAGE_WAIT;
  /* Only a managed reservation with old allocation debt can hold up an arm. */
  if (l_unlikely((previous & LUA_GATE_WAITING) ||
                 (control & requests) ||
                 ((control & LUA_WORLD_ARM_TRACE) && managed &&
                  armed != luaW_armepoch(&g->world))))
    luaW_leave(g, stack, previous & LUA_GATE_FLAGS, control);
}

/* A stack-only token carries the obligations of this admission. */
enum { LUA_VALUE_STACK = 1, LUA_VALUE_TRACING = 2 };

LUAI_FUNC uintptr_t luaX_valueenterslow (lua_State *L, int reserved);
LUAI_FUNC void luaX_valueleaveslow (lua_State *L, uintptr_t owner);
LUAI_FUNC void luaX_releaseaftertrace (lua_State *L, uintptr_t reservation);

/* Stack-only scopes can borrow stack slots and immutable payloads. Heap
** mutation, allocation, parking, and other stack reservations require
** managed admission. */
l_sinline uintptr_t luaX_valueenter (lua_State *L, int stackonly) {
  uintptr_t expected = 0;
  if (stackonly && __atomic_load_n(&L->stack_gate.word, __ATOMIC_RELAXED) == 0 &&
      __atomic_compare_exchange_n(&L->stack_gate.word, &expected, LUA_GATE_STACK,
                                  0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
    /* A stop sees this reservation or precedes the control-word check. */
    if (l_likely(!luaW_stopped(&G(L)->world) && !luaC_tracing(L)))
      return LUA_VALUE_STACK;
    return luaX_valueenterslow(L, 1);
  }
  return luaX_valueenterslow(L, 0);
}

l_sinline void luaX_valueleave (lua_State *L, uintptr_t owner) {
  if (owner == LUA_VALUE_STACK) luaX_releasegate(L, LUA_GATE_STACK);
  else if (owner != 0) luaX_valueleaveslow(L, owner);
}

/* A callback token is its owner-bound reservation plus the local tracing bit. */
l_sinline void luaX_callleave (lua_State *L, uintptr_t owner) {
  if (l_unlikely(owner & LUA_VALUE_TRACING))
    luaX_releaseaftertrace(L, owner & ~cast(uintptr_t, LUA_VALUE_TRACING));
  else luaX_releasegate(L, owner);
}

/* If managed capacity requires a new reservation, release the stack so the
** caller can retry before changing it or invoking a user function. */
LUAI_FUNC int luaX_valuecall (lua_State *L, uintptr_t owner);

#endif
