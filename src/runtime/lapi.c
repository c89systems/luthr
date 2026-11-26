/*
** $Id: lapi.c $
** Lua API
** See Copyright Notice in lua.h
*/

#define lapi_c
#define LUA_CORE

#include "lprefix.h"


#include <limits.h>
#include <stdarg.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "lapiowned.h"
#include "laccess.h"
#include "lbuiltin.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lroots.h"
#include "lstackapi.h"
#include "lstring.h"
#include "ltable.h"
#include "ltableaccess.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"



const char lua_ident[] =
  "$LuaVersion: " LUA_COPYRIGHT " $"
  "$LuaAuthors: " LUA_AUTHORS " $";



/*
** Test for a valid index (one that is not the 'nilvalue').
*/
#define isvalid(L, o)	((o) != &G(L)->nilvalue)


/* test for pseudo index */
#define ispseudo(i)		((i) <= LUA_REGISTRYINDEX)

/* test for upvalue */
#define isupvalue(i)		((i) < LUA_REGISTRYINDEX)


/*
** Convert an acceptable index to a pointer to its respective value.
** Non-valid indices return the special nil value 'G(L)->nilvalue'.
** Actual shared slots require capture/registry ownership. API readers use
** luaA_indexvalue snapshots; only the guarded helpers below borrow addresses.
*/
static TValue *index2addr (lua_State *L, int idx) {
  CallInfo *ci = L->ci;
  if (idx > 0) {
    StkId o = ci->func.p + idx;
    api_check(L, idx <= ci->top.p - (ci->func.p + 1), "unacceptable index");
    if (o >= L->top.p) return &G(L)->nilvalue;
    else return s2v(o);
  }
  else if (!ispseudo(idx)) {  /* negative index */
    api_check(L, idx != 0 && -idx <= L->top.p - (ci->func.p + 1),
                 "invalid index");
    return s2v(L->top.p + idx);
  }
  else {  /* upvalues */
    api_check(L, idx != LUA_REGISTRYINDEX, "registry needs a snapshot");
    idx = LUA_REGISTRYINDEX - idx;
    api_check(L, idx <= MAXUPVAL + 1, "upvalue index too large");
    if (ttisCclosure(s2v(ci->func.p))) {  /* C closure? */
      CClosure *func = clCvalue(s2v(ci->func.p));
      return (idx <= func->nupvalues) ? &func->upvalue[idx-1]
                                      : &G(L)->nilvalue;
    }
    else {  /* light C function or Lua function (through a hook)?) */
      api_check(L, ttislcf(s2v(ci->func.p)), "caller not a C function");
      return &G(L)->nilvalue;  /* no upvalues */
    }
  }
}


/* API reads own a snapshot until their last use, including across callbacks.
** Borrowed addresses of shared capture/registry slots never escape ownership. */
TValue *luaA_indexvalue (lua_State *L, int idx, LuaNativeRoot *snapshot) {
  TValue *source;
  int valid;
  luaR_pushroot(L, snapshot);
  if (idx == LUA_REGISTRYINDEX) {
    luaA_getregistry(L, snapshot);
    return &snapshot->value;
  }
  if (isupvalue(idx) && ttisCclosure(s2v(L->ci->func.p))) {
    LuaObjectScope scope;
    luaA_begin(L, gcvalue(s2v(L->ci->func.p)), &scope);
    source = index2addr(L, idx);
    valid = isvalid(L, source);
    luaR_copyroot(L, snapshot, source);
    luaA_end(L, &scope);
  }
  else {
    source = index2addr(L, idx);
    valid = isvalid(L, source);
    /* Native stack slots are covered by the epoch's root snapshot and
    ** by the barriers on the strong heap edges from which C reads them.
    ** An embedded hook can also address a Lua frame's register shadow. */
    if (isLua(L->ci)) luaR_setroot(L, snapshot, source);
    else luaR_copyroot(L, snapshot, source);
  }
  return valid ? &snapshot->value : &G(L)->nilvalue;
}

static const TValue *readpseudo (lua_State *L, int idx, TValue *snapshot) {
  LuaNativeRoot root;
  const TValue *source = luaA_indexvalue(L, idx, &root);
  int valid = isvalid(L, source);
  setobj(L, snapshot, source);
  luaR_poproot(L, &root);
  return valid ? snapshot : &G(L)->nilvalue;
}

/* A value API has no safepoint after reading the owned slot. C closure and
** registry writers preserve overwritten values before publishing new ones. */
l_sinline const TValue *index2read (lua_State *L, int idx, TValue *snapshot,
                                   uintptr_t owner) {
  if (!ispseudo(idx)) {
    const TValue *source = index2addr(L, idx);
    if (owner != LUA_VALUE_BOUNDED && isLua(L->ci))
      luaC_tracevalue(L, source);
    return source;
  }
  return readpseudo(L, idx, snapshot);
}

l_sinline uintptr_t readenter (lua_State *L, int idx) {
  return luaX_valueenter(L, !ispseudo(idx));
}

static void writeindex (lua_State *L, int idx, const TValue *value) {
  TValue *target;
  if (isupvalue(idx)) {
    CClosure *closure;
    LuaObjectScope scope;
    api_check(L, ttisCclosure(s2v(L->ci->func.p)), "caller not a C closure");
    closure = clCvalue(s2v(L->ci->func.p));
    luaA_begin(L, obj2gco(closure), &scope);
    target = index2addr(L, idx);
    api_check(L, isvalid(L, target), "invalid index");
    luaA_publish(L, obj2gco(closure), value);
    luaC_prewrite(L, target, value);
    luaA_store(L, target, value);
    luaC_barrier(L, closure, value);
    luaA_end(L, &scope);
  }
  else if (idx == LUA_REGISTRYINDEX) {
    LuaObjectScope access;
    luaA_beginlock(L, &G(L)->registry_access, &access);
    luaA_storeregistry(L, value);
    luaA_end(L, &access);
  }
  else {
    target = index2addr(L, idx);
    api_check(L, isvalid(L, target), "invalid index");
    setobj(L, target, value);
  }
}

/* Recheck shared cells before conversion and retain ownership through commit.
** Allocator/emergency-GC boundaries are allowed; ordinary finalizers run only
** after releasing ownership in the calling API. */
static int stringindex (lua_State *L, int idx, LuaNativeRoot *value) {
  int converted = 0;
  if (isupvalue(idx)) {
    CClosure *closure = clCvalue(s2v(L->ci->func.p));
    LuaObjectScope scope;
    TValue *target;
    luaA_begin(L, obj2gco(closure), &scope);
    target = index2addr(L, idx);
    luaR_setroot(L, value, target);
    if (cvt2str(&value->value)) {
      luaO_tostring(L, &value->value);
      luaC_prewrite(L, target, &value->value);
      luaA_store(L, target, &value->value);
      luaC_barrier(L, closure, &value->value);
      converted = 1;
    }
    luaA_end(L, &scope);
  }
  else {
    LuaObjectScope access;
    if (idx == LUA_REGISTRYINDEX) {
      luaA_beginlock(L, &G(L)->registry_access, &access);
      luaR_setroot(L, value, &G(L)->l_registry);
    }
    if (cvt2str(&value->value)) {
      luaO_tostring(L, &value->value);
      if (idx == LUA_REGISTRYINDEX)
        luaA_storeregistry(L, &value->value);
      else writeindex(L, idx, &value->value);
      converted = 1;
    }
    if (idx == LUA_REGISTRYINDEX) luaA_end(L, &access);
  }
  return converted;
}



/*
** Convert a valid actual index (not a pseudo-index) to its address.
*/
static StkId index2stack (lua_State *L, int idx) {
  CallInfo *ci = L->ci;
  if (idx > 0) {
    StkId o = ci->func.p + idx;
    api_check(L, o < L->top.p, "invalid index");
    return o;
  }
  else {    /* non-positive index */
    api_check(L, idx != 0 && -idx <= L->top.p - (ci->func.p + 1),
                 "invalid index");
    api_check(L, !ispseudo(idx), "invalid index");
    return L->top.p + idx;
  }
}


int luaA_checkstack (lua_State *L, int n) {
  int res;
  CallInfo *ci = L->ci;
  api_check(L, n >= 0, "negative 'n'");
  if (L->stack_last.p - L->top.p > n)  /* stack large enough? */
    res = 1;  /* yes; check is OK */
  else  /* need to grow stack */
    res = luaD_growstack(L, n, 0);
  if (res && ci->top.p < L->top.p + n)
    ci->top.p = L->top.p + n;  /* adjust frame top */
  return res;
}

LUA_API int lua_checkstack (lua_State *L, int n) {
  int result;
  lua_lock(L);
  result = luaA_checkstack(L, n);
  lua_unlock(L);
  return result;
}

void luaA_move (lua_State *from, lua_State *to, int n) {
  int i;
  api_checkpop(from, n);
  api_check(from, to->ci->top.p - to->top.p >= n, "stack overflow");
  for (i = 0; i < n; i++) {
    /* Promotion can wait for a weak cutoff. Retain every source stack root
    ** and recompute its address after that admission boundary. */
    luaA_sharevalue(from, s2v(from->top.p - n + i));
    luaC_tracevalue(to, s2v(from->top.p - n + i));
    setobjs2s(to, to->top.p, from->top.p - n + i);
    to->top.p++;  /* stack already checked by previous 'api_check' */
  }
  from->top.p -= n;
}

LUA_API void lua_xmove (lua_State *from, lua_State *to, int n) {
  if (from == to) return;
  api_check(from, G(from) == G(to), "moving among independent states");
  luaX_lockpair(from, to);
  luaA_move(from, to, n);
  luaX_unlockpair(from, to);
}


