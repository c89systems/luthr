/*
** $Id: lfunc.h $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lua.h
*/

#ifndef lfunc_h
#define lfunc_h


#include "lobject.h"
struct LuaNativeRoot;
struct CallInfo;


#define sizeCclosure(n)  \
	(offsetof(CClosure, upvalue) + sizeof(TValue) * cast_uint(n))

#define sizeLclosure(n)  \
	(offsetof(LClosure, upvals) + sizeof(UpVal *) * cast_uint(n))


/*
** maximum number of upvalues in a closure (both C and Lua). (Value
** must fit in a VM register.)
*/
#define MAXUPVAL	255


#define upisopen(up)	((up)->owner != NULL)


#define uplevel(up) check_exp(upisopen(up), \
  cast(StkId, cast_charp((up)->owner->stack.p) + (up)->level))


/*
** maximum number of misses before giving up the cache of closures
** in prototypes
*/
#define MAXMISS		10



/* special status to close upvalues preserving the top of the stack */
#define CLOSEKTOP	(LUA_ERRERR + 1)


LUAI_FUNC Proto *luaF_newproto (lua_State *L);
LUAI_FUNC void luaF_lowerproto (lua_State *L, Proto *p);
LUAI_FUNC CClosure *luaF_newCclosure (lua_State *L, int nupvals);
LUAI_FUNC LClosure *luaF_newLclosure (lua_State *L, int nupvals);
LUAI_FUNC void luaF_initupvals (lua_State *L, LClosure *cl);
LUAI_FUNC void luaF_setproto (lua_State *L, LClosure *cl, Proto *proto);
LUAI_FUNC UpVal *luaF_findupval (lua_State *L, StkId level);
LUAI_FUNC void luaF_readupval (lua_State *L, UpVal *uv,
                             struct LuaNativeRoot *value);
LUAI_FUNC void luaF_writeupval (lua_State *L, UpVal *uv, const TValue *value);
/* Caller owns the private cell or its mutex. This publication kernel cannot
** allocate, wait, move the stack or invoke Lua; collector barriers still apply. */
LUAI_FUNC void luaF_writeownedupval (lua_State *L, UpVal *uv, const TValue *value);
LUAI_FUNC int luaF_trycloseupval (lua_State *L);
/* Register shadows belong to the executing frame. Shared cells are authoritative;
** refresh only operands that are read and publish only actual writes. A frame's
** filter may keep false positives after close, collection or register collisions. */
#define luaF_capturebit(reg) (UINT64_C(1) << (cast_uint(reg) & 63))

l_sinline uint64_t luaF_capturerange (int first, int count) {
  uint64_t bits;
  unsigned shift = cast_uint(first) & 63;
  if (count <= 0) return 0;
  if (count >= 64) return UINT64_MAX;
  bits = UINT64_MAX >> (64 - count);
  return (bits << shift) | (bits >> ((64 - shift) & 63));
}

LUAI_FUNC void luaF_loadrange (lua_State *L, StkId first, int count);
LUAI_FUNC void luaF_storelocals (lua_State *L, StkId first, int count);
LUAI_FUNC StkId luaF_loadlocal (lua_State *L, StkId slot);
LUAI_FUNC void luaF_getcapture (lua_State *L, LClosure *closure, int n,
                               struct LuaNativeRoot *capture);
/* The VM selects a cell once. Reads retain that exact cell across waits and
** later joins; private reads cannot wait. Writes require a non-private cell. */
LUAI_FUNC void luaF_readcapture (lua_State *L, UpVal *uv,
                                struct LuaNativeRoot *value);
LUAI_FUNC void luaF_writecapture (lua_State *L, UpVal *uv, const TValue *value);
LUAI_FUNC void luaF_joincaptures (lua_State *L, LClosure *to, int target,
                                 LClosure *from, int source);
LUAI_FUNC void luaF_newtbcupval (lua_State *L, StkId level);
LUAI_FUNC void luaF_closeupval (lua_State *L, StkId level);
LUAI_FUNC StkId luaF_close (lua_State *L, StkId level, TStatus status, int yy);
LUAI_FUNC void luaF_unlinkupval (UpVal *uv);
LUAI_FUNC lu_mem luaF_protosize (Proto *p);
LUAI_FUNC void luaF_freeproto (lua_State *L, Proto *f);
LUAI_FUNC void luaF_retireproto (lua_State *L, Proto *f);
LUAI_FUNC const char *luaF_getlocalname (const Proto *func, int local_number,
                                         int pc);


#endif
