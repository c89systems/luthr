/*
** $Id: lfunc.h $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lua.h
*/

#ifndef lfunc_h
#define lfunc_h


#include "lgc.h"
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

/* Keep a speculative scalar call's stack and safepoint latency bounded. */
enum { LUA_SCALAR_REGISTERS = 8, LUA_SCALAR_CODE_LIMIT = 16 };



/* special status to close upvalues preserving the top of the stack */
#define CLOSEKTOP	(LUA_ERRERR + 1)


LUAI_FUNC Proto *luaF_newproto (lua_State *L);
LUAI_FUNC Proto *luaF_newloadedproto (lua_State *L, int instructions);
LUAI_FUNC void luaF_lowerproto (lua_State *L, Proto *p);
LUAI_FUNC CClosure *luaF_newCclosure (lua_State *L, int nupvals);
LUAI_FUNC LClosure *luaF_newLclosure (lua_State *L, int nupvals);
LUAI_FUNC void luaF_initupvals (lua_State *L, LClosure *cl);
LUAI_FUNC void luaF_setproto (lua_State *L, LClosure *cl, Proto *proto);
LUAI_FUNC UpVal *luaF_findupval (lua_State *L, StkId level);
LUAI_FUNC void luaF_writeupval (lua_State *L, UpVal *uv, const TValue *value);
LUAI_FUNC void luaF_writecell (lua_State *L, UpVal *uv, const TValue *value);
enum { LUA_CELL_WRITING = 1, LUA_CELL_WAITERS = 2, LUA_CELL_VERSION_STEP = 4 };
LUAI_FUNC uint64_t luaF_waitcell (UpVal *uv);
LUAI_FUNC void luaF_wakecell (UpVal *uv);

/* The minimum integer uses the general slot. Every other integer fits one
** atomic word without a range check or a managed reference. */
l_sinline lua_Unsigned luaF_packinteger (const TValue *value) {
  return ttisinteger(value) ? l_castS2U(ivalue(value)) ^ l_castS2U(LUA_MININTEGER)
                           : 0;
}

l_sinline lua_Integer luaF_unpackinteger (lua_Unsigned integer) {
  return l_castU2S(integer ^ l_castS2U(LUA_MININTEGER));
}

l_sinline int luaF_trystoreinteger (UpVal *uv, const TValue *value) {
  lua_Unsigned next = luaF_packinteger(value), previous;
  if (next == 0) return 0;
  previous = __atomic_load_n(&uv->integer, __ATOMIC_RELAXED);
  return previous != 0 &&
    __atomic_compare_exchange_n(&uv->integer, &previous, next, 0,
                                 __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

l_sinline uint64_t luaF_trylockcell (UpVal *uv) {
  uint64_t version = __atomic_load_n(&uv->sequence, __ATOMIC_RELAXED);
  uint64_t writing = version | LUA_CELL_WRITING;
  if ((version & LUA_CELL_WRITING) ||
      !__atomic_compare_exchange_n(&uv->sequence, &version, writing, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
    return 0;
  /* Stop integer-only writers before giving the full owner its ordinary
  ** slot. The exchange includes any integer written before it won. */
  if (__atomic_load_n(&uv->integer, __ATOMIC_RELAXED) != 0) {
    lua_Unsigned packed = __atomic_exchange_n(&uv->integer, 0, __ATOMIC_ACQ_REL);
    __atomic_store_n(&uv->value.value_.i, luaF_unpackinteger(packed),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&uv->value.tt_, LUA_VNUMINT, __ATOMIC_RELEASE);
  }
  return writing;
}

l_sinline uint64_t luaF_lockcell (UpVal *uv) {
  uint64_t writing = luaF_trylockcell(uv);
  return l_likely(writing != 0) ? writing : luaF_waitcell(uv);
}

l_sinline void luaF_unlockcell (UpVal *uv, uint64_t writing) {
  uint64_t version = writing + (LUA_CELL_VERSION_STEP - LUA_CELL_WRITING);
  uint64_t previous = __atomic_exchange_n(&uv->sequence, version, __ATOMIC_RELEASE);
  lua_assert((previous & ~cast(uint64_t, LUA_CELL_WAITERS)) == writing);
  if (l_unlikely(previous & LUA_CELL_WAITERS)) luaF_wakecell(uv);
}
LUAI_FUNC void luaF_publishupval (lua_State *L, UpVal *uv);
LUAI_FUNC int luaF_dropupvals (lua_State *L, int wait);
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
LUAI_FUNC void luaF_loadshared (UpVal *uv, TValue *value);

l_sinline UpVal *luaF_capturecell (LClosure *closure, int n) {
  return __atomic_load_n(&closure->upvals[n], __ATOMIC_ACQUIRE);
}

l_sinline int luaF_localupval (lua_State *L, UpVal *uv) {
  return __atomic_load_n(&uv->sharing, __ATOMIC_ACQUIRE) == cast(uintptr_t, L);
}

l_sinline TValue *luaF_privateslot (lua_State *L, UpVal *uv) {
  lua_assert(uv->owner == NULL || uv->owner == L);
  return upisopen(uv) ? s2v(cast(StkId, cast_charp(L->stack.p) + uv->level))
                     : &uv->value;
}

l_sinline void luaF_writeprivate (lua_State *L, UpVal *uv, const TValue *value) {
  lua_assert(luaF_localupval(L, uv));
  if (upisopen(uv)) { setobj(L, luaF_privateslot(L, uv), value); }
  else luaF_writecell(L, uv, value);
}

/* Acquiring either written component also observes its preceding odd
** sequence. An unchanged even sequence validates the whole value. */
l_sinline int luaF_tryloadshared (UpVal *uv, TValue *value) {
  lua_Unsigned integer = __atomic_load_n(&uv->integer, __ATOMIC_ACQUIRE);
  uint64_t before;
  Value payload;
  lu_byte tag;
  if (integer != 0) {
    setivalue(value, luaF_unpackinteger(integer));
    return 1;
  }
  before = __atomic_load_n(&uv->sequence, __ATOMIC_ACQUIRE);
  if (before & LUA_CELL_WRITING) return 0;
  __atomic_load(&uv->value.value_, &payload, __ATOMIC_ACQUIRE);
  tag = __atomic_load_n(&uv->value.tt_, __ATOMIC_ACQUIRE);
  if (__atomic_load_n(&uv->sequence, __ATOMIC_ACQUIRE) != before ||
      __atomic_load_n(&uv->integer, __ATOMIC_ACQUIRE) != 0) return 0;
  value->value_ = payload;
  value->tt_ = tag;
  return 1;
}

/* Participation protects the selected cell even if another execution joins
** the closure. A borrowed operand must be consumed or rooted before leaving
** participation. The heap write barrier retains overwritten values and cells.
** The contended load also holds admission and the stack gate. */
l_sinline void luaF_loadupval (lua_State *L, UpVal *uv, TValue *value) {
  if (luaF_localupval(L, uv)) {
    const TValue *slot = luaF_privateslot(L, uv);
    value->value_ = slot->value_;
    value->tt_ = slot->tt_;
  }
  else if (!luaF_tryloadshared(uv, value)) luaF_loadshared(uv, value);
}

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