LUA_API lua_CFunction lua_atpanic (lua_State *L, lua_CFunction panicf) {
  lua_CFunction old;
  LuaObjectScope access;
  lua_lock(L);
  luaA_beginlock(L, &G(L)->panic_access, &access);
  old = G(L)->panic;
  G(L)->panic = panicf;
  luaA_end(L, &access);
  lua_unlock(L);
  return old;
}


LUA_API lua_Number lua_version (lua_State *L) {
  UNUSED(L);
  return LUA_VERSION_NUM;
}



/*
** basic stack manipulation
*/


/*
** convert an acceptable stack index into an absolute index
*/
LUA_API int lua_absindex (lua_State *L, int idx) {
  int result;
  if (idx > 0 || ispseudo(idx))
    return idx;
  uintptr_t owner = luaX_enterbounded(L, 1);
  result = cast_int(L->top.p - L->ci->func.p) + idx;
  luaX_leaveboundedvalue(L, owner);
  return result;
}


LUA_API int lua_gettop (lua_State *L) {
  int result;
  uintptr_t owner = luaX_enterbounded(L, 1);
  result = cast_int(L->top.p - (L->ci->func.p + 1));
  luaX_leaveboundedvalue(L, owner);
  return result;
}


LUA_API void lua_settop (lua_State *L, int idx) {
  CallInfo *ci;
  StkId func, newtop;
  ptrdiff_t diff;  /* difference for new top */
  int closing = 0;
  int stackonly = 1;
  int bounded = 1;
  uintptr_t owner;
 retry:
  owner = bounded ? luaX_enterbounded(L, stackonly)
                  : luaX_valueenter(L, stackonly);
  ci = L->ci;
  func = ci->func.p;
  if (idx >= 0) {
    api_check(L, idx <= ci->top.p - (func + 1), "new top too large");
    diff = ((func + 1) + idx) - L->top.p;
    if (owner == LUA_VALUE_BOUNDED && diff > LUA_MINSTACK) {
      luaX_leavebounded(L);
      bounded = 0;
      goto retry;
    }
    for (; diff > 0; diff--)
      setnilvalue2s(L->top.p++);  /* clear new slots */
  }
  else {
    api_check(L, -(idx+1) <= (L->top.p - (func + 1)), "invalid new top");
    diff = idx + 1;  /* will "subtract" index (as it is negative) */
  }
  newtop = L->top.p + diff;
  if (diff < 0 && L->tbclist.p >= newtop) {
    ptrdiff_t offset = savestack(L, newtop);
    lua_assert(ci->callstatus & CIST_TBC);
    if (!luaX_valuecall(L, owner)) {
      stackonly = 0;
      goto retry;
    }
    newtop = restorestack(L, offset);
    closing = 1;
    newtop = luaF_close(L, newtop, CLOSEKTOP, 0);
  }
  L->top.p = newtop;  /* correct top only after closing any upvalue */
  if (closing) lua_unlock(L);
  else luaX_leaveboundedvalue(L, owner);
}


LUA_API void lua_closeslot (lua_State *L, int idx) {
  StkId level;
  lua_lock(L);
  level = index2stack(L, idx);
  api_check(L, (L->ci->callstatus & CIST_TBC) && (L->tbclist.p == level),
     "no variable to close at given level");
  level = luaF_close(L, level, CLOSEKTOP, 0);
  setnilvalue2s(level);
  lua_unlock(L);
}


/*
** Reverse the stack segment from 'from' to 'to'
** (auxiliary to 'lua_rotate')
** Note that we move(copy) only the value inside the stack.
** (We do not move additional fields that may exist.)
*/
static void reverse (lua_State *L, StkId from, StkId to) {
  for (; from < to; from++, to--) {
    TValue temp;
    setobj(L, &temp, s2v(from));
    setobjs2s(L, from, to);
    setobj2s(L, to, &temp);
  }
}


/*
** Let x = AB, where A is a prefix of length 'n'. Then,
** rotate x n == BA. But BA == (A^r . B^r)^r.
*/
LUA_API void lua_rotate (lua_State *L, int idx, int n) {
  StkId p, t, m;
  uintptr_t owner = luaX_enterbounded(L, 1);
  t = L->top.p - 1;  /* end of stack segment being rotated */
  p = index2stack(L, idx);  /* start of segment */
  if (owner == LUA_VALUE_BOUNDED && t - p >= LUA_MINSTACK) {
    luaX_leavebounded(L);
    owner = luaX_valueenter(L, 1);
    t = L->top.p - 1;
    p = index2stack(L, idx);
  }
  api_check(L, L->tbclist.p < p, "moving a to-be-closed slot");
  api_check(L, (n >= 0 ? n : -n) <= (t - p + 1), "invalid 'n'");
  m = (n >= 0 ? t - n : p - n - 1);  /* end of prefix */
  reverse(L, p, m);  /* reverse the prefix with length 'n' */
  reverse(L, m + 1, t);  /* reverse the suffix */
  reverse(L, p, t);  /* reverse the entire segment */
  luaX_leaveboundedvalue(L, owner);
}


static void __attribute__((noinline))
copyshared (lua_State *L, int fromidx, int toidx) {
  LuaNativeRoot source;
  TValue *fr;
  uintptr_t owner = luaX_valueenter(L, 0);
  fr = luaA_indexvalue(L, fromidx, &source);
  if (fromidx == toidx) {
    api_check(L, isvalid(L, fr), "invalid index");
  }
  else
    writeindex(L, toidx, fr);
  luaR_poproot(L, &source);
  luaX_valueleave(L, owner);
}


LUA_API void lua_copy (lua_State *L, int fromidx, int toidx) {
  const TValue *from;
  TValue *to;
  uintptr_t owner;
  if (ispseudo(fromidx) || ispseudo(toidx)) {
    copyshared(L, fromidx, toidx);
    return;
  }
  owner = luaX_enterbounded(L, 1);
  from = index2addr(L, fromidx);
  to = index2addr(L, toidx);
  api_check(L, isvalid(L, to), "invalid index");
  if (fromidx != toidx) {
    /* A hook can read shadow registers of a Lua frame. Keep the
    ** conservative source barrier when copying one into a live slot. */
    if (owner != LUA_VALUE_BOUNDED && isLua(L->ci)) luaC_tracevalue(L, from);
    setobj(L, to, from);
  }
  luaX_leaveboundedvalue(L, owner);
}


LUA_API void lua_pushvalue (lua_State *L, int idx) {
  TValue snapshot;
  const TValue *source;
  uintptr_t owner = luaX_enterbounded(L, !ispseudo(idx));
  source = index2read(L, idx, &snapshot, owner);
  setobj2s(L, L->top.p, source);
  api_incr_top(L);
  luaX_leaveboundedvalue(L, owner);
}



/*
** access functions (stack -> C)
*/


LUA_API int lua_type (lua_State *L, int idx) {
  TValue snapshot;
  const TValue *o;
  int result;
  uintptr_t owner = luaX_enterbounded(L, !ispseudo(idx));
  o = index2read(L, idx, &snapshot, owner);
  result = isvalid(L, o) ? ttype(o) : LUA_TNONE;
  luaX_leaveboundedvalue(L, owner);
  return result;
}


LUA_API const char *lua_typename (lua_State *L, int t) {
  UNUSED(L);
  api_check(L, LUA_TNONE <= t && t < LUA_NUMTYPES, "invalid type");
  return ttypename(t);
}


LUA_API int lua_iscfunction (lua_State *L, int idx) {
  TValue snapshot;
  const TValue *o;
  int result;
  uintptr_t owner = luaX_enterbounded(L, !ispseudo(idx));
  o = index2read(L, idx, &snapshot, owner);
  result = ttislcf(o) || ttisCclosure(o);
  luaX_leaveboundedvalue(L, owner);
  return result;
}


LUA_API int lua_isinteger (lua_State *L, int idx) {
  TValue snapshot;
  const TValue *o;
  int result;
  uintptr_t owner = luaX_enterbounded(L, !ispseudo(idx));
  o = index2read(L, idx, &snapshot, owner);
  result = ttisinteger(o);
  luaX_leaveboundedvalue(L, owner);
  return result;
}


LUA_API int lua_isnumber (lua_State *L, int idx) {
  TValue snapshot;
  lua_Number n;
  const TValue *o;
  int result;
  uintptr_t owner = readenter(L, idx);
  o = index2read(L, idx, &snapshot, owner);
  result = tonumber(o, &n);
  luaX_valueleave(L, owner);
  return result;
}


LUA_API int lua_isstring (lua_State *L, int idx) {
  TValue snapshot;
  const TValue *o;
  int result;
  uintptr_t owner = luaX_enterbounded(L, !ispseudo(idx));
  o = index2read(L, idx, &snapshot, owner);
  result = ttisstring(o) || cvt2str(o);
  luaX_leaveboundedvalue(L, owner);
  return result;
}


LUA_API int lua_isuserdata (lua_State *L, int idx) {
  TValue snapshot;
  const TValue *o;
  int result;
  uintptr_t owner = luaX_enterbounded(L, !ispseudo(idx));
  o = index2read(L, idx, &snapshot, owner);
  result = ttisfulluserdata(o) || ttislightuserdata(o);
  luaX_leaveboundedvalue(L, owner);
  return result;
}


static int __attribute__((noinline))
equalshared (lua_State *L, int index1, int index2) {
  LuaNativeRoot first, second;
  const TValue *o1, *o2;
  int result;
  uintptr_t owner = luaX_valueenter(L, 0);
  o1 = luaA_indexvalue(L, index1, &first);
  if (index1 == index2) {
    luaR_pushroot(L, &second);
    luaR_setroot(L, &second, o1);
    o2 = isvalid(L, o1) ? &second.value : &G(L)->nilvalue;
  }
  else
    o2 = luaA_indexvalue(L, index2, &second);
  result = (isvalid(L, o1) && isvalid(L, o2)) ? luaV_rawequalobj(o1, o2) : 0;
  luaR_poproot(L, &second);
  luaR_poproot(L, &first);
  luaX_valueleave(L, owner);
  return result;
}


