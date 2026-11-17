/*
** Per-coroutine stack ownership and VM safepoints.
** See Copyright Notice in lua.h
*/
#ifndef lgate_h
#define lgate_h

#include <pthread.h>
#include "llimits.h"
#include "lexec.h"

typedef struct LuaStackGate {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  LuaOwner owner;
  LuaWaiter *waiters;
  unsigned long next_ticket;
  unsigned long serving;
  unsigned waiting;  /* atomic request count, read by no-request safepoints */
  int ready;
} LuaStackGate;

LUAI_FUNC int luaX_initgate (LuaStackGate *gate);
LUAI_FUNC void luaX_destroygate (LuaStackGate *gate);
LUAI_FUNC void luaX_lockstate (lua_State *L);
LUAI_FUNC void luaX_unlockstate (lua_State *L);
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
