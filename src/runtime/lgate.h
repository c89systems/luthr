/*
** Per-coroutine stack ownership and VM safepoints.
** See Copyright Notice in lua.h
*/
#ifndef lgate_h
#define lgate_h

#include <pthread.h>
#include "llimits.h"
#include "lexec.h"

struct LuaGateWaiter;

/* Managed reservations carry an aligned owner token. A stack-only
** reservation needs only its flag: it cannot recurse, migrate or park.
** Both modes participate in root rendezvous.
** A managed owner can withdraw admission while retaining the stack.
** WAITING reserves an unowned gate for its FIFO head. DORMANT marks a Lua-created
** coroutine that has never entered the managed rendezvous directory. */
enum {
  LUA_GATE_MANAGED = 1,
  LUA_GATE_WAITING = 2,
  LUA_GATE_STACK = 4,
  LUA_GATE_DORMANT = 8,
  LUA_GATE_ACTIVE = LUA_GATE_MANAGED | LUA_GATE_STACK,
  LUA_GATE_FLAGS = LUA_GATE_ACTIVE | LUA_GATE_WAITING | LUA_GATE_DORMANT
};

enum { LUA_GATE_VALUES = 1, LUA_GATE_SHARED_READ = 2 };
enum { LUA_NATIVE_ARGUMENTS = 2, LUA_NATIVE_TYPES = 8 };

/* A scheduled root can publish its inactive stack and its recallable slot
** in one word. Linux x86-64 userspace addresses use at most 56 bits.
** A zero code means no offer; slots beyond this encoding remain ordinary. */
#define LUA_GATE_OFFER_SHIFT 56
#define LUA_GATE_OFFER_MASK (UINTPTR_MAX << LUA_GATE_OFFER_SHIFT)

typedef struct LuaStackGate {
  uintptr_t word;
  struct LuaGateWaiter *head;
  struct LuaGateWaiter **tail;
  int ready;
  uint64_t values;  /* bounded API scope; a shared read also carries its epoch */
  uintptr_t arguments[LUA_NATIVE_ARGUMENTS];  /* detached argument observations */
} LuaStackGate;

l_sinline struct NativeStackContext *luaX_gateowner (uintptr_t word) {
  return cast(struct NativeStackContext *,
              word & ~(cast(uintptr_t, LUA_GATE_FLAGS) | LUA_GATE_OFFER_MASK));
}

LUAI_FUNC void luaX_initgate (LuaStackGate *gate, int dormant);
LUAI_FUNC void luaX_destroygate (LuaStackGate *gate);
LUAI_FUNC void luaX_waitvalues (LuaStackGate *gate);
LUAI_FUNC void luaX_cleararguments (LuaStackGate *gate);

/* A full claim is published before this join. A racing bounded claim must
** observe the full claim or finish before the full owner reads the stack. */
l_sinline void luaX_joinvalues (LuaStackGate *gate) {
  if (l_unlikely(__atomic_load_n(&gate->values, __ATOMIC_SEQ_CST)))
    luaX_waitvalues(gate);
}

LUAI_FUNC void luaX_lockstate (lua_State *L);
LUAI_FUNC void luaX_unlockstate (lua_State *L);
/* A rooted system worker must never park while borrowing managed capacity.
** No Lua/native activation or policy is entered by this payload-only scope. */
LUAI_FUNC int luaX_tryworker (lua_State *L);
/* Public/private API entry can borrow an attached runtime C activation;
** physical state gates themselves remain strictly nonrecursive. */
LUAI_FUNC void luaX_apienter (lua_State *L);
LUAI_FUNC void luaX_apileave (lua_State *L);
/* Complete a call or enter the full API scope and return zero for ordinary
** dispatch. A nonnull status permits a protected call to finish here. */
LUAI_FUNC int luaX_callenter (lua_State *L, int nargs, int nresults,
                              TStatus *status);
/* Nonblocking collector reservation. 0 means busy, 1 is a new reservation,
** and 2 means this execution already owns the target. The collector remains
** admitted on its own state throughout a reservation. */
LUAI_FUNC int luaX_tryreserve (lua_State *target);
LUAI_FUNC void luaX_endreserve (lua_State *target, int reservation);
LUAI_FUNC void luaX_lockpair (lua_State *first, lua_State *second);
LUAI_FUNC void luaX_unlockpair (lua_State *first, lua_State *second);
/* Keep the execution slot when transferring from a pair to its callee.
** Joining the suspended caller at return never waits on its gate. */
LUAI_FUNC void luaX_transferpair (lua_State *caller, lua_State *callee);
LUAI_FUNC void luaX_releaseparent (lua_State *child);
LUAI_FUNC int luaX_joinpair (lua_State *owner, lua_State *other);
LUAI_FUNC l_uint32 luaX_parentcalls (lua_State *from);
LUAI_FUNC void luaX_checksync (int status);
LUAI_FUNC void luaX_poll (lua_State *L);
/* Caller publishes live stack values before a possible collector handoff.
** This checks ownership requests without consuming a scheduling quantum. */
LUAI_FUNC void luaX_pollrequests (lua_State *L);

#endif