LUA_API int lua_rawequal (lua_State *L, int index1, int index2) {
  const TValue *o1, *o2;
  uintptr_t owner;
  int result;
  if (ispseudo(index1) || ispseudo(index2))
    return equalshared(L, index1, index2);
  owner = luaX_valueenter(L, 1);
  o1 = index2addr(L, index1);
  o2 = index2addr(L, index2);
  result = isvalid(L, o1) && isvalid(L, o2) && luaV_rawequalobj(o1, o2);
  luaX_valueleave(L, owner);
  return result;
}


LUA_API void lua_arith (lua_State *L, int op) {
  lua_lock(L);
  if (op != LUA_OPUNM && op != LUA_OPBNOT)
    api_checkpop(L, 2);  /* all other operations expect two operands */
  else {  /* for unary operations, add fake 2nd operand */
    api_checkpop(L, 1);
    setobjs2s(L, L->top.p, L->top.p - 1);
    api_incr_top(L);
  }
  /* first operand at top - 2, second at top - 1; result go to top - 2 */
  luaO_arith(L, op, s2v(L->top.p - 2), s2v(L->top.p - 1), L->top.p - 2);
  L->top.p--;  /* pop second operand */
  lua_unlock(L);
}


LUA_API int lua_compare (lua_State *L, int index1, int index2, int op) {
  LuaNativeRoot first, second;
  const TValue *o1;
  const TValue *o2;
  int i = 0;
  lua_lock(L);  /* may call tag method */
  o1 = luaA_indexvalue(L, index1, &first);
  if (index1 == index2) {
    luaR_pushroot(L, &second);
    luaR_setroot(L, &second, o1);
    o2 = isvalid(L, o1) ? &second.value : &G(L)->nilvalue;
  }
  else
    o2 = luaA_indexvalue(L, index2, &second);
  if (isvalid(L, o1) && isvalid(L, o2)) {
    switch (op) {
      case LUA_OPEQ: i = luaV_equalobj(L, o1, o2); break;
      case LUA_OPLT: i = luaV_lessthan(L, o1, o2); break;
      case LUA_OPLE: i = luaV_lessequal(L, o1, o2); break;
      default: api_check(L, 0, "invalid option");
    }
  }
  luaR_poproot(L, &second);
  luaR_poproot(L, &first);
  lua_unlock(L);
  return i;
}


LUA_API unsigned lua_numbertocstring (lua_State *L, int idx, char *buff) {
  TValue snapshot;
  const TValue *o;
  unsigned len = 0;
  uintptr_t owner = readenter(L, idx);
  o = index2read(L, idx, &snapshot, owner);
  if (ttisnumber(o)) {
    len = luaO_tostringbuff(o, buff);
    buff[len++] = '\0';  /* add final zero */
  }
  luaX_valueleave(L, owner);
  return len;
}


LUA_API size_t lua_stringtonumber (lua_State *L, const char *s) {
  size_t sz;
  lua_lock(L);
  sz = luaO_str2num(s, s2v(L->top.p));
  if (sz != 0)
    api_incr_top(L);
  lua_unlock(L);
  return sz;
}


LUA_API lua_Number lua_tonumberx (lua_State *L, int idx, int *pisnum) {
  TValue snapshot;
  lua_Number n = 0;
  const TValue *o;
  int isnum;
  uintptr_t owner = readenter(L, idx);
  o = index2read(L, idx, &snapshot, owner);
  isnum = tonumber(o, &n);
  if (pisnum)
    *pisnum = isnum;
  luaX_valueleave(L, owner);
  return n;
}


LUA_API lua_Integer lua_tointegerx (lua_State *L, int idx, int *pisnum) {
  TValue snapshot;
  lua_Integer res = 0;
  const TValue *o;
  int isnum;
  uintptr_t owner = readenter(L, idx);
  o = index2read(L, idx, &snapshot, owner);
  isnum = tointeger(o, &res);
  if (pisnum)
    *pisnum = isnum;
  luaX_valueleave(L, owner);
  return res;
}


LUA_API int lua_toboolean (lua_State *L, int idx) {
  TValue snapshot;
  const TValue *o;
  int result;
  uintptr_t owner = luaX_enterbounded(L, !ispseudo(idx));
  o = index2read(L, idx, &snapshot, owner);
  result = !l_isfalse(o);
  luaX_leaveboundedvalue(L, owner);
  return result;
}


static const char *stringbytes (const TValue *value, size_t *len) {
  if (ttisstring(value)) {
    TString *str = tsvalue(value);
    return len != NULL ? getlstr(str, *len) : getstr(str);
  }
  if (len != NULL) *len = 0;
  return NULL;
}


LUA_API const char *lua_tolstring (lua_State *L, int idx, size_t *len) {
  TValue snapshot;
  const char *result;
  uintptr_t owner;
  const TValue *value;
  int stackonly = !ispseudo(idx);
 retry:
  owner = luaX_enterbounded(L, stackonly);
  value = index2read(L, idx, &snapshot, owner);
  if (cvt2str(value)) {
    LuaNativeRoot root;
    if (!luaX_valuecall(L, owner)) {
      stackonly = 0;
      goto retry;
    }
    luaR_pushroot(L, &root);
    luaR_setroot(L, &root, value);
    if (stringindex(L, idx, &root)) luaC_checkGC(L);
    result = stringbytes(&root.value, len);
    luaR_poproot(L, &root);
    lua_unlock(L);
  }
  else {
    result = stringbytes(value, len);
    luaX_leaveboundedvalue(L, owner);
  }
  return result;
}


static lua_Unsigned fixedlen (const TValue *o) {
  switch (ttypetag(o)) {
    case LUA_VSHRSTR: return tsvalue(o)->shrlen;
    case LUA_VLNGSTR: return cast(lua_Unsigned, tsvalue(o)->lnglen);
    case LUA_VUSERDATA: return cast(lua_Unsigned, uvalue(o)->len);
    default: return 0;
  }
}


LUA_API lua_Unsigned lua_rawlen (lua_State *L, int idx) {
  TValue snapshot;
  lua_Unsigned result;
  uintptr_t owner = luaX_enterbounded(L, !ispseudo(idx));
  const TValue *o = index2read(L, idx, &snapshot, owner);
  if (l_likely(!ttistable(o))) {
    result = fixedlen(o);
    luaX_leaveboundedvalue(L, owner);
  }
  else {
    LuaNativeRoot rooted;
    if (!luaX_valuecall(L, owner)) lua_lock(L);
    o = luaA_indexvalue(L, idx, &rooted);
    result = ttistable(o) ? luaH_getn(L, hvalue(o)) : fixedlen(o);
    luaR_poproot(L, &rooted);
    lua_unlock(L);
  }
  return result;
}


LUA_API lua_CFunction lua_tocfunction (lua_State *L, int idx) {
  TValue snapshot;
  const TValue *o;
  lua_CFunction result = NULL;
  uintptr_t owner = luaX_enterbounded(L, !ispseudo(idx));
  o = index2read(L, idx, &snapshot, owner);
  if (ttislcf(o)) result = fvalue(o);
  else if (ttisCclosure(o))
    result = clCvalue(o)->f;
  luaX_leaveboundedvalue(L, owner);
  return result;
}


LUA_API void *lua_touserdata (lua_State *L, int idx) {
  TValue snapshot;
  const TValue *o;
  void *result;
  uintptr_t owner = luaX_enterbounded(L, !ispseudo(idx));
  o = index2read(L, idx, &snapshot, owner);
  result = luaA_udatapointer(o);
  luaX_leaveboundedvalue(L, owner);
  return result;
}


LUA_API lua_State *lua_tothread (lua_State *L, int idx) {
  TValue snapshot;
  const TValue *o;
  lua_State *result;
  uintptr_t owner = luaX_enterbounded(L, !ispseudo(idx));
  o = index2read(L, idx, &snapshot, owner);
  result = (!ttisthread(o)) ? NULL : thvalue(o);
  luaX_leaveboundedvalue(L, owner);
  return result;
}


/*
** Returns a pointer to the internal representation of an object.
** Note that ISO C does not allow the conversion of a pointer to
** function to a 'void*', so the conversion here goes through
** a 'size_t'. (As the returned pointer is only informative, this
** conversion should not be a problem.)
*/
LUA_API const void *lua_topointer (lua_State *L, int idx) {
  TValue snapshot;
  const TValue *o;
  const void *result = NULL;
  uintptr_t owner = luaX_enterbounded(L, !ispseudo(idx));
  o = index2read(L, idx, &snapshot, owner);
  switch (ttypetag(o)) {
    case LUA_VLCF: case LUA_VMCF:
      result = cast_voidp(cast_sizet(fvalue(o))); break;
    case LUA_VUSERDATA: case LUA_VLIGHTUSERDATA:
      result = luaA_udatapointer(o); break;
    default: {
      if (iscollectable(o))
        result = gcvalue(o);
      break;
    }
  }
  luaX_leaveboundedvalue(L, owner);
  return result;
}



/*
** push functions (C -> stack)
*/


LUA_API void lua_pushnil (lua_State *L) {
  uintptr_t owner = luaX_enterbounded(L, 1);
  setnilvalue2s(L->top.p);
  api_incr_top(L);
  luaX_leaveboundedvalue(L, owner);
}


LUA_API void lua_pushnumber (lua_State *L, lua_Number n) {
  uintptr_t owner = luaX_enterbounded(L, 1);
  setfltvalue(s2v(L->top.p), n);
  api_incr_top(L);
  luaX_leaveboundedvalue(L, owner);
}


LUA_API void lua_pushinteger (lua_State *L, lua_Integer n) {
  uintptr_t owner = luaX_enterbounded(L, 1);
  setivalue(s2v(L->top.p), n);
  api_incr_top(L);
  luaX_leaveboundedvalue(L, owner);
}


