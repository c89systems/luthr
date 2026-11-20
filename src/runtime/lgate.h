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

/* The atomic word holds the aligned owner token and two low bits. An owner
** can withdraw collector participation while retaining the stack. WAITING
** reserves an unowned gate for its FIFO head. */
enum {
  LUA_GATE_ACTIVE = 1,
  LUA_GATE_WAITING = 2,
  LUA_GATE_FLAGS = LUA_GATE_ACTIVE | LUA_GATE_WAITING
};

typedef struct LuaStackGate {
  uintptr_t word;
  struct NativeStackContext *context;  /* current reservation's logical stack */
  struct LuaGateWaiter *head;
  struct LuaGateWaiter **tail;
  int ready;
} LuaStackGate;

l_sinline void *luaX_gateowner (uintptr_t word) {
  return cast(void *, word & ~cast(uintptr_t, LUA_GATE_FLAGS));
}

LUAI_FUNC void luaX_initgate (LuaStackGate *gate);
LUAI_FUNC void luaX_destroygate (LuaStackGate *gate);
LUAI_FUNC void luaX_lockstate (lua_State *L);
LUAI_FUNC void luaX_unlockstate (lua_State *L);
/* A rooted system worker must never park while borrowing managed capacity.
** No Lua/native activation or policy is entered by this payload-only scope. */
LUAI_FUNC int luaX_tryworker (lua_State *L);
/* Public/private API entry can borrow an attached runtime C activation;
** physical state gates themselves remain strictly nonrecursive. */
LUAI_FUNC void luaX_apienter (lua_State *L);
LUAI_FUNC void luaX_apileave (lua_State *L);
/* Value-only API transactions retain the same ownership, participation and
** snapshot handshake. They cannot allocate or call Lua; a closing stack
** operation upgrades before invoking its first user callback. */
LUAI_FUNC void luaX_valueenter (lua_State *L);
LUAI_FUNC void luaX_valueleave (lua_State *L);
LUAI_FUNC void luaX_valuecall (lua_State *L);
/* Nonblocking collector reservation. 0 means busy, 1 is a new reservation,
** and 2 means this execution already owns the target. The collector remains
** admitted on its own state throughout a reservation. */
LUAI_FUNC int luaX_tryreserve (lua_State *target);
LUAI_FUNC void luaX_endreserve (lua_State *target, int reservation);
LUAI_FUNC void luaX_lockpair (lua_State *first, lua_State *second);
LUAI_FUNC void luaX_unlockpair (lua_State *first, lua_State *second);
LUAI_FUNC l_uint32 luaX_parentcalls (lua_State *from);
LUAI_FUNC void luaX_checksync (int status);
LUAI_FUNC void luaX_poll (lua_State *L);
/* Caller publishes live stack values before a possible collector handoff.
** This checks ownership requests without consuming a scheduling quantum. */
LUAI_FUNC void luaX_pollrequests (lua_State *L);

#endif
