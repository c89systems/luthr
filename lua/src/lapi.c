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
#include "laccess.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lroots.h"
#include "lstring.h"
#include "ltable.h"
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
** index2value snapshots; only the guarded helpers below borrow addresses.
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
  else if (idx == LUA_REGISTRYINDEX)
    return &G(L)->l_registry;
  else {  /* upvalues */
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
static TValue *index2value (lua_State *L, int idx, LuaNativeRoot *snapshot) {
  TValue *source;
  int valid;
  luaR_pushroot(L, snapshot);
  if (isupvalue(idx) && ttisCclosure(s2v(L->ci->func.p))) {
    LuaObjectScope scope;
    luaA_begin(L, gcvalue(s2v(L->ci->func.p)), &scope);
    source = index2addr(L, idx);
    valid = isvalid(L, source);
    luaR_setroot(L, snapshot, source);
    luaA_end(L, &scope);
  }
  else {
    if (idx == LUA_REGISTRYINDEX) luaW_heapenter(L);
    source = index2addr(L, idx);
    valid = isvalid(L, source);
    luaR_setroot(L, snapshot, source);
    if (idx == LUA_REGISTRYINDEX) luaW_heapleave(L);
  }
  return valid ? &snapshot->value : &G(L)->nilvalue;
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
    setobj(L, target, value);
    luaC_barrier(L, closure, value);
    luaA_end(L, &scope);
  }
  else {
    if (idx == LUA_REGISTRYINDEX) luaW_heapenter(L);
    target = index2addr(L, idx);
    api_check(L, isvalid(L, target), "invalid index");
    setobj(L, target, value);
    /* The collector revisits the registry before completing collection. */
    if (idx == LUA_REGISTRYINDEX) luaW_heapleave(L);
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
      setobj(L, target, &value->value);
      luaC_barrier(L, closure, &value->value);
      converted = 1;
    }
    luaA_end(L, &scope);
  }
  else {
    if (idx == LUA_REGISTRYINDEX) {
      luaW_heapenter(L);
      luaR_setroot(L, value, &G(L)->l_registry);
    }
    if (cvt2str(&value->value)) {
      luaO_tostring(L, &value->value);
      writeindex(L, idx, &value->value);
      converted = 1;
    }
    if (idx == LUA_REGISTRYINDEX) luaW_heapleave(L);
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


LUA_API int lua_checkstack (lua_State *L, int n) {
  int res;
  CallInfo *ci;
  lua_lock(L);
  ci = L->ci;
  api_check(L, n >= 0, "negative 'n'");
  if (L->stack_last.p - L->top.p > n)  /* stack large enough? */
    res = 1;  /* yes; check is OK */
  else  /* need to grow stack */
    res = luaD_growstack(L, n, 0);
  if (res && ci->top.p < L->top.p + n)
    ci->top.p = L->top.p + n;  /* adjust frame top */
  lua_unlock(L);
  return res;
}


LUA_API void lua_xmove (lua_State *from, lua_State *to, int n) {
  int i;
  if (from == to) return;
  lua_lock(to);
  api_checkpop(from, n);
  api_check(from, G(from) == G(to), "moving among independent states");
  api_check(from, to->ci->top.p - to->top.p >= n, "stack overflow");
  from->top.p -= n;
  for (i = 0; i < n; i++) {
    setobjs2s(to, to->top.p, from->top.p + i);
    to->top.p++;  /* stack already checked by previous 'api_check' */
  }
  lua_unlock(to);
}


LUA_API lua_CFunction lua_atpanic (lua_State *L, lua_CFunction panicf) {
  lua_CFunction old;
  lua_lock(L);
  old = G(L)->panic;
  G(L)->panic = panicf;
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
  lua_lock(L);
  result = cast_int(L->top.p - L->ci->func.p) + idx;
  lua_unlock(L);
  return result;
}


LUA_API int lua_gettop (lua_State *L) {
  int result;
  lua_lock(L);
  result = cast_int(L->top.p - (L->ci->func.p + 1));
  lua_unlock(L);
  return result;
}


LUA_API void lua_settop (lua_State *L, int idx) {
  CallInfo *ci;
  StkId func, newtop;
  ptrdiff_t diff;  /* difference for new top */
  lua_lock(L);
  ci = L->ci;
  func = ci->func.p;
  if (idx >= 0) {
    api_check(L, idx <= ci->top.p - (func + 1), "new top too large");
    diff = ((func + 1) + idx) - L->top.p;
    for (; diff > 0; diff--)
      setnilvalue2s(L->top.p++);  /* clear new slots */
  }
  else {
    api_check(L, -(idx+1) <= (L->top.p - (func + 1)), "invalid new top");
    diff = idx + 1;  /* will "subtract" index (as it is negative) */
  }
  newtop = L->top.p + diff;
  if (diff < 0 && L->tbclist.p >= newtop) {
    lua_assert(ci->callstatus & CIST_TBC);
    newtop = luaF_close(L, newtop, CLOSEKTOP, 0);
  }
  L->top.p = newtop;  /* correct top only after closing any upvalue */
  lua_unlock(L);
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
  lua_lock(L);
  t = L->top.p - 1;  /* end of stack segment being rotated */
  p = index2stack(L, idx);  /* start of segment */
  api_check(L, L->tbclist.p < p, "moving a to-be-closed slot");
  api_check(L, (n >= 0 ? n : -n) <= (t - p + 1), "invalid 'n'");
  m = (n >= 0 ? t - n : p - n - 1);  /* end of prefix */
  reverse(L, p, m);  /* reverse the prefix with length 'n' */
  reverse(L, m + 1, t);  /* reverse the suffix */
  reverse(L, p, t);  /* reverse the entire segment */
  lua_unlock(L);
}


LUA_API void lua_copy (lua_State *L, int fromidx, int toidx) {
  LuaNativeRoot source;
  TValue *fr;
  lua_lock(L);
  fr = index2value(L, fromidx, &source);
  if (fromidx == toidx) {
    api_check(L, isvalid(L, fr), "invalid index");
  }
  else
    writeindex(L, toidx, fr);
  luaR_poproot(L, &source);
  lua_unlock(L);
}


LUA_API void lua_pushvalue (lua_State *L, int idx) {
  LuaNativeRoot snapshot;
  lua_lock(L);
  setobj2s(L, L->top.p, index2value(L, idx, &snapshot));
  api_incr_top(L);
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
}



/*
** access functions (stack -> C)
*/


LUA_API int lua_type (lua_State *L, int idx) {
  LuaNativeRoot snapshot;
  const TValue *o;
  int result;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  result = isvalid(L, o) ? ttype(o) : LUA_TNONE;
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
  return result;
}


LUA_API const char *lua_typename (lua_State *L, int t) {
  UNUSED(L);
  api_check(L, LUA_TNONE <= t && t < LUA_NUMTYPES, "invalid type");
  return ttypename(t);
}


LUA_API int lua_iscfunction (lua_State *L, int idx) {
  LuaNativeRoot snapshot;
  const TValue *o;
  int result;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  result = ttislcf(o) || ttisCclosure(o);
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
  return result;
}


LUA_API int lua_isinteger (lua_State *L, int idx) {
  LuaNativeRoot snapshot;
  const TValue *o;
  int result;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  result = ttisinteger(o);
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
  return result;
}


LUA_API int lua_isnumber (lua_State *L, int idx) {
  LuaNativeRoot snapshot;
  lua_Number n;
  const TValue *o;
  int result;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  result = tonumber(o, &n);
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
  return result;
}


LUA_API int lua_isstring (lua_State *L, int idx) {
  LuaNativeRoot snapshot;
  const TValue *o;
  int result;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  result = ttisstring(o) || cvt2str(o);
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
  return result;
}


LUA_API int lua_isuserdata (lua_State *L, int idx) {
  LuaNativeRoot snapshot;
  const TValue *o;
  int result;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  result = ttisfulluserdata(o) || ttislightuserdata(o);
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
  return result;
}


LUA_API int lua_rawequal (lua_State *L, int index1, int index2) {
  LuaNativeRoot first, second;
  const TValue *o1, *o2;
  int result;
  lua_lock(L);
  o1 = index2value(L, index1, &first);
  if (index1 == index2) {
    luaR_pushroot(L, &second);
    luaR_setroot(L, &second, o1);
    o2 = isvalid(L, o1) ? &second.value : &G(L)->nilvalue;
  }
  else
    o2 = index2value(L, index2, &second);
  result = (isvalid(L, o1) && isvalid(L, o2)) ? luaV_rawequalobj(o1, o2) : 0;
  luaR_poproot(L, &second);
  luaR_poproot(L, &first);
  lua_unlock(L);
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
  o1 = index2value(L, index1, &first);
  if (index1 == index2) {
    luaR_pushroot(L, &second);
    luaR_setroot(L, &second, o1);
    o2 = isvalid(L, o1) ? &second.value : &G(L)->nilvalue;
  }
  else
    o2 = index2value(L, index2, &second);
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
  LuaNativeRoot snapshot;
  const TValue *o;
  unsigned len = 0;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  if (ttisnumber(o)) {
    len = luaO_tostringbuff(o, buff);
    buff[len++] = '\0';  /* add final zero */
  }
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
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
  LuaNativeRoot snapshot;
  lua_Number n = 0;
  const TValue *o;
  int isnum;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  isnum = tonumber(o, &n);
  if (pisnum)
    *pisnum = isnum;
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
  return n;
}


LUA_API lua_Integer lua_tointegerx (lua_State *L, int idx, int *pisnum) {
  LuaNativeRoot snapshot;
  lua_Integer res = 0;
  const TValue *o;
  int isnum;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  isnum = tointeger(o, &res);
  if (pisnum)
    *pisnum = isnum;
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
  return res;
}


LUA_API int lua_toboolean (lua_State *L, int idx) {
  LuaNativeRoot snapshot;
  const TValue *o;
  int result;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  result = !l_isfalse(o);
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
  return result;
}


LUA_API const char *lua_tolstring (lua_State *L, int idx, size_t *len) {
  LuaNativeRoot snapshot;
  TValue *o;
  const char *result;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  if (!ttisstring(o)) {
    if (cvt2str(o)) {
      if (stringindex(L, idx, &snapshot))
        luaC_checkGC(L);
      o = &snapshot.value;
    }
    if (!ttisstring(o)) {
      if (len != NULL) *len = 0;
      luaR_poproot(L, &snapshot);
      lua_unlock(L);
      return NULL;
    }
  }
  if (len != NULL)
    result = getlstr(tsvalue(o), *len);
  else
    result = getstr(tsvalue(o));
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
  return result;
}


LUA_API lua_Unsigned lua_rawlen (lua_State *L, int idx) {
  LuaNativeRoot snapshot;
  const TValue *o;
  lua_Unsigned result = 0;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  switch (ttypetag(o)) {
    case LUA_VSHRSTR: result = cast(lua_Unsigned, tsvalue(o)->shrlen); break;
    case LUA_VLNGSTR: result = cast(lua_Unsigned, tsvalue(o)->u.lnglen); break;
    case LUA_VUSERDATA: result = cast(lua_Unsigned, uvalue(o)->len); break;
    case LUA_VTABLE: {
      result = luaH_getn(L, hvalue(o));
      break;
    }
    default: break;
  }
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
  return result;
}


LUA_API lua_CFunction lua_tocfunction (lua_State *L, int idx) {
  LuaNativeRoot snapshot;
  const TValue *o;
  lua_CFunction result = NULL;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  if (ttislcf(o)) result = fvalue(o);
  else if (ttisCclosure(o))
    result = clCvalue(o)->f;
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
  return result;
}


l_sinline void *touserdata (const TValue *o) {
  switch (ttype(o)) {
    case LUA_TUSERDATA: return getudatamem(uvalue(o));
    case LUA_TLIGHTUSERDATA: return pvalue(o);
    default: return NULL;
  }
}


LUA_API void *lua_touserdata (lua_State *L, int idx) {
  LuaNativeRoot snapshot;
  const TValue *o;
  void *result;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  result = touserdata(o);
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
  return result;
}


LUA_API lua_State *lua_tothread (lua_State *L, int idx) {
  LuaNativeRoot snapshot;
  const TValue *o;
  lua_State *result;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  result = (!ttisthread(o)) ? NULL : thvalue(o);
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
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
  LuaNativeRoot snapshot;
  const TValue *o;
  const void *result = NULL;
  lua_lock(L);
  o = index2value(L, idx, &snapshot);
  switch (ttypetag(o)) {
    case LUA_VLCF: result = cast_voidp(cast_sizet(fvalue(o))); break;
    case LUA_VUSERDATA: case LUA_VLIGHTUSERDATA:
      result = touserdata(o); break;
    default: {
      if (iscollectable(o))
        result = gcvalue(o);
      break;
    }
  }
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
  return result;
}



/*
** push functions (C -> stack)
*/


LUA_API void lua_pushnil (lua_State *L) {
  lua_lock(L);
  setnilvalue2s(L->top.p);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushnumber (lua_State *L, lua_Number n) {
  lua_lock(L);
  setfltvalue(s2v(L->top.p), n);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushinteger (lua_State *L, lua_Integer n) {
  lua_lock(L);
  setivalue(s2v(L->top.p), n);
  api_incr_top(L);
  lua_unlock(L);
}


/*
** Pushes on the stack a string with given length. Avoid using 's' when
** 'len' == 0 (as 's' can be NULL in that case), due to later use of
** 'memcmp' and 'memcpy'.
*/
LUA_API const char *lua_pushlstring (lua_State *L, const char *s, size_t len) {
  TString *ts;
  lua_lock(L);
  ts = (len == 0) ? luaS_new(L, "") : luaS_newlstr(L, s, len);
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


LUA_API void lua_pushcclosure (lua_State *L, lua_CFunction fn, int n) {
  lua_lock(L);
  if (n == 0) {
    setfvalue(s2v(L->top.p), fn);
    api_incr_top(L);
  }
  else {
    int i;
    CClosure *cl;
    api_checkpop(L, n);
    api_check(L, n <= MAXUPVAL, "upvalue index too large");
    cl = luaF_newCclosure(L, n);
    cl->f = fn;
    for (i = 0; i < n; i++) {
      setobj2n(L, &cl->upvalue[i], s2v(L->top.p - n + i));
      /* does not need barrier because closure is white */
      lua_assert(iswhite(cl));
    }
    L->top.p -= n;
    setclCvalue(L, s2v(L->top.p), cl);
    api_incr_top(L);
    luaC_checkGC(L);
  }
  lua_unlock(L);
}


LUA_API void lua_pushboolean (lua_State *L, int b) {
  lua_lock(L);
  if (b)
    setbtvalue(s2v(L->top.p));
  else
    setbfvalue(s2v(L->top.p));
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API void lua_pushlightuserdata (lua_State *L, void *p) {
  lua_lock(L);
  setpvalue(s2v(L->top.p), p);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API int lua_pushthread (lua_State *L) {
  int ismain;
  lua_lock(L);
  setthvalue(L, s2v(L->top.p), L);
  api_incr_top(L);
  ismain = mainthread(G(L)) == L;
  lua_unlock(L);
  return ismain;
}



/*
** get functions (Lua -> stack)
*/


static int auxgetstr (lua_State *L, const TValue *t, const char *k) {
  lu_byte tag;
  LuaNativeRoot object;
  TString *str;
  luaR_pushroot(L, &object);
  luaR_setroot(L, &object, t);
  t = &object.value;
  str = luaS_new(L, k);
  luaV_fastget(t, str, s2v(L->top.p), luaH_getstr, tag);
  if (!tagisempty(tag))
    api_incr_top(L);
  else {
    setsvalue2s(L, L->top.p, str);
    api_incr_top(L);
    tag = luaV_finishget(L, t, s2v(L->top.p - 1), L->top.p - 1, tag);
  }
  luaR_poproot(L, &object);
  return novariant(tag);
}


/*
** The following function assumes that the registry cannot be a weak
** table; so, an emergency collection while using the global table
** cannot collect it.
*/
static void getGlobalTable (lua_State *L, TValue *gt) {
  LuaNativeRoot registry;
  lu_byte tag;
  (void)index2value(L, LUA_REGISTRYINDEX, &registry);
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
  result = auxgetstr(L, &gt, name);
  lua_unlock(L);
  return result;
}


LUA_API int lua_gettable (lua_State *L, int idx) {
  LuaNativeRoot indexed;
  lu_byte tag;
  TValue *t;
  lua_lock(L);
  api_checkpop(L, 1);
  t = index2value(L, idx, &indexed);
  luaV_fastget(t, s2v(L->top.p - 1), s2v(L->top.p - 1), luaH_get, tag);
  if (tagisempty(tag))
    tag = luaV_finishget(L, t, s2v(L->top.p - 1), L->top.p - 1, tag);
  luaR_poproot(L, &indexed);
  lua_unlock(L);
  return novariant(tag);
}


LUA_API int lua_getfield (lua_State *L, int idx, const char *k) {
  LuaNativeRoot indexed;
  int result;
  lua_lock(L);
  result = auxgetstr(L, index2value(L, idx, &indexed), k);
  luaR_poproot(L, &indexed);
  lua_unlock(L);
  return result;
}


LUA_API int lua_geti (lua_State *L, int idx, lua_Integer n) {
  LuaNativeRoot indexed;
  TValue *t;
  lu_byte tag;
  lua_lock(L);
  t = index2value(L, idx, &indexed);
  luaV_fastgeti(t, n, s2v(L->top.p), tag);
  if (tagisempty(tag)) {
    TValue key;
    setivalue(&key, n);
    tag = luaV_finishget(L, t, &key, L->top.p, tag);
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
  TValue *t = index2value(L, idx, indexed);
  api_check(L, ttistable(t), "table expected");
  return hvalue(t);
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


LUA_API int lua_rawgeti (lua_State *L, int idx, lua_Integer n) {
  LuaNativeRoot indexed;
  Table *t;
  lu_byte tag;
  int result;
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
  Table *t;
  lua_lock(L);
  t = luaH_new(L);
  sethvalue2s(L, L->top.p, t);
  api_incr_top(L);
  if (narray > 0 || nrec > 0)
    luaH_resize(L, t, cast_uint(narray), cast_uint(nrec));
  luaC_checkGC(L);
  lua_unlock(L);
}


LUA_API int lua_getmetatable (lua_State *L, int objindex) {
  LuaNativeRoot indexed;
  const TValue *obj;
  LuaNativeRoot mt;
  int res = 0;
  lua_lock(L);
  obj = index2value(L, objindex, &indexed);
  luaR_pushroot(L, &mt);
  luaA_getmetatable(L, obj, &mt);
  if (!ttisnil(&mt.value)) {
    setobj2s(L, L->top.p, &mt.value);
    api_incr_top(L);
    res = 1;
  }
  luaR_poproot(L, &mt);
  luaR_poproot(L, &indexed);
  lua_unlock(L);
  return res;
}


LUA_API int lua_getiuservalue (lua_State *L, int idx, int n) {
  LuaNativeRoot indexed;
  TValue *o;
  LuaObjectScope scope;
  int t;
  lua_lock(L);
  o = index2value(L, idx, &indexed);
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
  lua_unlock(L);
  return t;
}


/*
** set functions (stack -> Lua)
*/

/*
** t[k] = value at the top of the stack (where 'k' is a string)
*/
static void auxsetstr (lua_State *L, const TValue *t, const char *k) {
  int hres;
  LuaNativeRoot object;
  TString *str;
  luaR_pushroot(L, &object);
  luaR_setroot(L, &object, t);
  t = &object.value;
  str = luaS_new(L, k);
  api_checkpop(L, 1);
  luaV_fastset(t, str, s2v(L->top.p - 1), hres, luaH_psetstr);
  if (hres == HOK) {
    L->top.p--;  /* pop value */
  }
  else {
    setsvalue2s(L, L->top.p, str);  /* push 'str' (to make it a TValue) */
    api_incr_top(L);
    luaV_finishset(L, t, s2v(L->top.p - 1), s2v(L->top.p - 2), hres);
    L->top.p -= 2;  /* pop value and key */
  }
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
  int hres;
  lua_lock(L);
  api_checkpop(L, 2);
  t = index2value(L, idx, &indexed);
  luaV_fastset(t, s2v(L->top.p - 2), s2v(L->top.p - 1), hres, luaH_pset);
  if (hres != HOK)
    luaV_finishset(L, t, s2v(L->top.p - 2), s2v(L->top.p - 1), hres);
  L->top.p -= 2;  /* pop index and value */
  luaR_poproot(L, &indexed);
  lua_unlock(L);
}


LUA_API void lua_setfield (lua_State *L, int idx, const char *k) {
  LuaNativeRoot indexed;
  lua_lock(L);
  auxsetstr(L, index2value(L, idx, &indexed), k);
  luaR_poproot(L, &indexed);
  lua_unlock(L);
}


LUA_API void lua_seti (lua_State *L, int idx, lua_Integer n) {
  LuaNativeRoot indexed;
  TValue *t;
  int hres;
  lua_lock(L);
  api_checkpop(L, 1);
  t = index2value(L, idx, &indexed);
  luaV_fastseti(t, n, s2v(L->top.p - 1), hres);
  if (hres != HOK) {
    TValue temp;
    setivalue(&temp, n);
    luaV_finishset(L, t, &temp, s2v(L->top.p - 1), hres);
  }
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
  lua_lock(L);
  api_checkpop(L, 1);
  t = gettable(L, idx, &indexed);
  luaH_setint(L, t, n, s2v(L->top.p - 1));
  L->top.p--;
  luaR_poproot(L, &indexed);
  lua_unlock(L);
}


LUA_API int lua_setmetatable (lua_State *L, int objindex) {
  LuaNativeRoot indexed;
  TValue *obj;
  Table *mt;
  lua_lock(L);
  api_checkpop(L, 1);
  obj = index2value(L, objindex, &indexed);
  if (ttisnil(s2v(L->top.p - 1)))
    mt = NULL;
  else {
    api_check(L, ttistable(s2v(L->top.p - 1)), "table expected");
    mt = hvalue(s2v(L->top.p - 1));
  }
  luaA_setmetatable(L, obj, mt);
  L->top.p--;
  luaR_poproot(L, &indexed);
  lua_unlock(L);
  return 1;
}


LUA_API int lua_setiuservalue (lua_State *L, int idx, int n) {
  LuaNativeRoot indexed;
  TValue *o;
  LuaObjectScope scope;
  int res;
  lua_lock(L);
  api_checkpop(L, 1);
  o = index2value(L, idx, &indexed);
  api_check(L, ttisfulluserdata(o), "full userdata expected");
  luaA_begin(L, gcvalue(o), &scope);
  if (!(cast_uint(n) - 1u < cast_uint(uvalue(o)->nuvalue)))
    res = 0;  /* 'n' not in [1, uvalue(o)->nuvalue] */
  else {
    setobj(L, &uvalue(o)->uv[n - 1].uv, s2v(L->top.p - 1));
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


#define checkresults(L,na,nr) \
     (api_check(L, (nr) == LUA_MULTRET \
               || (L->ci->top.p - L->top.p >= (nr) - (na)), \
	"results from function overflow current stack size"), \
      api_check(L, LUA_MULTRET <= (nr) && (nr) <= MAXRESULTS,  \
                   "invalid number of results"))


LUA_API void lua_callk (lua_State *L, int nargs, int nresults,
                        lua_KContext ctx, lua_KFunction k) {
  StkId func;
  lua_lock(L);
  api_check(L, k == NULL || !isLua(L->ci),
    "cannot use continuations inside hooks");
  api_checkpop(L, nargs + 1);
  api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
  checkresults(L, nargs, nresults);
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
  lua_lock(L);
  api_check(L, k == NULL || !isLua(L->ci),
    "cannot use continuations inside hooks");
  api_checkpop(L, nargs + 1);
  api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
  checkresults(L, nargs, nresults);
  if (errfunc == 0)
    func = 0;
  else {
    StkId o = index2stack(L, errfunc);
    api_check(L, ttisfunction(s2v(o)), "error handler must be a function");
    func = savestack(L, o);
  }
  c.func = L->top.p - (nargs+1);  /* function to be called */
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


LUA_API int lua_load (lua_State *L, lua_Reader reader, void *data,
                      const char *chunkname, const char *mode) {
  ZIO z;
  TStatus status;
  lua_lock(L);
  luaC_checkGC(L);
  if (!chunkname) chunkname = "?";
  luaZ_init(L, &z, reader, data);
  status = luaD_protectedparser(L, &z, chunkname, mode);
  if (status == LUA_OK) {  /* no errors? */
    LClosure *f = clLvalue(s2v(L->top.p - 1));  /* get new function */
    if (f->nupvalues >= 1) {  /* does it have an upvalue? */
      /* get global table from registry */
      TValue gt;
      getGlobalTable(L, &gt);
      /* set global table as 1st upvalue of 'f' (may be LUA_ENV) */
      luaF_writecapture(L, f, 0, &gt);
    }
  }
  lua_unlock(L);
  return APIstatus(status);
}


/*
** Dump a Lua function, calling 'writer' to write its parts. Ensure
** the stack returns with its original size.
*/
LUA_API int lua_dump (lua_State *L, lua_Writer writer, void *data, int strip) {
  int status;
  ptrdiff_t otop;
  TValue *f;
  lua_lock(L);
  otop = savestack(L, L->top.p);
  f = s2v(L->top.p - 1);
  api_checkpop(L, 1);
  api_check(L, isLfunction(f), "Lua function expected");
  status = luaU_dump(L, clLvalue(f)->p, writer, data, strip);
  L->top.p = restorestack(L, otop);  /* restore top */
  lua_unlock(L);
  return status;
}


LUA_API int lua_status (lua_State *L) {
  int result;
  lua_lock(L);
  result = APIstatus(L->status);
  lua_unlock(L);
  return result;
}


/*
** Garbage-collection function
*/
LUA_API int lua_gc (lua_State *L, int what, ...) {
  va_list argp;
  int res = 0;
  global_State *g = G(L);
  lua_lock(L);
  luaW_stop(L);
  if (g->gcstp & (GCSTPGC | GCSTPCLS)) {  /* internal stop? */
    luaW_start(L);
    lua_unlock(L);
    return -1;  /* all options are invalid when stopped */
  }
  va_start(argp, what);
  switch (what) {
    case LUA_GCSTOP: {
      g->gcstp = GCSTPUSR;  /* stopped by the user */
      break;
    }
    case LUA_GCRESTART: {
      luaE_setdebt(g, 0);
      g->gcstp = 0;  /* (other bits must be zero here) */
      break;
    }
    case LUA_GCCOLLECT: {
      luaC_fullgc(L, 0);
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
    case LUA_GCSTEP: {
      lu_byte oldstp = g->gcstp;
      l_mem n = cast(l_mem, va_arg(argp, size_t));
      l_mem newdebt;
      int work = 0;  /* true if GC did some work */
      g->gcstp = 0;  /* allow GC to run (other bits must be zero here) */
      if (n <= 0)
        newdebt = 0;  /* force to run one basic step */
      else if (g->GCdebt >= n - MAX_LMEM)  /* no overflow? */
        newdebt = g->GCdebt - n;
      else  /* overflow */
        newdebt = -MAX_LMEM;  /* set debt to mininum value */
      luaE_setdebt(g, newdebt);
      luaC_condGC(L, (void)0, work = 1);
      if (work && g->gcstate == GCSpause)  /* end of cycle? */
        res = 1;  /* signal it */
      g->gcstp = oldstp;  /* restore previous state */
      break;
    }
    case LUA_GCISRUNNING: {
      res = gcrunning(g);
      break;
    }
    case LUA_GCGEN: {
      res = (g->gckind == KGC_INC) ? LUA_GCINC : LUA_GCGEN;
      luaC_changemode(L, KGC_GENMINOR);
      break;
    }
    case LUA_GCINC: {
      res = (g->gckind == KGC_INC) ? LUA_GCINC : LUA_GCGEN;
      luaC_changemode(L, KGC_INC);
      break;
    }
    case LUA_GCPARAM: {
      int param = va_arg(argp, int);
      int value = va_arg(argp, int);
      api_check(L, 0 <= param && param < LUA_GCPN, "invalid parameter");
      res = cast_int(luaO_applyparam(g->gcparams[param], 100));
      if (value >= 0)
        g->gcparams[param] = luaO_codeparam(cast_uint(value));
      break;
    }
    default: res = -1;  /* invalid option */
  }
  va_end(argp);
  luaW_start(L);
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
  t = index2value(L, idx, &snapshot);
  luaV_objlen(L, L->top.p, t);
  api_incr_top(L);
  luaR_poproot(L, &snapshot);
  lua_unlock(L);
}


LUA_API lua_Alloc lua_getallocf (lua_State *L, void **ud) {
  lua_Alloc f;
  lua_lock(L);
  if (ud) *ud = G(L)->ud;
  f = G(L)->frealloc;
  lua_unlock(L);
  return f;
}


LUA_API void lua_setallocf (lua_State *L, lua_Alloc f, void *ud) {
  lua_lock(L);
  G(L)->ud = ud;
  G(L)->frealloc = f;
  lua_unlock(L);
}


void lua_setwarnf (lua_State *L, lua_WarnFunction f, void *ud) {
  lua_lock(L);
  G(L)->ud_warn = ud;
  G(L)->warnf = f;
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
  (void)index2value(L, funcindex, &function);
  luaR_pushroot(L, &capture);
  name = aux_upvalue(L, &function.value, n, &val, &capture);
  if (name) {
    GCObject *owner = gcvalue(&capture.value);
    if (owner->tt == LUA_VUPVAL) {
      LuaNativeRoot snapshot;
      luaR_pushroot(L, &snapshot);
      luaF_readupval(L, gco2upv(owner), &snapshot);
      setobj2s(L, L->top.p, &snapshot.value);
      luaR_poproot(L, &snapshot);
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
  (void)index2value(L, funcindex, &function);
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
      setobj(L, val, s2v(L->top.p - 1));
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
  TValue *fi = index2value(L, fidx, function);
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
  fi = index2value(L, fidx, &indexed);
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
    case LUA_VLCF:
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