/*
** Pushes on the stack a string with given length. The canonical empty
** string is selected without reading 's' when 'len' is zero.
*/
LUA_API const char *lua_pushlstring (lua_State *L, const char *s, size_t len) {
  TString *ts;
  if (len <= LUA_ALLOC_MAX - sizestrreg(0) &&
      !(__atomic_load_n(&L->stack_gate.word, __ATOMIC_RELAXED) &
        ~LUA_GATE_OFFER_MASK) &&
      luaX_tryboundedscope(L, LUA_MARK_ACTIVE | LUA_WORLD_ARM_TRACE)) {
    ts = len <= LUAI_MAXSHORTLEN ? luaS_trycachedshort(L, s, len)
         : !isLua(L->ci) ? luaS_trytasklstr(L, s, len) : NULL;
    if (ts != NULL) {
      setsvalue2s(L, L->top.p, ts);
      api_incr_top(L);
      s = getstr(ts);
      luaX_leavebounded(L);
      return s;
    }
    luaX_leavebounded(L);
  }
  lua_lock(L);
  ts = luaS_cachelstr(L, s, len);
  setsvalue2s(L, L->top.p, ts);
  api_incr_top(L);
  luaC_checkGC(L);
  s = getstr(ts);
  lua_unlock(L);
  return s;
}


LUA_API const char *lua_pushexternalstring (lua_State *L,
	        const char *s, size_t len, lua_Alloc falloc, void *ud) {
  TString *ts;
  lua_lock(L);
  api_check(L, len <= MAX_SIZE, "string too large");
  api_check(L, s[len] == '\0', "string not ending with zero");
  ts = luaS_newextlstr (L, s, len, falloc, ud);
  setsvalue2s(L, L->top.p, ts);
  api_incr_top(L);
  luaC_checkGC(L);
  s = getstr(ts);
  lua_unlock(L);
  return s;
}


LUA_API const char *lua_pushstring (lua_State *L, const char *s) {
  if (s != NULL &&
      !(__atomic_load_n(&L->stack_gate.word, __ATOMIC_RELAXED) &
        ~LUA_GATE_OFFER_MASK) && luaX_tryunmarked(L)) {
    TString *ts = luaS_trycachedname(L, s);
    if (ts != NULL) {
      setsvalue2s(L, L->top.p, ts);
      api_incr_top(L);
      s = getshrstr(ts);
      luaX_leavebounded(L);
      return s;
    }
    luaX_leavebounded(L);
  }
  lua_lock(L);
  if (s == NULL)
    setnilvalue2s(L->top.p);
  else {
    TString *ts;
    ts = luaS_new(L, s);
    setsvalue2s(L, L->top.p, ts);
    s = getstr(ts);  /* internal copy's address */
  }
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
  return s;
}


LUA_API const char *lua_pushvfstring (lua_State *L, const char *fmt,
                                      va_list argp) {
  const char *ret;
  lua_lock(L);
  ret = luaO_pushvfstring(L, fmt, argp);
  luaC_checkGC(L);
  lua_unlock(L);
  return ret;
}


LUA_API const char *lua_pushfstring (lua_State *L, const char *fmt, ...) {
  const char *ret;
  va_list argp;
  lua_lock(L);
  pushvfstring(L, argp, fmt, ret);
  luaC_checkGC(L);
  lua_unlock(L);
  return ret;
}


static void pushcclosure (lua_State *L, lua_CFunction fn, int n, int managed) {
  if (n == 0) {
    uintptr_t owner = luaX_valueenter(L, 1);
    if (managed) {
      int builtin = luaB_leafindex(fn);
      if (builtin >= 0) { setbcfvalue(s2v(L->top.p), builtin); }
      else { setmcfvalue(s2v(L->top.p), fn); }
    }
    else { setfvalue(s2v(L->top.p), fn); }
    api_incr_top(L);
    luaX_valueleave(L, owner);
  }
  else {
    int i;
    CClosure *cl;
    lua_lock(L);
    api_checkpop(L, n);
    api_check(L, n <= MAXUPVAL, "upvalue index too large");
    cl = luaF_newCclosure(L, n);
    cl->f = fn;
    cl->managed = cast_byte(managed);
    for (i = 0; i < n; i++) {
      luaA_store(L, &cl->upvalue[i], s2v(L->top.p - n + i));
      luaC_barrier(L, cl, &cl->upvalue[i]);
    }
    L->top.p -= n;
    setclCvalue(L, s2v(L->top.p), cl);
    api_incr_top(L);
    luaC_checkGC(L);
    lua_unlock(L);
  }
}

LUA_API void lua_pushcclosure (lua_State *L, lua_CFunction fn, int n) {
  pushcclosure(L, fn, n, 0);
}

void luaQ_pushmanaged (lua_State *L, lua_CFunction fn, int n) {
  pushcclosure(L, fn, n, 1);
}


LUA_API void lua_pushboolean (lua_State *L, int b) {
  uintptr_t owner = luaX_enterbounded(L, 1);
  if (b)
    setbtvalue(s2v(L->top.p));
  else
    setbfvalue(s2v(L->top.p));
  api_incr_top(L);
  luaX_leaveboundedvalue(L, owner);
}


LUA_API void lua_pushlightuserdata (lua_State *L, void *p) {
  uintptr_t owner = luaX_enterbounded(L, 1);
  setpvalue(s2v(L->top.p), p);
  api_incr_top(L);
  luaX_leaveboundedvalue(L, owner);
}


LUA_API int lua_pushthread (lua_State *L) {
  int ismain;
  uintptr_t owner = luaX_valueenter(L, 1);
  setthvalue(L, s2v(L->top.p), L);
  api_incr_top(L);
  ismain = mainthread(G(L)) == L;
  luaX_valueleave(L, owner);
  return ismain;
}



/*
** get functions (Lua -> stack)
*/


/*
** The following function assumes that the registry cannot be a weak
** table; so, an emergency collection while using the global table
** cannot collect it.
*/
static void getGlobalTable (lua_State *L, TValue *gt) {
  LuaNativeRoot registry;
  lu_byte tag;
  (void)luaA_indexvalue(L, LUA_REGISTRYINDEX, &registry);
  api_check(L, ttistable(&registry.value), "registry must be a table");
  tag = luaH_getint(L, hvalue(&registry.value), LUA_RIDX_GLOBALS, gt);
  (void)tag;  /* avoid not-used warnings when checks are off */
  api_check(L, novariant(tag) == LUA_TTABLE, "global table must exist");
  luaR_poproot(L, &registry);
}


LUA_API int lua_getglobal (lua_State *L, const char *name) {
  TValue gt;
  int result;
  lua_lock(L);
  getGlobalTable(L, &gt);
  result = luaA_pushfield(L, &gt, name);
  lua_unlock(L);
  return result;
}


LUA_API int lua_gettable (lua_State *L, int idx) {
  LuaNativeRoot indexed;
  lu_byte tag;
  TValue *t;
  lua_lock(L);
  api_checkpop(L, 1);
  t = luaA_indexvalue(L, idx, &indexed);
  luaV_fastget(t, s2v(L->top.p - 1), s2v(L->top.p - 1), luaH_get, tag);
  if (luaV_needsindex(tag))
    tag = luaV_finishget(L, t, s2v(L->top.p - 1), L->top.p - 1);
  luaR_poproot(L, &indexed);
  lua_unlock(L);
  return novariant(tag);
}


static int readnamedfield (lua_State *L, const TValue *object, const char *name,
                            TValue *result) {
  size_t len = 0;
  if (!ttistable(object)) return 0;
  while (len <= LUAI_MAXSHORTLEN && name[len] != '\0') len++;
  return len <= LUAI_MAXSHORTLEN &&
         luaH_readsharedname(L, hvalue(object), name, len, result);
}


int luaA_testnamedudata (lua_State *L, int idx, const char *name, void **result) {
  const TValue *object;
  TValue registry, value;
  Table *mt;
  void *payload;
  int ready;
  if (ispseudo(idx) || !luaX_trysharedread(L)) return 0;
  object = index2addr(L, idx);
  payload = luaA_udatapointer(object);
  mt = payload != NULL ? luaA_readmetatable(G(L), object) : NULL;
  ready = mt == NULL || (luaA_tryregistry(G(L), &registry) &&
                         readnamedfield(L, &registry, name, &value));
  if (ready)
    *result = mt != NULL && ttistable(&value) && hvalue(&value) == mt
      ? payload : NULL;
  luaX_leavesharedread(L);
  return ready;
}


LUA_API int lua_getfield (lua_State *L, int idx, const char *k) {
  LuaNativeRoot indexed;
  int result;
  if (idx >= LUA_REGISTRYINDEX && luaX_trysharedread(L)) {
    TValue registry, value;
    const TValue *object = idx != LUA_REGISTRYINDEX ? index2addr(L, idx)
      : luaA_tryregistry(G(L), &registry) ? &registry : NULL;
    if (object != NULL && readnamedfield(L, object, k, &value)) {
      setobj2s(L, L->top.p, &value);
      api_incr_top(L);
      result = ttype(&value);
      luaX_leavesharedread(L);
      return result;
    }
    luaX_leavesharedread(L);
  }
  lua_lock(L);
  result = luaA_pushfield(L, luaA_indexvalue(L, idx, &indexed), k);
  luaR_poproot(L, &indexed);
  lua_unlock(L);
  return result;
}


LUA_API int lua_geti (lua_State *L, int idx, lua_Integer n) {
  LuaNativeRoot indexed;
  TValue *t;
  lu_byte tag;
  lua_lock(L);
  t = luaA_indexvalue(L, idx, &indexed);
  luaV_fastgeti(t, n, s2v(L->top.p), tag);
  if (luaV_needsindex(tag)) {
    TValue key;
    setivalue(&key, n);
    tag = luaV_finishget(L, t, &key, L->top.p);
  }
  api_incr_top(L);
  luaR_poproot(L, &indexed);
  lua_unlock(L);
  return novariant(tag);
}


static int finishrawget (lua_State *L, lu_byte tag) {
  if (tagisempty(tag))  /* avoid copying empty items to the stack */
    setnilvalue2s(L->top.p);
  api_incr_top(L);
  return novariant(tag);
}


l_sinline Table *gettable (lua_State *L, int idx, LuaNativeRoot *indexed) {
  TValue *t = luaA_indexvalue(L, idx, indexed);
  api_check(L, ttistable(t), "table expected");
  return hvalue(t);
}

static Table *tryregistryreferences (lua_State *L) {
  TValue registry;
  if (!luaX_trysharedaccess(L, LUA_WORLD_ARM_TRACE))
    return NULL;
  /* Keep an extra slot available to retain the directory through a wake
  ** after withdrawing the storage ticket. */
  if (!isLua(L->ci) && L->top.p < L->ci->top.p &&
      luaA_tryregistry(G(L), &registry) && ttistable(&registry))
    return hvalue(&registry);
  luaX_leavesharedread(L);
  return NULL;
}

static void finishregistryreference (lua_State *L, Table *table,
                                      const LuaPartNotice *notice) {
  if (notice->flags != 0) {
    sethvalue2s(L, L->top.p, table);
    api_incr_top(L);
  }
  luaX_leavesharedread(L);
  if (l_unlikely(luaC_barriernotice(G(L)))) luaC_notifybarriers(G(L));
  if (notice->flags != 0) {
    luaH_notifypart(L, notice);
    lua_lock(L);
    L->top.p--;
    lua_unlock(L);
  }
}

int luaA_ref (lua_State *L, int idx) {
  LuaNativeRoot value, indexed;
  LuaObjectScope scope;
  Table *table;
  TValue link;
  lua_Integer head = 0;
  int ref;
  if (idx == LUA_REGISTRYINDEX && (table = tryregistryreferences(L)) != NULL) {
    LuaPartNotice notice = {0};
    const TValue *value;
    int nil;
    api_checkpop(L, 1);
    value = s2v(L->top.p - 1);
    nil = ttisnil(value);
    ref = nil ? 0 : luaH_tryref(L, table, value, &notice);
    if (nil || ref != 0) L->top.p--;
    finishregistryreference(L, table, &notice);
    if (nil || ref != 0) return ref;
  }
  lua_lock(L);
  api_checkpop(L, 1);
  if (ttisnil(s2v(L->top.p - 1))) {
    L->top.p--;
    lua_unlock(L);
    return 0;
  }
  luaR_pushroot(L, &value);
  luaR_setroot(L, &value, s2v(L->top.p - 1));
  table = gettable(L, idx, &indexed);
  ref = luaH_ref(L, table, &value.value);
  if (ref != 0) goto done;
  luaA_begin(L, obj2gco(table), &scope);
  setnilvalue(&link);
  luaH_getint(L, table, 1, &link);
  if (ttisnumber(&link)) {
    tointeger(&link, &head);
  }
  else {
    lua_assert(l_isfalse(&link));
    setivalue(&link, 0);
    luaH_setint(L, table, 1, &link);
  }
  ref = cast_int(head);
  if (ref != 0) {
    setnilvalue(&link);
    luaH_getint(L, table, ref, &link);
    luaH_setint(L, table, 1, &link);
  }
  else ref = cast_int(luaH_getn(L, table)) + 1;
  luaH_setint(L, table, ref, &value.value);
  luaA_end(L, &scope);
 done:
  L->top.p--;
  luaR_poproot(L, &indexed);
  luaR_poproot(L, &value);
  lua_unlock(L);
  return ref;
}

void luaA_unref (lua_State *L, int idx, int ref) {
  LuaNativeRoot indexed;
  LuaObjectScope scope;
  Table *table;
  TValue head;
  if (idx == LUA_REGISTRYINDEX && (table = tryregistryreferences(L)) != NULL) {
    LuaPartNotice notice = {0};
    int released = luaH_tryunref(L, table, ref, &notice);
    finishregistryreference(L, table, &notice);
    if (released) return;
  }
  lua_lock(L);
  table = gettable(L, idx, &indexed);
  if (!luaH_unref(L, table, ref)) {
    luaA_begin(L, obj2gco(table), &scope);
    setnilvalue(&head);
    luaH_getint(L, table, 1, &head);
    lua_assert(ttisinteger(&head));
    luaH_setint(L, table, ref, &head);
    setivalue(&head, ref);
    luaH_setint(L, table, 1, &head);
    luaA_end(L, &scope);
  }
  luaR_poproot(L, &indexed);
  lua_unlock(L);
}


LUA_API int lua_rawget (lua_State *L, int idx) {
  LuaNativeRoot indexed;
  Table *t;
  lu_byte tag;
  int result;
  lua_lock(L);
  api_checkpop(L, 1);
  t = gettable(L, idx, &indexed);
  tag = luaH_get(L, t, s2v(L->top.p - 1), s2v(L->top.p - 1));
  L->top.p--;  /* pop key */
  result = finishrawget(L, tag);
  luaR_poproot(L, &indexed);
  lua_unlock(L);
  return result;
}

/* A bounded claim protects this scheduled stack and its unpublished nursery
** even when another execution has recalled its managed slot. Readers, sealing
** and trace arming join the claim before borrowing the stack. */
l_sinline int localheap (lua_State *L, int idx) {
  uintptr_t gate = __atomic_load_n(&L->stack_gate.word, __ATOMIC_RELAXED);
  return !ispseudo(idx) && (gate & ~LUA_GATE_OFFER_MASK) == 0 &&
    (gate != 0 || __atomic_load_n(&L->task_owner, __ATOMIC_RELAXED) != NULL) &&
    luaX_tryboundedscope(L, LUA_MARK_ACTIVE | LUA_MARK_ARMING |
                           LUA_WORLD_ARM_TRACE);
}

l_sinline Table *localtable (lua_State *L, int idx, uint64_t *state) {
  const TValue *v = index2addr(L, idx);
  Table *t;
  if (!ttistable(v) || isLua(L->ci)) return NULL;
  t = hvalue(v);
  if (__atomic_load_n(&t->sharing, __ATOMIC_ACQUIRE) != cast(uintptr_t, L))
    return NULL;
  *state = luaC_tablestate(t);
  return luaC_tasktablestate(t, *state) ? t : NULL;
}

l_sinline Table *localarray (lua_State *L, int idx, lua_Integer key,
                              uint64_t *state) {
  Table *t = localtable(L, idx, state);
  return t != NULL && l_castS2U(key) - 1 < t->asize ? t : NULL;
}


LUA_API int lua_rawgeti (lua_State *L, int idx, lua_Integer n) {
  LuaNativeRoot indexed;
  Table *t;
  lu_byte tag;
  int result;
  if (localheap(L, idx)) {
    uint64_t state;
    t = localarray(L, idx, n, &state);
    if (t != NULL) {
      unsigned i = cast_uint(n - 1);
      tag = *getArrTag(t, i);
      if (!tagisempty(tag)) farr2val(t, i, tag, s2v(L->top.p));
      result = finishrawget(L, tag);
      luaX_leavebounded(L);
      return result;
    }
    luaX_leavebounded(L);
  }
  lua_lock(L);
  t = gettable(L, idx, &indexed);
  luaH_fastgeti(t, n, s2v(L->top.p), tag);
  result = finishrawget(L, tag);
  luaR_poproot(L, &indexed);
  lua_unlock(L);
  return result;
}


LUA_API int lua_rawgetp (lua_State *L, int idx, const void *p) {
  LuaNativeRoot indexed;
  Table *t;
  TValue k;
  int result;
  lua_lock(L);
  t = gettable(L, idx, &indexed);
  setpvalue(&k, cast_voidp(p));
  result = finishrawget(L, luaH_get(L, t, &k, s2v(L->top.p)));
  luaR_poproot(L, &indexed);
  lua_unlock(L);
  return result;
}


LUA_API void lua_createtable (lua_State *L, int narray, int nrec) {
  unsigned asize = narray > 0 ? cast_uint(narray) : 0;
  unsigned hsize = nrec > 0 ? cast_uint(nrec) : 0;
  /* A small inline array keeps short native result sequences on one page. */
  enum { NATIVE_SMALL_ARRAY = 6 };
  Table *t;
#if !defined(HARDMEMTESTS)
  if (localheap(L, 0)) {
    /* This is a private cache hit with no backing allocation to pace. A
    ** registered publisher lets a stop settle its batched byte accounting.
    ** Leave refills and any GC assistance to the ordinary path. */
    if (!isLua(L->ci) && !L->task_nursery_pending &&
        __atomic_load_n(&L->publish_previous, __ATOMIC_RELAXED) != NULL &&
        !luaC_needassist(L) && !luaC_cyclepressure(L) &&
        (luaC_sweeping(L) ? L->trace_debt < LUA_GC_ASSIST_BYTES
          : (!luaC_needstep(L) ||
             __atomic_load_n(&G(L)->gcstep_pending, __ATOMIC_RELAXED)))) {
      t = luaH_trycachedtable(L,
        asize == 0 && hsize == 0 ? NATIVE_SMALL_ARRAY : asize, hsize);
      if (t != NULL) {
        sethvalue2s(L, L->top.p, t);
        api_incr_top(L);
        luaX_leavebounded(L);
        return;
      }
    }
    luaX_leavebounded(L);
  }
#endif
  lua_lock(L);
  if (asize == 0 && hsize == 0 && !isLua(L->ci) && L->task_owner != NULL &&
      L->native_depth != 0 && luaC_tasktableeligible(L, LUA_TABLE_INLINE_LIMIT))
    asize = NATIVE_SMALL_ARRAY;
  t = luaH_new(L, asize, hsize);
  sethvalue2s(L, L->top.p, t);
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
}


LUA_API int lua_getmetatable (lua_State *L, int objindex) {
  TValue indexed;
  uintptr_t owner = luaX_enterbounded(L, !ispseudo(objindex));
  const TValue *object = index2read(L, objindex, &indexed, owner);
  Table *mt = luaA_readmetatable(G(L), object);
  if (mt != NULL) {
    /* Metatable replacements shade the outgoing pointer before publishing. */
    sethvalue2s(L, L->top.p, mt);
    api_incr_top(L);
  }
  luaX_leaveboundedvalue(L, owner);
  return mt != NULL;
}


static int __attribute__((noinline)) readlockeduservalue (lua_State *L, int idx,
                                                          int n) {
  LuaNativeRoot indexed;
  TValue *o;
  LuaObjectScope scope;
  int t;
  o = luaA_indexvalue(L, idx, &indexed);
  api_check(L, ttisfulluserdata(o), "full userdata expected");
  luaA_begin(L, gcvalue(o), &scope);
  if (n <= 0 || n > uvalue(o)->nuvalue) {
    setnilvalue2s(L->top.p);
    t = LUA_TNONE;
  }
  else {
    setobj2s(L, L->top.p, &uvalue(o)->uv[n - 1].uv);
    t = ttype(s2v(L->top.p));
  }
  luaA_end(L, &scope);
  api_incr_top(L);
  luaR_poproot(L, &indexed);
  return t;
}


LUA_API int lua_getiuservalue (lua_State *L, int idx, int n) {
  TValue indexed, value;
  uintptr_t owner = luaX_enterbounded(L, !ispseudo(idx));
  const TValue *object = index2read(L, idx, &indexed, owner);
  Udata *u;
  int t = LUA_TNONE;
  api_check(L, ttisfulluserdata(object), "full userdata expected");
  u = uvalue(object);
  if (n <= 0 || n > u->nuvalue)
    setnilvalue2s(L->top.p);
  else {
    /* Inline slots cannot be retired independently of the userdata. A
    ** bounded stack claim keeps the snapshot visible to any new root scan. */
    if (!luaA_readslot(&u->sequence, &u->uv[n - 1].uv, &value)) {
      if (!luaX_valuecall(L, owner)) lua_lock(L);
      t = readlockeduservalue(L, idx, n);
      lua_unlock(L);
      return t;
    }
    /* The writer's deletion barrier preserves an overwritten user value. */
    setobj2s(L, L->top.p, &value);
    t = ttype(&value);
  }
  api_incr_top(L);
  luaX_leaveboundedvalue(L, owner);
  return t;
}


/*
** set functions (stack -> Lua)
*/

/*
** t[k] = value at the top of the stack (where 'k' is a string)
*/
static void auxsetstr (lua_State *L, const TValue *t, const char *k) {
  TValue key;
  LuaNativeRoot object;
  TString *str;
  luaR_pushroot(L, &object);
  luaR_setroot(L, &object, t);
  t = &object.value;
  str = luaS_new(L, k);
  api_checkpop(L, 1);
  setsvalue(L, &key, str);
  luaV_settable(L, t, &key, s2v(L->top.p - 1));
  L->top.p--;  /* pop value */
  luaR_poproot(L, &object);
}


LUA_API void lua_setglobal (lua_State *L, const char *name) {
  TValue gt;
  lua_lock(L);
  getGlobalTable(L, &gt);
  auxsetstr(L, &gt, name);
  lua_unlock(L);
}


LUA_API void lua_settable (lua_State *L, int idx) {
  LuaNativeRoot indexed;
  TValue *t;
  lua_lock(L);
  api_checkpop(L, 2);
  t = luaA_indexvalue(L, idx, &indexed);
  luaV_settable(L, t, s2v(L->top.p - 2), s2v(L->top.p - 1));
  L->top.p -= 2;  /* pop index and value */
  luaR_poproot(L, &indexed);
  lua_unlock(L);
}


LUA_API void lua_setfield (lua_State *L, int idx, const char *k) {
  LuaNativeRoot indexed;
  if (localheap(L, idx)) {
    uint64_t state;
    Table *t = localtable(L, idx, &state);
    if (t != NULL) {
      TString *name = luaS_trycachedname(L, k);
      if (name != NULL) {
        TValue key;
        api_checkpop(L, 1);
        setsvalue(L, &key, name);
        if (luaH_trytasksetshort(L, t, state, &key, s2v(L->top.p - 1), 0)) {
          L->top.p--;
          luaX_leavebounded(L);
          return;
        }
      }
    }
    luaX_leavebounded(L);
  }
  lua_lock(L);
  auxsetstr(L, luaA_indexvalue(L, idx, &indexed), k);
  luaR_poproot(L, &indexed);
  lua_unlock(L);
}


LUA_API void lua_seti (lua_State *L, int idx, lua_Integer n) {
  LuaNativeRoot indexed;
  TValue *t, key;
  lua_lock(L);
  api_checkpop(L, 1);
  t = luaA_indexvalue(L, idx, &indexed);
  setivalue(&key, n);
  luaV_settable(L, t, &key, s2v(L->top.p - 1));
  L->top.p--;  /* pop value */
  luaR_poproot(L, &indexed);
  lua_unlock(L);
}


static void aux_rawset (lua_State *L, int idx, TValue *key, int n) {
  LuaNativeRoot indexed;
  Table *t;
  lua_lock(L);
  api_checkpop(L, n);
  t = gettable(L, idx, &indexed);
  if (key == NULL) key = s2v(L->top.p - 2);
  luaH_set(L, t, key, s2v(L->top.p - 1));
  L->top.p -= n;
  luaR_poproot(L, &indexed);
  lua_unlock(L);
}


LUA_API void lua_rawset (lua_State *L, int idx) {
  if (localheap(L, idx)) {
    uint64_t state;
    Table *t = localtable(L, idx, &state);
    api_checkpop(L, 2);
    if (t != NULL && ttisshrstring(s2v(L->top.p - 2)) &&
        luaH_trytasksetshort(L, t, state, s2v(L->top.p - 2),
                            s2v(L->top.p - 1), 1)) {
      L->top.p -= 2;
      luaX_leavebounded(L);
      return;
    }
    luaX_leavebounded(L);
  }
  aux_rawset(L, idx, NULL, 2);
}


LUA_API void lua_rawsetp (lua_State *L, int idx, const void *p) {
  TValue k;
  setpvalue(&k, cast_voidp(p));
  aux_rawset(L, idx, &k, 1);
}


LUA_API void lua_rawseti (lua_State *L, int idx, lua_Integer n) {
  LuaNativeRoot indexed;
  Table *t;
  if (localheap(L, idx)) {
    uint64_t state;
    api_checkpop(L, 1);
    t = localarray(L, idx, n, &state);
    if (t != NULL) {
      TValue *value = s2v(L->top.p - 1);
      if (!iscollectable(value) || luaC_trytaskedge(L, t, state, gcvalue(value))) {
        obj2arr(t, cast_uint(n - 1), value);
        L->top.p--;
        luaX_leavebounded(L);
        return;
      }
    }
    luaX_leavebounded(L);
  }
  lua_lock(L);
  api_checkpop(L, 1);
  t = gettable(L, idx, &indexed);
  luaH_setint(L, t, n, s2v(L->top.p - 1));
  L->top.p--;
  luaR_poproot(L, &indexed);
  lua_unlock(L);
}


static void setmetatableowned (lua_State *L, int objindex) {
  LuaNativeRoot indexed;
  TValue *obj;
  Table *mt;
  api_checkpop(L, 1);
  obj = luaA_indexvalue(L, objindex, &indexed);
  if (ttisnil(s2v(L->top.p - 1)))
    mt = NULL;
  else {
    api_check(L, ttistable(s2v(L->top.p - 1)), "table expected");
    mt = hvalue(s2v(L->top.p - 1));
  }
  if (!(mt != NULL && ttisfulluserdata(obj) &&
        luaA_bindprivateuserdata(L, uvalue(obj), mt)))
    luaA_setmetatable(L, obj, mt);
  L->top.p--;
  luaR_poproot(L, &indexed);
}


LUA_API int lua_setmetatable (lua_State *L, int objindex) {
  lua_lock(L);
  setmetatableowned(L, objindex);
  lua_unlock(L);
  return 1;
}


void luaA_setnamedmeta (lua_State *L, const char *name) {
  TValue registry, value;
  lua_lock(L);
  if (luaA_tryregistry(G(L), &registry) &&
      readnamedfield(L, &registry, name, &value)) {
    setobj2s(L, L->top.p, &value);
    api_incr_top(L);
  }
  else {
    LuaNativeRoot root;
    luaR_pushroot(L, &root);
    luaA_getregistry(L, &root);
    luaA_pushfield(L, &root.value, name);
    luaR_poproot(L, &root);
  }
  setmetatableowned(L, -2);
  lua_unlock(L);
}


LUA_API int lua_setiuservalue (lua_State *L, int idx, int n) {
  LuaNativeRoot indexed;
  TValue *o;
  LuaObjectScope scope;
  int res;
  lua_lock(L);
  api_checkpop(L, 1);
  o = luaA_indexvalue(L, idx, &indexed);
  api_check(L, ttisfulluserdata(o), "full userdata expected");
  if (cast_uint(n) - 1u < cast_uint(uvalue(o)->nuvalue) &&
      luaC_taskuserdata(gcvalue(o)) && iscollectable(s2v(L->top.p - 1)))
    luaC_sealtask(L);
  luaA_begin(L, gcvalue(o), &scope);
  if (!(cast_uint(n) - 1u < cast_uint(uvalue(o)->nuvalue)))
    res = 0;  /* 'n' not in [1, uvalue(o)->nuvalue] */
  else {
    luaA_publish(L, gcvalue(o), s2v(L->top.p - 1));
    luaC_prewrite(L, &uvalue(o)->uv[n - 1].uv, s2v(L->top.p - 1));
    luaA_store(L, &uvalue(o)->uv[n - 1].uv, s2v(L->top.p - 1));
    luaC_barrierback(L, gcvalue(o), s2v(L->top.p - 1));
    res = 1;
  }
  luaA_end(L, &scope);
  L->top.p--;
  luaR_poproot(L, &indexed);
  lua_unlock(L);
  return res;
}


/*
** 'load' and 'call' functions (run Lua code)
*/


LUA_API void lua_callk (lua_State *L, int nargs, int nresults,
                        lua_KContext ctx, lua_KFunction k) {
  StkId func;
  if (k == NULL) {
    if (luaX_callenter(L, nargs, nresults)) return;
  }
  else lua_lock(L);
  api_check(L, k == NULL || !isLua(L->ci),
    "cannot use continuations inside hooks");
  api_checkpop(L, nargs + 1);
  api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
  api_checkresults(L, nargs, nresults);
  func = L->top.p - (nargs+1);
  if (k != NULL && yieldable(L)) {  /* need to prepare continuation? */
    L->ci->u.c.k = k;  /* save continuation */
    L->ci->u.c.ctx = ctx;  /* save context */
    luaD_call(L, func, nresults);  /* do the call */
  }
  else  /* no continuation or no yieldable */
    luaD_callnoyield(L, func, nresults);  /* just do the call */
  adjustresults(L, nresults);
  lua_unlock(L);
}



/*
** Execute a protected call.
*/
struct CallS {  /* data to 'f_call' */
  StkId func;
  int nresults;
};


static void f_call (lua_State *L, void *ud) {
  struct CallS *c = cast(struct CallS *, ud);
  luaD_callnoyield(L, c->func, c->nresults);
}



LUA_API int lua_pcallk (lua_State *L, int nargs, int nresults, int errfunc,
                        lua_KContext ctx, lua_KFunction k) {
  struct CallS c;
  TStatus status;
  ptrdiff_t func;
  if (k == NULL && errfunc == 0) {
    if (luaX_callenter(L, nargs, nresults)) return LUA_OK;
  }
  else lua_lock(L);
  api_check(L, k == NULL || !isLua(L->ci),
    "cannot use continuations inside hooks");
  api_checkpop(L, nargs + 1);
  api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
  api_checkresults(L, nargs, nresults);
  if (errfunc == 0)
    func = 0;
  else {
    StkId o = index2stack(L, errfunc);
    api_check(L, ttisfunction(s2v(o)), "error handler must be a function");
    func = savestack(L, o);
  }
  c.func = L->top.p - (nargs+1);  /* function to be called */
  /* A committed scalar call cannot yield, raise or invoke the error
  ** handler. A rejected probe still needs the ordinary protected frame. */
  if (ttisLclosure(s2v(c.func))) {
    const Proto *p = clLvalue(s2v(c.func))->p;
    if (p->scalar_ticks != 0 &&
        luaD_tryscalarcall(L, c.func, p, nargs, nresults, luaX_boundcontext(L))) {
      adjustresults(L, nresults);
      lua_unlock(L);
      return LUA_OK;
    }
  }
  if (k == NULL || !yieldable(L)) {  /* no continuation or no yieldable? */
    c.nresults = nresults;  /* do a 'conventional' protected call */
    status = luaD_pcall(L, f_call, &c, savestack(L, c.func), func);
  }
  else {  /* prepare continuation (call is already protected by 'resume') */
    CallInfo *ci = L->ci;
    ci->u.c.k = k;  /* save continuation */
    ci->u.c.ctx = ctx;  /* save context */
    /* save information for error recovery */
    ci->u2.funcidx = cast_int(savestack(L, c.func));
    ci->u.c.old_errfunc = L->errfunc;
    L->errfunc = func;
    setoah(ci, L->allowhook);  /* save value of 'allowhook' */
    ci->callstatus |= CIST_YPCALL;  /* function can do error recovery */
    luaD_call(L, c.func, nresults);  /* do the call */
    ci->callstatus &= ~CIST_YPCALL;
    L->errfunc = ci->u.c.old_errfunc;
    status = LUA_OK;  /* if it is here, there were no errors */
  }
  adjustresults(L, nresults);
  lua_unlock(L);
  return APIstatus(status);
}


int luaA_load (lua_State *L, lua_Reader reader, void *data,
               const char *chunkname, const char *mode, int managed) {
  ZIO z;
  TStatus status;
  lua_lock(L);
  luaC_checkGC(L);
  if (!chunkname) chunkname = "?";
  luaZ_init(L, &z, reader, data, managed);
  status = luaD_protectedparser(L, &z, chunkname, mode);
  if (status == LUA_OK) {  /* no errors? */
    LuaNativeRoot function;
    LClosure *f;
    luaR_pushroot(L, &function);
    luaR_setroot(L, &function, s2v(L->top.p - 1));
    f = clLvalue(&function.value);
    if (f->nupvalues >= 1) {  /* does it have an upvalue? */
      /* get global table from registry */
      TValue gt;
      getGlobalTable(L, &gt);
      /* set global table as 1st upvalue of 'f' (may be LUA_ENV) */
      luaF_writeupval(L, __atomic_load_n(&f->upvals[0], __ATOMIC_ACQUIRE), &gt);
    }
    luaR_poproot(L, &function);
  }
  lua_unlock(L);
  return APIstatus(status);
}

LUA_API int lua_load (lua_State *L, lua_Reader reader, void *data,
                      const char *chunkname, const char *mode) {
  return luaA_load(L, reader, data, chunkname, mode, 0);
}

/*
** Dump a Lua function, calling 'writer' to write its parts. Ensure
** the stack returns with its original size.
*/
LUA_API int lua_dump (lua_State *L, lua_Writer writer, void *data, int strip) {
  LuaNativeRoot function;
  int status;
  ptrdiff_t otop;
  TValue *f;
  lua_lock(L);
  otop = savestack(L, L->top.p);
  api_checkpop(L, 1);
  luaR_pushroot(L, &function);
  luaR_setroot(L, &function, s2v(L->top.p - 1));
  f = &function.value;
  api_check(L, isLfunction(f), "Lua function expected");
  status = luaU_dump(L, clLvalue(f)->p, writer, data, strip, 0);
  L->top.p = restorestack(L, otop);  /* restore top */
  luaR_poproot(L, &function);
  lua_unlock(L);
  return status;
}


LUA_API int lua_status (lua_State *L) {
  int result;
  uintptr_t owner = luaX_valueenter(L, 1);
  result = APIstatus(L->status);
  luaX_valueleave(L, owner);
  return result;
}


/*
** Garbage-collection function
*/
LUA_API int lua_gc (lua_State *L, int what, ...) {
  va_list argp;
  int res = 0;
  global_State *g = G(L);
  LuaGCFrame gcframe;
  lua_lock(L);
  if (luaC_ingc(L)) {
    lua_unlock(L);
    return -1;
  }
  if (what == LUA_GCSTEP) {
    l_mem bytes;
    va_start(argp, what);
    bytes = cast(l_mem, va_arg(argp, size_t));
    va_end(argp);
    res = luaC_userstep(L, bytes);
    lua_unlock(L);
    return res;
  }
  if (what == LUA_GCCOLLECT) {
    if (g->gcstp & (GCSTPGC | GCSTPCLS)) res = -1;
    else luaC_fullgc(L, 0);
    lua_unlock(L);
    return res;
  }
  luaC_entergc(L, &gcframe);
  do {
    luaC_waittrace(L);
    luaW_stop(L);
    if (!luaC_tracing(L)) break;
    luaW_start(L);
  } while (1);
  if (g->gcstp & (GCSTPGC | GCSTPCLS)) {  /* internal stop? */
    luaW_start(L);
    luaC_leavegc(L, &gcframe);
    lua_unlock(L);
    return -1;  /* all options are invalid when stopped */
  }
  va_start(argp, what);
  switch (what) {
    case LUA_GCSTOP: {
      g->gcstp = GCSTPUSR;  /* stopped by the user */
      __atomic_store_n(&g->sweep_workers, 0, __ATOMIC_RELEASE);
      break;
    }
    case LUA_GCRESTART: {
      luaE_setdebt(g, 0);
      g->gcstp = 0;  /* (other bits must be zero here) */
      __atomic_store_n(&g->sweep_workers, luaC_sweeping(L) &&
        luaC_sweepready(g), __ATOMIC_RELEASE);
      break;
    }
    case LUA_GCCOUNT: {
      /* GC values are expressed in Kbytes: #bytes/2^10 */
      res = cast_int(gettotalbytes(g) >> 10);
      break;
    }
    case LUA_GCCOUNTB: {
      res = cast_int(gettotalbytes(g) & 0x3ff);
      break;
    }
    case LUA_GCISRUNNING: {
      res = gcrunning(g);
      break;
    }
    case LUA_GCGEN: {
      res = (luaC_kind(g) == KGC_INC) ? LUA_GCINC : LUA_GCGEN;
      luaC_changemode(L, KGC_GENMINOR);
      break;
    }
    case LUA_GCINC: {
      res = (luaC_kind(g) == KGC_INC) ? LUA_GCINC : LUA_GCGEN;
      luaC_changemode(L, KGC_INC);
      break;
    }
    case LUA_GCPARAM: {
      int param = va_arg(argp, int);
      int value = va_arg(argp, int);
      api_check(L, 0 <= param && param < LUA_GCPN, "invalid parameter");
      res = cast_int(luaO_applyparam(g->gcparams[param], 100));
      if (value >= 0) {
        lu_byte setting = luaO_codeparam(cast_uint(value));
        if (setting != g->gcparams[param]) {
          g->gcparams[param] = setting;
          if (param == LUA_GCPMINORMUL || param == LUA_GCPMINORMAJOR)
            luaC_noteyoung(g);
        }
      }
      break;
    }
    default: res = -1;  /* invalid option */
  }
  va_end(argp);
  luaW_start(L);
  luaC_leavegc(L, &gcframe);
  lua_unlock(L);
  return res;
}



/*
** miscellaneous functions
*/


LUA_API int lua_error (lua_State *L) {
  TValue *errobj;
  lua_lock(L);
  errobj = s2v(L->top.p - 1);
  api_checkpop(L, 1);
  /* error object is the memory error message? */
  if (ttisshrstring(errobj) && eqshrstr(tsvalue(errobj), G(L)->memerrmsg))
    luaM_error(L);  /* raise a memory error */
  else
    luaG_errormsg(L);  /* raise a regular error */
  /* code unreachable; will unlock when control actually leaves the kernel */
  return 0;  /* to avoid warnings */
}


LUA_API int lua_next (lua_State *L, int idx) {
  LuaNativeRoot indexed;
  Table *t;
  int more;
  lua_lock(L);
  api_checkpop(L, 1);
  t = gettable(L, idx, &indexed);
  more = luaH_next(L, t, L->top.p - 1);
  if (more)
    api_incr_top(L);
  else  /* no more elements */
    L->top.p--;  /* pop key */
  luaR_poproot(L, &indexed);
  lua_unlock(L);
  return more;
}


LUA_API void lua_toclose (lua_State *L, int idx) {
  StkId o;
  lua_lock(L);
  o = index2stack(L, idx);
  api_check(L, L->tbclist.p < o, "given index below or equal a marked one");
  luaF_newtbcupval(L, o);  /* create new to-be-closed upvalue */
  L->ci->callstatus |= CIST_TBC;  /* mark that function has TBC slots */
  lua_unlock(L);
}


LUA_API void lua_concat (lua_State *L, int n) {
  lua_lock(L);
  if (n > 0) {
    api_checkpop(L, n);
    luaV_concat(L, n);
    luaC_checkGC(L);
  }
  else {  /* nothing to concatenate */
    setsvalue2s(L, L->top.p, luaS_newlstr(L, "", 0));  /* push empty string */
    api_incr_top(L);
  }
  lua_unlock(L);
}


LUA_API void lua_len (lua_State *L, int idx) {
  LuaNativeRoot snapshot;
  TValue *t;
  lua_lock(L);
  t = luaA_indexvalue(L, idx, &snapshot);
  luaV_objlen(L, L->top.p, t);
  api_incr_top(L);
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
}


LUA_API lua_Alloc lua_getallocf (lua_State *L, void **ud) {
  lua_Alloc f;
  lua_lock(L);
  if (ud) *ud = G(L)->allocator.context;
  f = G(L)->allocator.function;
  lua_unlock(L);
  return f;
}


LUA_API void lua_setallocf (lua_State *L, lua_Alloc f, void *ud) {
  lua_lock(L);
  luaW_stop(L);
  if (f != luaAL_builtin) luaM_drainsparepages(L);
  G(L)->allocator.context = ud;
  G(L)->allocator.function = f;
  luaW_start(L);
  lua_unlock(L);
}


void lua_setwarnf (lua_State *L, lua_WarnFunction f, void *ud) {
  LuaObjectScope access;
  lua_lock(L);
  luaA_beginlock(L, &G(L)->warning_access, &access);
  G(L)->ud_warn = ud;
  G(L)->warnf = f;
  luaA_end(L, &access);
  lua_unlock(L);
}


void lua_warning (lua_State *L, const char *msg, int tocont) {
  lua_lock(L);
  luaE_warning(L, msg, tocont);
  lua_unlock(L);
}



LUA_API void *lua_newuserdatauv (lua_State *L, size_t size, int nuvalue) {
  Udata *u;
  void *result;
  lua_lock(L);
  api_check(L, 0 <= nuvalue && nuvalue < SHRT_MAX, "invalid value");
  u = luaS_newudata(L, size, cast(unsigned short, nuvalue));
  setuvalue(L, s2v(L->top.p), u);
  api_incr_top(L);
  luaC_checkGC(L);
  result = getudatamem(u);
  lua_unlock(L);
  return result;
}



static const char *aux_upvalue (lua_State *L, TValue *fi, int n, TValue **val,
                                LuaNativeRoot *owner) {
  switch (ttypetag(fi)) {
    case LUA_VCCL: {  /* C closure */
      CClosure *f = clCvalue(fi);
      if (!(cast_uint(n) - 1u < cast_uint(f->nupvalues)))
        return NULL;  /* 'n' not in [1, f->nupvalues] */
      *val = &f->upvalue[n-1];
      setgcovalue(L, &owner->value, obj2gco(f));
      return "";
    }
    case LUA_VLCL: {  /* Lua closure */
      LClosure *f = clLvalue(fi);
      TString *name;
      Proto *p = f->p;
      if (!(cast_uint(n) - 1u  < cast_uint(p->sizeupvalues)))
        return NULL;  /* 'n' not in [1, p->sizeupvalues] */
      *val = NULL;  /* copy Lua captures under their owner/metadata gates */
      luaF_getcapture(L, f, n - 1, owner);
      name = p->upvalues[n-1].name;
      return (name == NULL) ? "(no name)" : getstr(name);
    }
    default: return NULL;  /* not a closure */
  }
}


LUA_API const char *lua_getupvalue (lua_State *L, int funcindex, int n) {
  const char *name;
  TValue *val = NULL;  /* to avoid warnings */
  LuaNativeRoot function, capture;
  lua_lock(L);
  (void)luaA_indexvalue(L, funcindex, &function);
  luaR_pushroot(L, &capture);
  name = aux_upvalue(L, &function.value, n, &val, &capture);
  if (name) {
    GCObject *owner = gcvalue(&capture.value);
    if (owner->tt == LUA_VUPVAL) {
      luaF_loadupval(L, gco2upv(owner), s2v(L->top.p));
      luaC_tracevalue(L, s2v(L->top.p));
    }
    else {
      LuaObjectScope scope;
      luaA_begin(L, owner, &scope);
      setobj2s(L, L->top.p, val);
      luaA_end(L, &scope);
    }
    api_incr_top(L);
  }
  luaR_poproot(L, &capture);
  luaR_poproot(L, &function);
  lua_unlock(L);
  return name;
}


LUA_API const char *lua_setupvalue (lua_State *L, int funcindex, int n) {
  const char *name;
  TValue *val = NULL;  /* to avoid warnings */
  LuaNativeRoot function, capture;
  TValue *fi;
  lua_lock(L);
  (void)luaA_indexvalue(L, funcindex, &function);
  luaR_pushroot(L, &capture);
  fi = &function.value;
  api_checkpop(L, 1);
  name = aux_upvalue(L, fi, n, &val, &capture);
  if (name) {
    GCObject *owner = gcvalue(&capture.value);
    if (owner->tt == LUA_VUPVAL)
      luaF_writeupval(L, gco2upv(owner), s2v(L->top.p - 1));
    else {
      LuaObjectScope scope;
      luaA_begin(L, owner, &scope);
      luaA_publish(L, owner, s2v(L->top.p - 1));
      luaC_prewrite(L, val, s2v(L->top.p - 1));
      luaA_store(L, val, s2v(L->top.p - 1));
      luaC_barrier(L, owner, val);
      luaA_end(L, &scope);
    }
    L->top.p--;
  }
  luaR_poproot(L, &capture);
  luaR_poproot(L, &function);
  lua_unlock(L);
  return name;
}


static LClosure *getupvalclosure (lua_State *L, int fidx, int n,
                                 LuaNativeRoot *function) {
  TValue *fi = luaA_indexvalue(L, fidx, function);
  LClosure *f;
  api_check(L, ttisLclosure(fi), "Lua function expected");
  f = clLvalue(fi);
  api_check(L, 1 <= n && n <= f->p->sizeupvalues, "invalid upvalue index");
  UNUSED(n);  /* range checking is optional in release builds */
  return f;
}


LUA_API void *lua_upvalueid (lua_State *L, int fidx, int n) {
  LuaNativeRoot indexed;
  TValue *fi;
  void *result = NULL;
  lua_lock(L);
  fi = luaA_indexvalue(L, fidx, &indexed);
  switch (ttypetag(fi)) {
    case LUA_VLCL: {  /* lua closure */
      LClosure *f = clLvalue(fi);
      if (1 <= n && n <= f->p->sizeupvalues) {
        LuaNativeRoot capture;
        luaR_pushroot(L, &capture);
        luaF_getcapture(L, f, n - 1, &capture);
        result = gcvalue(&capture.value);
        luaR_poproot(L, &capture);
      }
      break;
    }
    case LUA_VCCL: {  /* C closure */
      CClosure *f = clCvalue(fi);
      if (1 <= n && n <= f->nupvalues)
        result = &f->upvalue[n - 1];
      break;
    }
    case LUA_VLCF: case LUA_VMCF:
      break;  /* light C functions have no upvalues */
    default: {
      api_check(L, 0, "function expected");
      break;
    }
  }
  luaR_poproot(L, &indexed);
  lua_unlock(L);
  return result;
}


LUA_API void lua_upvaluejoin (lua_State *L, int fidx1, int n1,
                                            int fidx2, int n2) {
  LuaNativeRoot first, second;
  LClosure *f1, *f2;
  lua_lock(L);
  f1 = getupvalclosure(L, fidx1, n1, &first);
  f2 = getupvalclosure(L, fidx2, n2, &second);
  luaF_joincaptures(L, f1, n1 - 1, f2, n2 - 1);
  luaR_poproot(L, &second);
  luaR_poproot(L, &first);
  lua_unlock(L);
}


