/*
** $Id: lvm.c $
** Lua virtual machine
** See Copyright Notice in lua.h
*/

#define lvm_c
#define LUA_CORE

#include "lprefix.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "laccess.h"
#include "lbuiltin.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "ljit.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lpolicy.h"
#include "lstackapi.h"
#include "lstate.h"
#include "lroots.h"
#include "lstring.h"
#include "ltable.h"
#include "ltableaccess.h"
#include "ltm.h"
#include "ltask.h"
#include "lvm.h"



/* limit for table tag-method chains (to avoid infinite loops) */
#define MAXTAGLOOP	2000


/*
** 'l_intfitsf' checks whether a given integer is in the range that
** can be converted to a float without rounding. Used in comparisons.
*/

/* number of bits in the mantissa of a float */
#define NBM		(l_floatatt(MANT_DIG))

/*
** Check whether some integers may not fit in a float, testing whether
** (maxinteger >> NBM) > 0. (That implies (1 << NBM) <= maxinteger.)
** (The shifts are done in parts, to avoid shifting by more than the size
** of an integer. In a worst case, NBM == 113 for long double and
** sizeof(long) == 32.)
*/
#if ((((LUA_MAXINTEGER >> (NBM / 4)) >> (NBM / 4)) >> (NBM / 4)) \
	>> (NBM - (3 * (NBM / 4))))  >  0

/* limit for integers that fit in a float */
#define MAXINTFITSF	((lua_Unsigned)1 << NBM)

/* check whether 'i' is in the interval [-MAXINTFITSF, MAXINTFITSF] */
#define l_intfitsf(i)	((MAXINTFITSF + l_castS2U(i)) <= (2 * MAXINTFITSF))

#else  /* all integers fit in a float precisely */

#define l_intfitsf(i)	1

#endif


/*
** Try to convert a value from string to a number value.
** If the value is not a string or is a string not representing
** a valid numeral (or if coercions from strings to numbers
** are disabled via macro 'cvt2num'), do not modify 'result'
** and return 0.
*/
static int l_strton (const TValue *obj, TValue *result) {
  lua_assert(obj != result);
  if (!cvt2num(obj))  /* is object not a string? */
    return 0;
  else {
    TString *st = tsvalue(obj);
    size_t stlen;
    const char *s = getlstr(st, stlen);
    return (luaO_str2num(s, result) == stlen + 1);
  }
}


/*
** Try to convert a value to a float. The float case is already handled
** by the macro 'tonumber'.
*/
int luaV_tonumber_ (const TValue *obj, lua_Number *n) {
  TValue v;
  if (ttisinteger(obj)) {
    *n = cast_num(ivalue(obj));
    return 1;
  }
  else if (l_strton(obj, &v)) {  /* string coercible to number? */
    *n = nvalue(&v);  /* convert result of 'luaO_str2num' to a float */
    return 1;
  }
  else
    return 0;  /* conversion failed */
}


/*
** try to convert a float to an integer, rounding according to 'mode'.
*/
int luaV_flttointeger (lua_Number n, lua_Integer *p, F2Imod mode) {
  lua_Number f = l_floor(n);
  if (n != f) {  /* not an integral value? */
    if (mode == F2Ieq) return 0;  /* fails if mode demands integral value */
    else if (mode == F2Iceil)  /* needs ceiling? */
      f += 1;  /* convert floor to ceiling (remember: n != f) */
  }
  return lua_numbertointeger(f, p);
}


/*
** try to convert a value to an integer, rounding according to 'mode',
** without string coercion.
** ("Fast track" handled by macro 'tointegerns'.)
*/
int luaV_tointegerns (const TValue *obj, lua_Integer *p, F2Imod mode) {
  if (ttisfloat(obj))
    return luaV_flttointeger(fltvalue(obj), p, mode);
  else if (ttisinteger(obj)) {
    *p = ivalue(obj);
    return 1;
  }
  else
    return 0;
}


/*
** try to convert a value to an integer.
*/
int luaV_tointeger (const TValue *obj, lua_Integer *p, F2Imod mode) {
  TValue v;
  if (l_strton(obj, &v))  /* does 'obj' point to a numerical string? */
    obj = &v;  /* change it to point to its corresponding number */
  return luaV_tointegerns(obj, p, mode);
}


/*
** Try to convert a 'for' limit to an integer, preserving the semantics
** of the loop. Return true if the loop must not run; otherwise, '*p'
** gets the integer limit.
** (The following explanation assumes a positive step; it is valid for
** negative steps mutatis mutandis.)
** If the limit is an integer or can be converted to an integer,
** rounding down, that is the limit.
** Otherwise, check whether the limit can be converted to a float. If
** the float is too large, clip it to LUA_MAXINTEGER.  If the float
** is too negative, the loop should not run, because any initial
** integer value is greater than such limit; so, the function returns
** true to signal that. (For this latter case, no integer limit would be
** correct; even a limit of LUA_MININTEGER would run the loop once for
** an initial value equal to LUA_MININTEGER.)
*/
static int forlimit (lua_State *L, lua_Integer init, const TValue *lim,
                                   lua_Integer *p, lua_Integer step) {
  if (!luaV_tointeger(lim, p, (step < 0 ? F2Iceil : F2Ifloor))) {
    /* not coercible to in integer */
    lua_Number flim;  /* try to convert to float */
    if (!tonumber(lim, &flim)) /* cannot convert to float? */
      luaG_forerror(L, lim, "limit");
    /* else 'flim' is a float out of integer bounds */
    if (luai_numlt(0, flim)) {  /* if it is positive, it is too large */
      if (step < 0) return 1;  /* initial value must be less than it */
      *p = LUA_MAXINTEGER;  /* truncate */
    }
    else {  /* it is less than min integer */
      if (step > 0) return 1;  /* initial value must be greater than it */
      *p = LUA_MININTEGER;  /* truncate */
    }
  }
  return (step > 0 ? init > *p : init < *p);  /* not to run? */
}


/*
** Prepare a numerical for loop (opcode OP_FORPREP).
** Before execution, stack is as follows:
**   ra     : initial value
**   ra + 1 : limit
**   ra + 2 : step
** Return true to skip the loop. Otherwise,
** after preparation, stack will be as follows:
**   ra     : loop counter (integer loops) or limit (float loops)
**   ra + 1 : step
**   ra + 2 : control variable
*/
static int forprep (lua_State *L, StkId ra) {
  TValue *pinit = s2v(ra);
  TValue *plimit = s2v(ra + 1);
  TValue *pstep = s2v(ra + 2);
  if (ttisinteger(pinit) && ttisinteger(pstep)) { /* integer loop? */
    lua_Integer init = ivalue(pinit);
    lua_Integer step = ivalue(pstep);
    lua_Integer limit;
    if (step == 0)
      luaG_runerror(L, "'for' step is zero");
    if (forlimit(L, init, plimit, &limit, step))
      return 1;  /* skip the loop */
    else {  /* prepare loop counter */
      lua_Unsigned count;
      if (step > 0) {  /* ascending loop? */
        count = l_castS2U(limit) - l_castS2U(init);
        if (step != 1)  /* avoid division in the too common case */
          count /= l_castS2U(step);
      }
      else {  /* step < 0; descending loop */
        count = l_castS2U(init) - l_castS2U(limit);
        /* 'step+1' avoids negating 'mininteger' */
        count /= l_castS2U(-(step + 1)) + 1u;
      }
      /* use 'chgivalue' for places that for sure had integers */
      chgivalue(s2v(ra), l_castU2S(count));  /* change init to count */
      setivalue(s2v(ra + 1), step);  /* change limit to step */
      chgivalue(s2v(ra + 2), init);  /* change step to init */
    }
  }
  else {  /* try making all values floats */
    lua_Number init; lua_Number limit; lua_Number step;
    if (l_unlikely(!tonumber(plimit, &limit)))
      luaG_forerror(L, plimit, "limit");
    if (l_unlikely(!tonumber(pstep, &step)))
      luaG_forerror(L, pstep, "step");
    if (l_unlikely(!tonumber(pinit, &init)))
      luaG_forerror(L, pinit, "initial value");
    if (step == 0)
      luaG_runerror(L, "'for' step is zero");
    if (luai_numlt(0, step) ? luai_numlt(limit, init)
                            : luai_numlt(init, limit))
      return 1;  /* skip the loop */
    else {
      /* make sure all values are floats */
      setfltvalue(s2v(ra), limit);
      setfltvalue(s2v(ra + 1), step);
      setfltvalue(s2v(ra + 2), init);  /* control variable */
    }
  }
  return 0;
}


/*
** Execute a step of a float numerical for loop, returning
** true iff the loop must continue. (The integer case is
** written inline with opcode OP_FORLOOP, for performance.)
*/
static int floatforloop (lua_State *L, StkId ra) {
  lua_Number step = fltvalue(s2v(ra + 1));
  lua_Number limit = fltvalue(s2v(ra));
  lua_Number idx = fltvalue(s2v(ra + 2));  /* control variable */
  idx = luai_numadd(L, idx, step);  /* increment index */
  if (luai_numlt(0, step) ? luai_numle(idx, limit)
                          : luai_numle(limit, idx)) {
    chgfltvalue(s2v(ra + 2), idx);  /* update control variable */
    return 1;  /* jump back */
  }
  else
    return 0;  /* finish the loop */
}


/*
** Finish the table access 'val = t[key]' and return the tag of the result.
** 'upvalue' is the captured receiver's slot plus one, or zero for other sources.
*/
static lu_byte finishget (lua_State *L, const TValue *t, TValue *key,
                          StkId val, unsigned upvalue, int miss) {
  LuaNativeRoot object, rooted_key, method;
  TValue firstmethod;
  lu_byte tag;
  int loop, knownmethod;
  if (ttistable(t)) {
    knownmethod = luaA_tryindex(L, hvalue(t), key, s2v(val), &firstmethod,
                                &tag, miss);
    if (knownmethod && ttisnil(&firstmethod)) return tag;
  }
  else
    knownmethod = ttisfulluserdata(t)
      ? luaA_readobjectfield(L, t, G(L)->tmname[TM_INDEX], &firstmethod)
      : luaA_readtypefield(L, ttype(t), G(L)->tmname[TM_INDEX], &firstmethod);
  if (knownmethod && ttisfunction(&firstmethod)) {
    luaC_tracevalue(L, &firstmethod);
    return luaT_callTMres(L, &firstmethod, t, key, val);
  }
  /* Resolve an inherited hit with both objects retained by managed
  ** admission. If lookup must continue, keep the validated method. */
  if (knownmethod && ttistable(&firstmethod)) {
    luaC_tracevalue(L, &firstmethod);
    if (luaH_tryget(L, hvalue(&firstmethod), key, s2v(val), &tag) &&
        tag != LUA_VABSTKEY)
      return tag;
  }
  luaR_pushroot(L, &object);
  luaR_setroot(L, &object, t);
  luaR_pushroot(L, &rooted_key);
  luaR_setroot(L, &rooted_key, key);
  luaR_pushroot(L, &method);
  for (loop = 0; loop < MAXTAGLOOP; loop++) {
    if (knownmethod) {
      luaR_setroot(L, &method, &firstmethod);
      knownmethod = 0;
    }
    else if (ttistable(&object.value)) {
      tag = luaA_index(L, hvalue(&object.value), &rooted_key.value,
                       s2v(val), &method);
      if (ttisnil(&method.value))
        goto done;
    }
    else
      luaT_gettmbyobj(L, &object.value, TM_INDEX, &method);
    if (ttisnil(&method.value)) {
      if (loop == 0 && upvalue != 0)
        luaG_upvaltypeerror(L, &object.value, upvalue - 1, "index");
      /* The first receiver still names its live register/upvalue. Later
      ** receivers are metamethod snapshots, with no such source identity. */
      luaG_typeerror(L, loop == 0 ? t : &object.value, "index");
    }
    if (ttisfunction(&method.value)) {
      tag = luaT_callTMres(L, &method.value, &object.value,
                          &rooted_key.value, val);
      goto done;
    }
    luaR_setroot(L, &object, &method.value);
  }
  luaG_runerror(L, "'__index' chain too long; possible loop");
 done:
  luaR_poproot(L, &method);
  luaR_poproot(L, &rooted_key);
  luaR_poproot(L, &object);
  return tag;
}

lu_byte luaV_finishget (lua_State *L, const TValue *t, TValue *key,
                        StkId val) {
  return finishget(L, t, key, val, 0, 0);
}

static void __attribute__((noinline)) finishvmget (lua_State *L,
    const TValue *receiver, TValue *key, StkId result, unsigned upvalue,
    int retry) {
  if (retry) {
    LuaNativeRoot object, index;
    ptrdiff_t offset = savestack(L, result);
    /* A retry may park while resolving the field. Retain both operands
    ** across stack relocation and any subsequent metamethod lookup. */
    luaR_pushroot(L, &object);
    luaR_setroot(L, &object, receiver);
    luaR_pushroot(L, &index);
    luaR_setroot(L, &index, key);
    if (luaV_needsindex(luaH_get(L, hvalue(&object.value), &index.value, s2v(result))))
      finishget(L, &object.value, &index.value, restorestack(L, offset), upvalue,
                0);
    luaR_poproot(L, &index);
    luaR_poproot(L, &object);
  }
  else
    finishget(L, receiver, key, result, upvalue, 1);
}


/*
** Complete a table assignment 't[key] = val'. Roots snapshot all inputs
** once; the table operation owns lookup, insertion and resize together.
*/
static void __attribute__((noinline)) finishset (lua_State *L, const TValue *t,
                         TValue *key, TValue *val, unsigned upvalue) {
  LuaObjectScope object;
  LuaNativeRoot owner;
  LuaNativeRoot *receiver = &owner;
  LuaNativeRoot rooted_key, value, method;
  Table *table = ttistable(t) ? hvalue(t) : NULL;
  TValue other = {0};
  int loop, source = -1;
  /* Retain diagnostic identity, not a stack pointer that an ownership wait
  ** can invalidate. Type information still comes from the rooted snapshot. */
  if (table == NULL) setobj(L, &other, t);
  if (table == NULL && upvalue == 0 && isLua(L->ci)) {
    uintptr_t address = cast(uintptr_t, t);
    uintptr_t begin = cast(uintptr_t, L->ci->func.p + 1);
    uintptr_t end = cast(uintptr_t, L->ci->top.p);
    if (address >= begin && address < end &&
        (address - begin) % sizeof(StackValue) == 0)
      source = cast_int((address - begin) / sizeof(StackValue));
  }
  luaR_pushroot(L, &rooted_key);
  luaR_setroot(L, &rooted_key, key);
  luaR_pushroot(L, &value);
  luaR_setroot(L, &value, val);
  luaR_pushroot(L, receiver);
  if (table != NULL) {
    int handled;
    sethvalue(L, &receiver->value, table);
    luaC_tracepointer(L, table);
    if (__atomic_load_n(&table->sharing, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED) {
      luaA_sharevalue(L, &rooted_key.value);
      luaA_sharevalue(L, &value.value);
      if (luaH_setshared(L, table, &rooted_key.value, &value.value, 0)) goto complete;
    }
    luaA_begin(L, obj2gco(table), &object);
    handled = luaH_assignowned(L, table, &rooted_key.value, &value.value, 0);
    luaA_end(L, &object);
    if (handled) goto complete;
  }
  else {
    luaR_setroot(L, receiver, &other);
  }
  luaR_pushroot(L, &method);
  for (loop = 0; loop < MAXTAGLOOP; loop++) {
    if (ttistable(&receiver->value)) {
      if (luaA_assign(L, &receiver->value, &rooted_key.value,
                      &value.value, &method))
        goto done;
    }
    else {
      luaT_gettmbyobj(L, &receiver->value, TM_NEWINDEX, &method);
      if (ttisnil(&method.value)) {
        if (loop == 0 && upvalue != 0)
          luaG_upvaltypeerror(L, &receiver->value, upvalue - 1, "index");
        if (loop == 0 && source >= 0)
          luaG_regtypeerror(L, &receiver->value, source, "index");
        luaG_typeerror(L, &receiver->value, "index");
      }
    }
    if (ttisfunction(&method.value)) {
      luaT_callTM(L, &method.value, &receiver->value, &rooted_key.value, &value.value);
      goto done;
    }
    luaR_setroot(L, receiver, &method.value);
  }
  luaG_runerror(L, "'__newindex' chain too long; possible loop");
 done:
  luaR_poproot(L, &method);
 complete:
  luaR_poproot(L, receiver);
  luaR_poproot(L, &value);
  luaR_poproot(L, &rooted_key);
}

void luaV_settable (lua_State *L, const TValue *t, TValue *key, TValue *val) {
  if (!ttistable(t) || !luaH_tryset(L, hvalue(t), key, val, 0))
    finishset(L, t, key, val, 0);
}


/*
** Function to be used for 0-terminated string order comparison
*/
#if !defined(l_strcoll)
#define l_strcoll	strcoll
#endif


/*
** Compare two strings 'ts1' x 'ts2', returning an integer less-equal-
** -greater than zero if 'ts1' is less-equal-greater than 'ts2'.
** The code is a little tricky because it allows '\0' in the strings
** and it uses 'strcoll' (to respect locales) for each segment
** of the strings. Note that segments can compare equal but still
** have different lengths.
*/
static int l_strcmp (const TString *ts1, const TString *ts2) {
  size_t rl1;  /* real length */
  const char *s1 = getlstr(ts1, rl1);
  size_t rl2;
  const char *s2 = getlstr(ts2, rl2);
  for (;;) {  /* for each segment */
    int temp = l_strcoll(s1, s2);
    if (temp != 0)  /* not equal? */
      return temp;  /* done */
    else {  /* strings are equal up to a '\0' */
      size_t zl1 = strlen(s1);  /* index of first '\0' in 's1' */
      size_t zl2 = strlen(s2);  /* index of first '\0' in 's2' */
      if (zl2 == rl2)  /* 's2' is finished? */
        return (zl1 == rl1) ? 0 : 1;  /* check 's1' */
      else if (zl1 == rl1)  /* 's1' is finished? */
        return -1;  /* 's1' is less than 's2' ('s2' is not finished) */
      /* both strings longer than 'zl'; go on comparing after the '\0' */
      zl1++; zl2++;
      s1 += zl1; rl1 -= zl1; s2 += zl2; rl2 -= zl2;
    }
  }
}


/*
** Check whether integer 'i' is less than float 'f'. If 'i' has an
** exact representation as a float ('l_intfitsf'), compare numbers as
** floats. Otherwise, use the equivalence 'i < f <=> i < ceil(f)'.
** If 'ceil(f)' is out of integer range, either 'f' is greater than
** all integers or less than all integers.
** (The test with 'l_intfitsf' is only for performance; the else
** case is correct for all values, but it is slow due to the conversion
** from float to int.)
** When 'f' is NaN, comparisons must result in false.
*/
l_sinline int LTintfloat (lua_Integer i, lua_Number f) {
  if (l_intfitsf(i))
    return luai_numlt(cast_num(i), f);  /* compare them as floats */
  else {  /* i < f <=> i < ceil(f) */
    lua_Integer fi;
    if (luaV_flttointeger(f, &fi, F2Iceil))  /* fi = ceil(f) */
      return i < fi;   /* compare them as integers */
    else  /* 'f' is either greater or less than all integers */
      return f > 0;  /* greater? */
  }
}


/*
** Check whether integer 'i' is less than or equal to float 'f'.
** See comments on previous function.
*/
l_sinline int LEintfloat (lua_Integer i, lua_Number f) {
  if (l_intfitsf(i))
    return luai_numle(cast_num(i), f);  /* compare them as floats */
  else {  /* i <= f <=> i <= floor(f) */
    lua_Integer fi;
    if (luaV_flttointeger(f, &fi, F2Ifloor))  /* fi = floor(f) */
      return i <= fi;   /* compare them as integers */
    else  /* 'f' is either greater or less than all integers */
      return f > 0;  /* greater? */
  }
}


/*
** Check whether float 'f' is less than integer 'i'.
** See comments on previous function.
*/
l_sinline int LTfloatint (lua_Number f, lua_Integer i) {
  if (l_intfitsf(i))
    return luai_numlt(f, cast_num(i));  /* compare them as floats */
  else {  /* f < i <=> floor(f) < i */
    lua_Integer fi;
    if (luaV_flttointeger(f, &fi, F2Ifloor))  /* fi = floor(f) */
      return fi < i;   /* compare them as integers */
    else  /* 'f' is either greater or less than all integers */
      return f < 0;  /* less? */
  }
}


/*
** Check whether float 'f' is less than or equal to integer 'i'.
** See comments on previous function.
*/
l_sinline int LEfloatint (lua_Number f, lua_Integer i) {
  if (l_intfitsf(i))
    return luai_numle(f, cast_num(i));  /* compare them as floats */
  else {  /* f <= i <=> ceil(f) <= i */
    lua_Integer fi;
    if (luaV_flttointeger(f, &fi, F2Iceil))  /* fi = ceil(f) */
      return fi <= i;   /* compare them as integers */
    else  /* 'f' is either greater or less than all integers */
      return f < 0;  /* less? */
  }
}


/*
** Return 'l < r', for numbers.
*/
l_sinline int LTnum (const TValue *l, const TValue *r) {
  lua_assert(ttisnumber(l) && ttisnumber(r));
  if (ttisinteger(l)) {
    lua_Integer li = ivalue(l);
    if (ttisinteger(r))
      return li < ivalue(r);  /* both are integers */
    else  /* 'l' is int and 'r' is float */
      return LTintfloat(li, fltvalue(r));  /* l < r ? */
  }
  else {
    lua_Number lf = fltvalue(l);  /* 'l' must be float */
    if (ttisfloat(r))
      return luai_numlt(lf, fltvalue(r));  /* both are float */
    else  /* 'l' is float and 'r' is int */
      return LTfloatint(lf, ivalue(r));
  }
}


/*
** Return 'l <= r', for numbers.
*/
l_sinline int LEnum (const TValue *l, const TValue *r) {
  lua_assert(ttisnumber(l) && ttisnumber(r));
  if (ttisinteger(l)) {
    lua_Integer li = ivalue(l);
    if (ttisinteger(r))
      return li <= ivalue(r);  /* both are integers */
    else  /* 'l' is int and 'r' is float */
      return LEintfloat(li, fltvalue(r));  /* l <= r ? */
  }
  else {
    lua_Number lf = fltvalue(l);  /* 'l' must be float */
    if (ttisfloat(r))
      return luai_numle(lf, fltvalue(r));  /* both are float */
    else  /* 'l' is float and 'r' is int */
      return LEfloatint(lf, ivalue(r));
  }
}


/*
** return 'l < r' for non-numbers.
*/
static int lessthanothers (lua_State *L, const TValue *l, const TValue *r) {
  lua_assert(!ttisnumber(l) || !ttisnumber(r));
  if (ttisstring(l) && ttisstring(r))  /* both are strings? */
    return l_strcmp(tsvalue(l), tsvalue(r)) < 0;
  else
    return luaT_callorderTM(L, l, r, TM_LT);
}


/*
** Main operation less than; return 'l < r'.
*/
int luaV_lessthan (lua_State *L, const TValue *l, const TValue *r) {
  if (ttisnumber(l) && ttisnumber(r))  /* both operands are numbers? */
    return LTnum(l, r);
  else return lessthanothers(L, l, r);
}


/*
** return 'l <= r' for non-numbers.
*/
static int lessequalothers (lua_State *L, const TValue *l, const TValue *r) {
  lua_assert(!ttisnumber(l) || !ttisnumber(r));
  if (ttisstring(l) && ttisstring(r))  /* both are strings? */
    return l_strcmp(tsvalue(l), tsvalue(r)) <= 0;
  else
    return luaT_callorderTM(L, l, r, TM_LE);
}


/*
** Main operation less than or equal to; return 'l <= r'.
*/
int luaV_lessequal (lua_State *L, const TValue *l, const TValue *r) {
  if (ttisnumber(l) && ttisnumber(r))  /* both operands are numbers? */
    return LEnum(l, r);
  else return lessequalothers(L, l, r);
}


/*
** Main operation for equality of Lua values; return 't1 == t2'.
** L == NULL means raw equality (no metamethods)
*/
static int equalmethod (lua_State *L, const TValue *t1, const TValue *t2) {
  LuaNativeRoot method;
  int result = 0;
  luaR_pushroot(L, &method);
  luaT_gettmbyobj(L, t1, TM_EQ, &method);
  if (ttisnil(&method.value))
    luaT_gettmbyobj(L, t2, TM_EQ, &method);
  if (!ttisnil(&method.value))
    result = !tagisfalse(luaT_callTMres(L, &method.value, t1, t2, L->top.p));
  luaR_poproot(L, &method);
  return result;
}

int luaV_equalobj (lua_State *L, const TValue *t1, const TValue *t2) {
  if (ttype(t1) != ttype(t2))  /* not the same type? */
    return 0;
  else if (ttypetag(t1) != ttypetag(t2)) {
    switch (ttypetag(t1)) {
      case LUA_VNUMINT: {  /* integer == float? */
        /* integer and float can only be equal if float has an integer
           value equal to the integer */
        lua_Integer i2;
        return (luaV_flttointeger(fltvalue(t2), &i2, F2Ieq) &&
                ivalue(t1) == i2);
      }
      case LUA_VNUMFLT: {  /* float == integer? */
        lua_Integer i1;  /* see comment in previous case */
        return (luaV_flttointeger(fltvalue(t1), &i1, F2Ieq) &&
                i1 == ivalue(t2));
      }
      case LUA_VSHRSTR: case LUA_VLNGSTR: {
        /* Interned and ordinary strings can have the same contents. */
        return luaS_eqstr(tsvalue(t1), tsvalue(t2));
      }
      case LUA_VLCF: case LUA_VMCF:
        return ttislcf(t2) && fvalue(t1) == fvalue(t2);
      default:
        return 0;
    }
  }
  else {  /* equal variants */
    switch (ttypetag(t1)) {
      case LUA_VNIL: case LUA_VFALSE: case LUA_VTRUE:
        return 1;
      case LUA_VNUMINT:
        return (ivalue(t1) == ivalue(t2));
      case LUA_VNUMFLT:
        return (fltvalue(t1) == fltvalue(t2));
      case LUA_VLIGHTUSERDATA: return pvalue(t1) == pvalue(t2);
      case LUA_VSHRSTR:
        return eqshrstr(tsvalue(t1), tsvalue(t2));
      case LUA_VLNGSTR:
        return luaS_eqstr(tsvalue(t1), tsvalue(t2));
      case LUA_VUSERDATA: {
        if (uvalue(t1) == uvalue(t2)) return 1;
        else if (L == NULL) return 0;
        return equalmethod(L, t1, t2);
      }
      case LUA_VTABLE: {
        if (hvalue(t1) == hvalue(t2)) return 1;
        else if (L == NULL) return 0;
        return equalmethod(L, t1, t2);
      }
      case LUA_VLCF: case LUA_VMCF:
        return (fvalue(t1) == fvalue(t2));
      default:  /* functions and threads */
        return (gcvalue(t1) == gcvalue(t2));
    }
  }
}


/* macro used by 'luaV_concat' to ensure that element at 'o' is a string */
#define tostring(L,o)  \
	(ttisstring(o) || (cvt2str(o) && (luaO_tostring(L, o), 1)))

/*
** Check whether object is a short empty string to optimize concatenation.
** (External strings can be empty too; they will be concatenated like
** non-empty ones.)
*/
#define isemptystr(o)	(ttisshrstring(o) && tsvalue(o)->shrlen == 0)

/* copy strings in stack from top - n up to top - 1 to buffer */
static void copy2buff (StkId top, int n, char *buff) {
  size_t tl = 0;  /* size already copied */
  do {
    TString *st = tsvalue(s2v(top - n));
    size_t l;  /* length of string being copied */
    const char *s = getlstr(st, l);
    memcpy(buff + tl, s, l * sizeof(char));
    tl += l;
  } while (--n > 0);
}


/*
** Main operation for concatenation: concat 'total' values in the stack,
** from 'L->top.p - total' up to 'L->top.p - 1'.
*/
void luaV_concat (lua_State *L, int total) {
  if (total == 1)
    return;  /* "all" values already concatenated */
  do {
    StkId top = L->top.p;
    int n = 2;  /* number of elements handled in this pass (at least 2) */
    if (!(ttisstring(s2v(top - 2)) || cvt2str(s2v(top - 2))) ||
        !tostring(L, s2v(top - 1)))
      luaT_tryconcatTM(L);  /* may invalidate 'top' */
    else if (isemptystr(s2v(top - 1)))  /* second operand is empty? */
      cast_void(tostring(L, s2v(top - 2)));  /* result is first operand */
    else if (isemptystr(s2v(top - 2))) {  /* first operand is empty string? */
      setobjs2s(L, top - 2, top - 1);  /* result is second op. */
    }
    else {
      /* at least two string values; get as many as possible */
      size_t tl = tsslen(tsvalue(s2v(top - 1)));  /* total length */
      TString *ts;
      /* collect total length and number of strings */
      for (n = 1; n < total && tostring(L, s2v(top - n - 1)); n++) {
        size_t l = tsslen(tsvalue(s2v(top - n - 1)));
        if (l_unlikely(l >= MAX_SIZE - sizeof(TString) - tl)) {
          L->top.p = top - total;  /* pop strings to avoid wasting stack */
          luaG_runerror(L, "string length overflow");
        }
        tl += l;
      }
      if (tl <= LUAI_MAXSHORTLEN) {  /* is result a short string? */
        char buff[LUAI_MAXSHORTLEN];
        copy2buff(top, n, buff);  /* copy strings to buffer */
        ts = luaS_newlstr(L, buff, tl);
      }
      else {  /* long string; copy strings directly to final result */
        ts = luaS_createlngstrobj(L, tl, 1);
        copy2buff(top, n, getlngstr(ts));
        luaS_finishlongstr(ts);
      }
      setsvalue2s(L, top - n, ts);  /* create result */
    }
    total -= n - 1;  /* got 'n' strings to create one new */
    L->top.p -= n - 1;  /* popped 'n' strings and pushed one */
  } while (total > 1);  /* repeat until only 1 result left */
}


/*
** Main operation 'ra = #rb'.
*/
void luaV_objlen (lua_State *L, StkId ra, const TValue *rb) {
  LuaNativeRoot method;
  if (ttisstring(rb)) {
    setivalue(s2v(ra), cast_st2S(tsslen(tsvalue(rb))));
    return;
  }
  if (ttistable(rb)) {
    unsigned border = luaH_cachedborder(L, hvalue(rb), 1);
    if (border != 0) {
      setivalue(s2v(ra), cast(lua_Integer, border));
      return;
    }
  }
  luaR_pushroot(L, &method);
  luaT_gettmbyobj(L, rb, TM_LEN, &method);
  switch (ttypetag(rb)) {
    case LUA_VTABLE: {
      Table *h = hvalue(rb);
      if (!ttisnil(&method.value)) break;
      setivalue(s2v(ra), l_castU2S(luaH_getn(L, h)));  /* else primitive len */
      luaR_poproot(L, &method);
      return;
    }
    default: {  /* try metamethod */
      if (l_unlikely(ttisnil(&method.value)))
        luaG_typeerror(L, rb, "get length of");
      break;
    }
  }
  luaT_callTMres(L, &method.value, rb, rb, ra);
  luaR_poproot(L, &method);
}

/*
** Integer division; return 'm // n', that is, floor(m/n).
** C division truncates its result (rounds towards zero).
** 'floor(q) == trunc(q)' when 'q >= 0' or when 'q' is integer,
** otherwise 'floor(q) == trunc(q) - 1'.
*/
lua_Integer luaV_idiv (lua_State *L, lua_Integer m, lua_Integer n) {
  if (l_unlikely(l_castS2U(n) + 1u <= 1u)) {  /* special cases: -1 or 0 */
    if (n == 0)
      luaG_runerror(L, "attempt to divide by zero");
    return intop(-, 0, m);   /* n==-1; avoid overflow with 0x80000...//-1 */
  }
  else {
    lua_Integer q = m / n;  /* perform C division */
    if ((m ^ n) < 0 && m % n != 0)  /* 'm/n' would be negative non-integer? */
      q -= 1;  /* correct result for different rounding */
    return q;
  }
}


/*
** Integer modulus; return 'm % n'. (Assume that C '%' with
** negative operands follows C99 behavior. See previous comment
** about luaV_idiv.)
*/
lua_Integer luaV_mod (lua_State *L, lua_Integer m, lua_Integer n) {
  if (l_unlikely(l_castS2U(n) + 1u <= 1u)) {  /* special cases: -1 or 0 */
    if (n == 0)
      luaG_runerror(L, "attempt to perform 'n%%0'");
    return 0;   /* m % -1 == 0; avoid overflow with 0x80000...%-1 */
  }
  else {
    lua_Integer r = m % n;
    if (r != 0 && (r ^ n) < 0)  /* 'm/n' would be non-integer negative? */
      r += n;  /* correct result for different rounding */
    return r;
  }
}


/*
** Float modulus
*/
lua_Number luaV_modf (lua_State *L, lua_Number m, lua_Number n) {
  lua_Number r;
  luai_nummod(L, m, n, r);
  return r;
}


/* number of bits in an integer */
#define NBITS	l_numbits(lua_Integer)


/*
** Shift left operation. (Shift right just negates 'y'.)
*/
lua_Integer luaV_shiftl (lua_Integer x, lua_Integer y) {
  if (y < 0) {  /* shift right? */
    if (y <= -NBITS) return 0;
    else return intop(>>, x, -y);
  }
  else {  /* shift left */
    if (y >= NBITS) return 0;
    else return intop(<<, x, y);
  }
}


/*
** Reuse an immutable closure from the current tracing cohort.
** A cache hit cannot allocate, park or raise an error.
*/
static LClosure *cachedclosure (lua_State *L, Proto *p) {
  /* Lua permits reusing indistinguishable functions. A zero-capture closure
  ** is immutable, so independent executions can share it. The cache is weak:
  ** a new tracing cohort invalidates it BEFORE any previous object can retire.
  ** Never load its pointer on an epoch mismatch; it may already be reclaimed.
  ** No safepoint separates this check, pointer load and stack publication. */
  if (p->sizeupvalues == 0 &&
      __atomic_load_n(&p->closure_epoch, __ATOMIC_ACQUIRE) ==
                   luaC_traceepoch(G(L))) {
    LClosure *ncl = __atomic_load_n(&p->closure_cache, __ATOMIC_ACQUIRE);
    if (ncl != NULL) {
      luaC_tracepointer(L, ncl);
      return ncl;
    }
  }
  return NULL;
}
/* Allocate a closure and initialize its captures. */
static void pushclosure (lua_State *L, Proto *p, LClosure *enclosing, StkId base,
                         StkId ra) {
  int nup = p->sizeupvalues;
  Upvaldesc *uv = p->upvalues;
  LuaNativeRoot closure, capture;
  ptrdiff_t base_offset = savestack(L, base);
  ptrdiff_t target_offset = savestack(L, ra);
  int i;
  LClosure *ncl;
  LuaObjectScope construction;
  luaR_pushroot(L, &closure);
  luaR_pushroot(L, &capture);
  ncl = luaF_newLclosure(L, nup);
  setclLvalue(L, &closure.value, ncl);
  luaA_begin(L, obj2gco(ncl), &construction);
  luaC_tracepointer(L, p);
  __atomic_store_n(&ncl->p, p, __ATOMIC_RELEASE);
  luaC_objbarrier(L, ncl, p);
  for (i = 0; i < nup; i++) {  /* fill in its upvalues */
    if (uv[i].instack)  /* upvalue refers to local variable? */
      __atomic_store_n(&ncl->upvals[i],
        luaF_findupval(L, restorestack(L, base_offset) + uv[i].idx), __ATOMIC_RELEASE);
    else {  /* snapshot the enclosing capture before publishing the child */
      luaF_getcapture(L, enclosing, uv[i].idx, &capture);
      __atomic_store_n(&ncl->upvals[i], gco2upv(gcvalue(&capture.value)), __ATOMIC_RELEASE);
    }
    luaC_objbarrier(L, ncl, ncl->upvals[i]);
  }
  luaA_end(L, &construction);
  setclLvalue2s(L, restorestack(L, target_offset), ncl);
  if (nup == 0) {
    /* Construction can collect or park. Publish the CURRENT cohort only once
    ** the rooted closure is complete. Racing publishers in that cohort all
    ** offer live immutable closures; either choice is valid. No collector
    ** reads this weak edge or needs to scan a cache directory. */
    global_State *g = G(L);
    uint64_t epoch = luaC_traceepoch(g);
    if (!(luaC_markstate(g) & LUA_MARK_ARMING)) {
      luaC_tracepointer(L, ncl);  /* construction may straddle activation */
      __atomic_store_n(&ncl->sharing, LUA_OBJECT_SHARED, __ATOMIC_RELEASE);
      __atomic_store_n(&p->closure_cache, ncl, __ATOMIC_RELEASE);
      __atomic_store_n(&p->closure_epoch, epoch, __ATOMIC_RELEASE);
    }
  }
  luaR_poproot(L, &capture);
  luaR_poproot(L, &closure);
}


/* The portable instruction is also the root map for a suspended operation.
** Uncaptured registers remain ordinary conservative stack roots. A captured
** shadow is needed only for operands/results of the operation in flight; the
** cell itself is traced separately. Decode on root publication, never dispatch.
** A C frame can borrow caller slots, so its complete live stack stays rooted. */
int luaV_shadowroot (CallInfo **frame, StkId slot, StkId stacktop) {
  CallInfo *ci = *frame;
  Instruction i;
  OpCode op;
  int reg, a, b, c, top;
  /* Later slots in the scan are lower, so they cannot need a frame
  ** whose function is above this slot. */
  while (ci != NULL && slot < ci->func.p) ci = ci->previous;
  *frame = ci;
  if (ci != NULL) {
    if (!isLua(ci) || slot == ci->func.p) return 1;
    if (ci->u.l.rootpc == NULL) return 0;
    i = *ci->u.l.rootpc;
    op = GET_OPCODE(i);
    reg = cast_int(slot - (ci->func.p + 1));
    top = cast_int(stacktop - (ci->func.p + 1));
    a = GETARG_A(i);
    /* These formats do not have B/C fields. */
    b = getarg(i, POS_B, SIZE_B);
    c = getarg(i, POS_C, SIZE_C);
    switch (op) {
      case OP_CALL: case OP_TAILCALL:
        return reg >= a && reg < (b == 0 ? top : a + b);
      case OP_RETURN:
        return reg >= a && reg < (b == 0 ? top : a + b - 1);
      case OP_RETURN0: case OP_JMP: case OP_EXTRAARG:
        return 0;
      case OP_CONCAT:
        return reg >= a && reg < a + b;
      case OP_CLOSE:
        return reg >= a;
      case OP_LOADNIL:
        return reg >= a && reg <= a + b;
      case OP_SELF:
        return reg == a || reg == a + 1 || reg == b;
      case OP_FORLOOP: case OP_FORPREP:
        return reg >= a && reg < a + 3;
      case OP_TFORPREP:
        return reg == a + 2 || reg == a + 3;
      case OP_TFORCALL:
        return reg >= a && reg < a + 3 + (c > 3 ? c : 3);
      case OP_TFORLOOP:
        return reg == a + 3;
      case OP_SETLIST: {
        int n = GETARG_vB(i);
        return reg >= a && reg < (n == 0 ? top : a + n + 1);
      }
      case OP_VARARG:
        return (TESTARG_k(i) && reg == b) ||
               (reg >= a && reg < (c == 0 ? top : a + c - 1));
      case OP_VARARGPREP:
        return 1;
      case OP_MMBIN: case OP_MMBINI: case OP_MMBINK:
        return reg == a || (op == OP_MMBIN && reg == b) ||
               reg == GETARG_A(*(ci->u.l.rootpc - 1));
      case OP_SETTABUP:
        return !TESTARG_k(i) && reg == c;
      case OP_SETTABLE:
        return reg == a || reg == b || (!TESTARG_k(i) && reg == c);
      case OP_SETI: case OP_SETFIELD:
        return reg == a || (!TESTARG_k(i) && reg == c);
      case OP_SETUPVAL: case OP_TBC: case OP_EQK: case OP_EQI:
      case OP_LTI: case OP_LEI: case OP_GTI: case OP_GEI: case OP_TEST:
      case OP_RETURN1: case OP_ERRNNIL:
        return reg == a;
      case OP_EQ: case OP_LT: case OP_LE: case OP_TESTSET:
        return reg == a || reg == b;
      default:
        if (testAMode(op) && reg == a) return 1;
        if (op == OP_MOVE || op == OP_GETI || op == OP_GETFIELD ||
            (OP_ADDI <= op && op <= OP_SHRI) ||
            (OP_UNM <= op && op <= OP_LEN))
          return reg == b;
        if (op == OP_GETTABLE || op == OP_GETVARG ||
            (OP_ADD <= op && op <= OP_SHR))
          return reg == b || reg == c;
        return 0;
    }
  }
  return 1;  /* outside Lua frames, retain the native stack contract */
}


/* Lower register writes to stable cells without changing the portable bytecode.
** Conditional and loop writes are published at the branch that performs them;
** call results are published by poscall, including yielded continuations. */
static void storeinstruction (lua_State *L, CallInfo *ci, Instruction i,
                               const Instruction *pc) {
  OpCode op = GET_OPCODE(i);
  StkId base = ci->func.p + 1;
  int first = GETARG_A(i), count = 1;
  if (op == OP_TESTSET || op == OP_FORLOOP || op == OP_FORPREP || op == OP_TFORLOOP ||
      op == OP_CALL || op == OP_TAILCALL)
    return;
  if (OP_ADDI <= op && op <= OP_SHR && testMMMode(GET_OPCODE(*pc)))
    return;  /* numeric fast path did not write a result */
  if (testMMMode(op))
    first = GETARG_A(*(pc - 2));
  else if (!testAMode(op))
    return;
  if (op == OP_LOADNIL) count = GETARG_B(i) + 1;
  else if (op == OP_SELF) count = 2;
  else if (op == OP_VARARG)
    count = GETARG_C(i) == 0 ? cast_int(L->top.p - (base + first))
                            : GETARG_C(i) - 1;
  if (count == 1 && !(ci->u.l.captures & luaF_capturebit(first)))
    return;
  luaF_storelocals(L, base + first, count);
}

/*
** finish execution of an opcode interrupted by a yield
*/
void luaV_finishOp (lua_State *L) {
  CallInfo *ci = L->ci;
  StkId base = ci->func.p + 1;
  Instruction inst = *(ci->u.l.savedpc - 1);  /* interrupted instruction */
  OpCode op = GET_OPCODE(inst);
  switch (op) {  /* finish its execution */
    case OP_MMBIN: case OP_MMBINI: case OP_MMBINK: {
      setobjs2s(L, base + GETARG_A(*(ci->u.l.savedpc - 2)), --L->top.p);
      break;
    }
    case OP_UNM: case OP_BNOT: case OP_LEN:
    case OP_GETTABUP: case OP_GETTABLE: case OP_GETI:
    case OP_GETFIELD: case OP_SELF: {
      setobjs2s(L, base + GETARG_A(inst), --L->top.p);
      break;
    }
    case OP_LT: case OP_LE:
    case OP_LTI: case OP_LEI:
    case OP_GTI: case OP_GEI:
    case OP_EQ: {  /* note that 'OP_EQI'/'OP_EQK' cannot yield */
      int res = !l_isfalse(s2v(L->top.p - 1));
      L->top.p--;
      lua_assert(GET_OPCODE(*ci->u.l.savedpc) == OP_JMP);
      if (res != GETARG_k(inst))  /* condition failed? */
        ci->u.l.savedpc++;  /* skip jump instruction */
      break;
    }
    case OP_CONCAT: {
      StkId top = L->top.p - 1;  /* top when 'luaT_tryconcatTM' was called */
      int a = GETARG_A(inst);      /* first element to concatenate */
      int total = cast_int(top - 1 - (base + a));  /* yet to concatenate */
      setobjs2s(L, top - 2, top);  /* put TM result in proper position */
      L->top.p = top - 1;  /* top is one after last element (at top-2) */
      luaV_concat(L, total);  /* concat them (may yield again) */
      break;
    }
    case OP_CLOSE: {  /* yielded closing variables */
      ci->u.l.savedpc--;  /* repeat instruction to close other vars. */
      break;
    }
    case OP_RETURN: {  /* yielded closing variables */
      StkId ra = base + GETARG_A(inst);
      /* adjust top to signal correct number of returns, in case the
         return is "up to top" ('isIT') */
      L->top.p = ra + ci->u2.nres;
      /* repeat instruction to close other vars. and complete the return */
      ci->u.l.savedpc--;
      break;
    }
    default: {
      /* only these other opcodes can yield */
      lua_assert(op == OP_TFORCALL || op == OP_CALL ||
           op == OP_TAILCALL || op == OP_SETTABUP || op == OP_SETTABLE ||
           op == OP_SETI || op == OP_SETFIELD);
      break;
    }
  }
  if (ci->u.l.captures != 0)
    storeinstruction(L, ci, inst, ci->u.l.savedpc);
}




/*
** {==================================================================
** Macros for arithmetic/bitwise/comparison opcodes in 'luaV_execute'
**
** All these macros are to be used exclusively inside the main
** interpreter loop (function luaV_execute) and may access directly
** the local variables of that function (L, i, pc, ci, etc.).
** ===================================================================
*/

#define l_addi(L,a,b)	intop(+, a, b)
#define l_subi(L,a,b)	intop(-, a, b)
#define l_muli(L,a,b)	intop(*, a, b)
#define l_band(a,b)	intop(&, a, b)
#define l_bor(a,b)	intop(|, a, b)
#define l_bxor(a,b)	intop(^, a, b)

#define l_lti(a,b)	(a < b)
#define l_lei(a,b)	(a <= b)
#define l_gti(a,b)	(a > b)
#define l_gei(a,b)	(a >= b)


/*
** Arithmetic operations with immediate operands. 'iop' is the integer
** operation, 'fop' is the float operation.
*/
#define op_arithI(L,iop,fop) {  \
  TValue *ra = s2v(RA(i)); \
  TValue *v1 = vRB(i);  \
  int imm = GETARG_sC(i);  \
  if (ttisinteger(v1)) {  \
    lua_Integer iv1 = ivalue(v1);  \
    pc++; setivalue(ra, iop(L, iv1, imm));  \
  }  \
  else if (ttisfloat(v1)) {  \
    lua_Number nb = fltvalue(v1);  \
    lua_Number fimm = cast_num(imm);  \
    pc++; setfltvalue(ra, fop(L, nb, fimm)); \
  }}


/*
** Auxiliary function for arithmetic operations over floats and others
** with two operands.
*/
#define op_arithf_aux(L,v1,v2,fop) {  \
  lua_Number n1; lua_Number n2;  \
  if (tonumberns(v1, n1) && tonumberns(v2, n2)) {  \
    StkId ra = RA(i);  \
    pc++; setfltvalue(s2v(ra), fop(L, n1, n2));  \
  }}


/*
** Arithmetic operations over floats and others with register operands.
*/
#define op_arithf(L,fop) {  \
  TValue *v1 = vRB(i);  \
  TValue *v2 = vRC(i);  \
  op_arithf_aux(L, v1, v2, fop); }


/*
** Arithmetic operations with K operands for floats.
*/
#define op_arithfK(L,fop) {  \
  TValue *v1 = vRB(i);  \
  TValue *v2 = KC(i); lua_assert(ttisnumber(v2));  \
  op_arithf_aux(L, v1, v2, fop); }


/*
** Arithmetic operations over integers and floats.
*/
#define op_arith_aux(L,v1,v2,iop,fop) {  \
  if (ttisinteger(v1) && ttisinteger(v2)) {  \
    StkId ra = RA(i); \
    lua_Integer i1 = ivalue(v1); lua_Integer i2 = ivalue(v2);  \
    pc++; setivalue(s2v(ra), iop(L, i1, i2));  \
  }  \
  else op_arithf_aux(L, v1, v2, fop); }


/*
** Arithmetic operations with register operands.
*/
#define op_arith(L,iop,fop) {  \
  TValue *v1 = vRB(i);  \
  TValue *v2 = vRC(i);  \
  op_arith_aux(L, v1, v2, iop, fop); }


/*
** Arithmetic operations with K operands.
*/
#define op_arithK(L,iop,fop) {  \
  TValue *v1 = vRB(i);  \
  TValue *v2 = KC(i); lua_assert(ttisnumber(v2));  \
  op_arith_aux(L, v1, v2, iop, fop); }


/*
** Bitwise operations with constant operand.
*/
#define op_bitwiseK(L,op) {  \
  TValue *v1 = vRB(i);  \
  TValue *v2 = KC(i);  \
  lua_Integer i1;  \
  lua_Integer i2 = ivalue(v2);  \
  if (tointegerns(v1, &i1)) {  \
    StkId ra = RA(i); \
    pc++; setivalue(s2v(ra), op(i1, i2));  \
  }}


/*
** Bitwise operations with register operands.
*/
#define op_bitwise(L,op) {  \
  TValue *v1 = vRB(i);  \
  TValue *v2 = vRC(i);  \
  lua_Integer i1; lua_Integer i2;  \
  if (tointegerns(v1, &i1) && tointegerns(v2, &i2)) {  \
    StkId ra = RA(i); \
    pc++; setivalue(s2v(ra), op(i1, i2));  \
  }}


/*
** Order operations with register operands. 'opn' actually works
** for all numbers, but the fast track improves performance for
** integers.
*/
#define op_order(L,opi,opn,other) {  \
  TValue *ra = vRA(i); \
  int cond;  \
  TValue *rb = vRB(i);  \
  if (ttisinteger(ra) && ttisinteger(rb)) {  \
    lua_Integer ia = ivalue(ra);  \
    lua_Integer ib = ivalue(rb);  \
    cond = opi(ia, ib);  \
  }  \
  else if (ttisnumber(ra) && ttisnumber(rb))  \
    cond = opn(ra, rb);  \
  else  \
    Protect(cond = other(L, ra, rb));  \
  docondjump(); }


/*
** Order operations with immediate operand. (Immediate operand is
** always small enough to have an exact representation as a float.)
*/
#define op_orderI(L,opi,opf,inv,tm) {  \
  TValue *ra = vRA(i); \
  int cond;  \
  int im = GETARG_sB(i);  \
  if (ttisinteger(ra))  \
    cond = opi(ivalue(ra), im);  \
  else if (ttisfloat(ra)) {  \
    lua_Number fa = fltvalue(ra);  \
    lua_Number fim = cast_num(im);  \
    cond = opf(fa, fim);  \
  }  \
  else {  \
    int isf = GETARG_C(i);  \
    Protect(cond = luaT_callorderiTM(L, ra, im, inv, isf, tm));  \
  }  \
  docondjump(); }

/* }================================================================== */


/*
** {==================================================================
** Function 'luaV_execute': main interpreter loop
** ===================================================================
*/

/*
** some macros for common tasks in 'luaV_execute'
*/


/* The per-frame filter occupies otherwise unused union space. Only a possible
** captured operand searches the open-cell list; it never parks or reallocates.
** Multiple reads of an aliased operand use the same register shadow. */
l_sinline StkId directregister (StkId base, int reg) {
  return base + reg;
}

#define readreg(reg) \
  (VM_CAPTURE && l_unlikely(ci->u.l.captures & luaF_capturebit(reg)) \
    ? luaF_loadlocal(L, base + (reg)) : directregister(base, (reg)))

#define RA(i)	(base+GETARG_A(i))
#define readRA(i) readreg(GETARG_A(i))
#define vRA(i)	s2v(readRA(i))
#define RB(i)	readreg(GETARG_B(i))
#define vRB(i)	s2v(RB(i))
#define KB(i)	(k+GETARG_B(i))
#define RC(i)	readreg(GETARG_C(i))
#define vRC(i)	s2v(RC(i))
#define KC(i)	(k+GETARG_C(i))
#define RKC(i)	((TESTARG_k(i)) ? k + GETARG_C(i) : vRC(i))

#define execute_OP_MOVE(i) setobjs2s(L, RA(i), RB(i))
#define execute_OP_GETUPVAL(i) \
  luaF_loadupval(L, luaF_capturecell(cl, GETARG_B(i)), s2v(readRA(i)))

/* Calls, returns and bulk operations consume contiguous operand ranges. This
** refresh belongs to the operation, including its fused iterator entries. */
#define readrange(first,count) \
  { if (VM_CAPTURE && l_unlikely(ci->u.l.captures != 0) && \
        (ci->u.l.captures & \
         luaF_capturerange(cast_int((first) - base), (count)))) \
      luaF_loadrange(L, (first), (count)); }



#define updatetrap(ci) \
  (trap = (ci)->u.l.trap | (gethookmask(L) & (LUA_MASKLINE | LUA_MASKCOUNT)))

#define updatebase(ci)	(base = ci->func.p + 1)


#define updatestack(ci)  \
	{ if (l_unlikely(trap)) { updatebase(ci); ra = RA(i); } }


/*
** Execute a jump instruction. The 'updatetrap' allows signals to stop
** tight loops. (Without it, the local copy of 'trap' could never change.)
*/
#define dojump(ci,i,e)	{ pc += GETARG_sJ(i) + e; updatetrap(ci); }


/* for test instructions, execute the jump instruction that follows it */
#define donextjump(ci) { Instruction ni = *pc; dojump(ci, ni, 1); }

/*
** do a conditional jump: skip next instruction if 'cond' is not what
** was expected (parameter 'k'), else do next instruction, which must
** be a jump.
*/
#define docondjump()	if (cond != GETARG_k(i)) pc++; else donextjump(ci);


/*
** Correct global 'pc'.
*/
#define savepc(ci) \
  (ci->u.l.rootpc = ip, ci->u.l.savedpc = pc)


/*
** Whenever code can raise errors, the global 'pc' and the global
** 'top' must be correct to report occasional errors.
*/
#define savestate(L,ci)		(savepc(ci), L->top.p = ci->top.p)


/*
** Protect code that, in general, can raise errors, reallocate the
** stack, and change the hooks.
*/
#define Protect(exp)  (savestate(L,ci), (exp), updatetrap(ci))

/* special version that does not change the top */
#define ProtectNT(exp)  (savepc(ci), (exp), updatetrap(ci))

/*
** Protect code that can only raise errors. (That is, it cannot change
** the stack or hooks.)
*/
#define halfProtect(exp)  (savestate(L,ci), (exp))

/* Only waits and metamethod dispatch need to publish the current frame. */
#define vmtableget(receiver,key,read,index,upvalue) { \
  lu_byte tag = LUA_VABSTKEY; \
  int retry = ttistable(receiver) && \
    !read(L, hvalue(receiver), index, s2v(ra), &tag); \
  if (l_unlikely(retry || luaV_needsindex(tag))) { \
    Table *methods = ttisstring(receiver) ? luaA_stringindex(L) : NULL; \
    if (methods != NULL) { \
      luaC_tracepointer(L, methods); \
      if (!read(L, methods, index, s2v(ra), &tag)) tag = LUA_VABSTKEY; \
    } \
    if (retry || luaV_needsindex(tag)) { \
      Protect(finishvmget(L, receiver, key, ra, upvalue, retry)); \
      updatebase(ci); \
    } \
  } \
}

#define vmtableset(receiver,key,value,upvalue,write) { \
  if (l_unlikely(!ttistable(receiver) || \
      !write(L, hvalue(receiver), key, value, 0))) { \
    Protect(finishset(L, receiver, key, value, upvalue)); \
    updatebase(ci); \
  } \
}

/* 'c' is the limit of live values in the stack. Publish before every handoff,
** even if this carrier does not collect. Another collector can move the stack
** while ownership is released, so reload dispatch state after reacquiring it.
** Allocations do not otherwise retire the current execution slot or quantum. */
#define checkGC(L,c)  \
	{ savepc(ci); L->top.p = (c); \
	  luaC_condGC(L, (void)0, (void)0); \
	  luaX_pollrequests(L); updatebase(ci); updatetrap(ci); }

static const TValue *projectedvalue (const LuaRecordSite *site,
                                     const Proto *p, StkId base, StkId result,
                                     const Instruction *first) {
  switch (site->source) {
    case LUA_PROJECT_REGISTER: return s2v(base + site->operand);
    case LUA_PROJECT_CONSTANT: return p->k + site->operand;
    case LUA_PROJECT_FIELD: {
      const TValue *key = s2v(base + site->operand);
      int i;
      if (!ttisstring(key)) return NULL;
      for (i = site->projection - 2; i >= 0; i--) {
        Instruction store = first[i];
        if (luaV_rawequalobj(key, p->k + GETARG_B(store)))
          return TESTARG_k(store) ? p->k + GETARG_C(store)
                                  : s2v(base + GETARG_C(store));
      }
      return NULL;
    }
    case LUA_PROJECT_INDEX: {
      lua_Integer index;
      if (luaV_tointegerns(s2v(base + site->operand), &index, F2Ieq) &&
          index > 0 && index <= site->moves)
        return s2v(result + index);
      return NULL;
    }
    default: return NULL;
  }
}

l_sinline const LuaRecordSite *constructorsite (const Proto *p,
                                               const Instruction *at) {
  int index = cast_int(at - p->code);
  int low = 0, high = p->sizerecords;
  while (low < high) {
    int middle = low + (high - low) / 2;
    if (p->records[middle].pc < index) low = middle + 1;
    else high = middle;
  }
  if (low < p->sizerecords && p->records[low].pc == index) return &p->records[low];
  return NULL;
}

static void __attribute__((noinline)) projecttable (
    lua_State *L, const Proto *proto, CallInfo *ci, StkId result,
    const Instruction *first, const LuaRecordSite *site) {
  StkId base = ci->func.p + 1;
  const TValue *value;
  unsigned move;
  /* Preserve temporary writes. There is no parking or user code before
  ** the final read overwrites the only register holding the table. */
  for (move = 0; move < site->moves; move++) {
    Instruction ins = first[move];
    setobjs2s(L, base + GETARG_A(ins), base + GETARG_B(ins));
  }
  value = projectedvalue(site, proto, base, result, first);
  if (value != NULL) { setobj2s(L, result, value); }
  else { setnilvalue2s(result); }
  L->top.p = site->moves != 0 ? ci->top.p : result + 1;
  L->poll_remaining -= site->projection;
}

/* Allocation can park and relocate registers. Keep this operation out of
** both dispatch loops, with the destination recoverable from the saved top. */
static void __attribute__((noinline)) createtable (
    lua_State *L, CallInfo *ci, const Instruction *at, Instruction i,
    Instruction extra) {
  unsigned hash = cast_uint(GETARG_vB(i));
  unsigned array = cast_uint(GETARG_vC(i));
  const Proto *proto = ci_func(ci)->p;
  int reg = GETARG_A(i);
  Table *keys = NULL;
  Table *table;
  int tasklocal = 1;
  if (hash != 0) hash = 1u << (hash - 1);  /* log2(hash size) + 1 */
  if (TESTARG_k(i)) {
    lua_assert(GETARG_Ax(extra) != 0);
    array += cast_uint(GETARG_Ax(extra)) * (MAXARG_vC + 1);
  }
  if (proto->sizerecords != 0) {
    const LuaRecordSite *site = constructorsite(proto, at);
    if (site != NULL) {
      keys = site->keys;
      /* Placement only. Ownership and metamethods still follow the store. */
      if (site->projection == 0 && site->operand != NO_REG &&
          L->task_owner != NULL && !L->task_nursery_closed &&
          ci->u.l.captures == 0) {
        const TValue *receiver = s2v(ci->func.p + 1 + site->operand);
        if (ttistable(receiver) &&
            (__atomic_load_n(&hvalue(receiver)->sharing, __ATOMIC_ACQUIRE) &
             LUA_OBJECT_SHARED))
          tasklocal = 0;
      }
    }
  }
  L->top.p = ci->func.p + 2 + reg;
  table = keys != NULL ? luaH_newrecord(L, keys, tasklocal)
                      : luaH_newtable(L, array, hash, tasklocal);
  sethvalue2s(L, L->top.p - 1, table);
  if (luaC_hastaskheap(L) && L->task_nursery_bytes >= L->task_nursery_limit)
    luaC_taskstep(L);
  luaC_condGC(L, (void)0, (void)0);
  luaX_pollrequests(L);
}

/* Allocation has the same saved instruction and stack roots as NEWTABLE.
** Return to the interpreter if the operation enabled a hook or a trap. */
int luaV_loopnewtable (LuaLoopFrame *frame, unsigned offset) {
  lua_State *L = frame->thread;
  CallInfo *ci = frame->call;
  const Instruction *at = frame->entry + offset;
  ci->u.l.rootpc = at;
  ci->u.l.savedpc = at + 2;
  createtable(L, ci, at, at[0], at[1]);
  frame->base = ci->func.p + 1;
  return !gethookmask(L) && !ci->u.l.trap;
}

/* No register changes until the binding and argument support a bounded call. */
static int __attribute__((noinline)) trystringrangecall (
    lua_State *L, const TValue *receiver, TString *key, const TValue *index,
    StkId result, unsigned crossed) {
  Table *methods;
  TValue method;
  const char *bytes;
  size_t length;
  lua_Unsigned position;
  int sub = key->shrlen == 3;  /* lowering selects "byte" or "sub" */
  if (!ttisstring(receiver) || !ttisinteger(index)) return 0;
  position = l_castS2U(ivalue(index)) - 1;
  bytes = getlstr(tsvalue(receiver), length);
  if (position >= length) return 0;
  methods = luaA_stringindex(L);
  if (methods == NULL) return 0;
  if (!(gettableflags(methods) & LUA_TABLE_STRING_RANGES)) {
    luaC_tracepointer(L, methods);
    if (!luaH_readshortstr(G(L), L, methods, key, &method) ||
        !checktag(&method, LUA_VBCF) ||
        fvalue(&method) != (sub ? luaB_strsub : luaB_strbyte))
      return 0;
  }
  if (gethookmask(L)) return 0;
  setobj2s(L, result + 1, receiver);
  setobj2s(L, result + 2, index);
  if (crossed == 3) setobj2s(L, result + 3, index);
  if (sub) {
    setsvalue2s(L, result, &G(L)->byte_strings[cast_uchar(bytes[position])]);
  }
  else { setivalue(s2v(result), cast_uchar(bytes[position])); }
  L->top.p = result + 1;
  return 1;
}


/* Open cells are ordered by descending stack level. Captures in callers do
** not require register synchronization in an uncaptured executing frame. */
#define haslocalcaptures(ci) \
  (VM_CAPTURE && (ci)->u.l.captures != 0)

/* fetch an instruction and prepare its execution */
#define vmfetch()	{ \
  ip = NULL; \
  if (l_unlikely(--L->poll_remaining == 0)) { \
    ptrdiff_t top = savestack(L, L->top.p); \
    savepc(ci); \
    if (L->top.p < ci->top.p) L->top.p = ci->top.p; \
    L->poll_remaining = 1024; \
    luaX_poll(L); \
    if (L->task_nursery_pending && L->native_depth == 0) luaC_taskstep(L); \
    updatebase(ci); \
    L->top.p = restorestack(L, top); \
    updatetrap(ci); \
  } \
  if (l_unlikely(trap)) {  /* stack reallocation or hooks? */ \
    ci->u.l.rootpc = NULL; \
    trap = luaG_traceexec(L, pc);  /* handle hooks */ \
    updatebase(ci);  /* correct stack */ \
  } \
  ip = pc; \
  i = *pc; \
  pc++; \
}

#define storeinstructionresults() { \
  if (l_unlikely(haslocalcaptures(ci))) { \
    savepc(ci); storeinstruction(L, ci, i, pc); updatebase(ci); \
  } \
}

typedef struct LuaInstructionFrame {
  LClosure *closure;
  TValue *constants;
  CallInfo *call;
  StkId base;
  const Instruction *pc, *ip;
  int trap;
} LuaInstructionFrame;

enum { VM_NEXT, VM_START, VM_RETURNING, VM_DONE, VM_SUSPENDED };
static const int32_t suspended;

/* A capture-aware instruction is generated from the same definitions as the
** register-only loop. Its single return boundary keeps ownership/cell paths
** out of ordinary dispatch and avoids replicating the fetch loop for them. */
static unsigned executecaptured (lua_State *L, Instruction i,
                                 LuaInstructionFrame *frame) {
  LClosure *cl = frame->closure;
  TValue *k = frame->constants;
  CallInfo *ci = frame->call;
  StkId base = frame->base;
  const Instruction *pc = frame->pc, *ip = frame->ip;
  int trap = frame->trap;
  unsigned result;
#undef vmbreak
#define vmbreak storeinstructionresults(); goto captured_next
#define vmcase(op) case op:
#define VM_CAPTURE 1
#define vmreturn goto captured_done
#define vmsuspend goto captured_suspended
#define startfunc captured_start
#define returning captured_returning
#define ret captured_ret
#define l_tforcall captured_tforcall
#define l_tforloop captured_tforloop
  switch (GET_OPCODE(i)) {
#include "lvmexecute.h"
  }
 captured_next:
  result = VM_NEXT;
  goto complete;
 captured_start:
  result = VM_START;
  goto complete;
 captured_returning:
  result = VM_RETURNING;
  goto complete;
 captured_done:
  result = VM_DONE;
  goto complete;
 captured_suspended:
  result = VM_SUSPENDED;
 complete:
  frame->call = ci;
  frame->base = base;
  frame->pc = pc;
  frame->trap = trap;
  return result;
#undef l_tforloop
#undef l_tforcall
#undef ret
#undef returning
#undef startfunc
#undef vmreturn
#undef vmsuspend
#undef VM_CAPTURE
#undef vmcase
#undef vmbreak
}


/* The linker and VM must see the same labels. GCC also needs distinct
** dispatch sites to preserve the branch history of each opcode. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noclone, optimize("no-gcse", "no-crossjumping")))
#else
__attribute__((noinline))
#endif
static const int32_t *execute (lua_State *L, CallInfo *ci) {
  LClosure *cl;
  const Proto *p;
  TValue *k;
  StkId base;
  const Instruction *code, *pc, *ip;
  const LuaDispatchSlot *dispatch;
  int trap;
#include "ljumptab.h"
  if (ci == NULL) return disptab;
 startfunc:
  cl = ci_func(ci);
  p = cl->p;
#if !defined(HARDSTACKTESTS)
 startclosure:
#endif
  if (l_unlikely(luaTK_hasstops(L))) luaTK_deliverstop(L);
  trap = gethookmask(L);
  goto loadframe;
 returning:  /* trap already set */
  cl = ci_func(ci);
  p = cl->p;
 loadframe:
  k = p->k;
  code = p->code;
  dispatch = p->dispatch;
  pc = ci->u.l.savedpc;
  if (l_unlikely(trap))
    trap = luaG_tracecall(L);
  base = ci->func.p + 1;
  /* main loop of interpreter */
  for (;;) {
    Instruction i;  /* instruction being executed */
    vmfetch();
    #if 0
    { /* low-level line tracing for debugging Lua */
      #include "lopnames.h"
      int pcrel = pcRel(pc, cl->p);
      printf("line: %d; %s (%d)\n", luaG_getfuncline(cl->p, pcrel),
             opnames[GET_OPCODE(i)], pcrel);
    }
    #endif
    lua_assert(base == ci->func.p + 1);
    lua_assert(base <= L->top.p && L->top.p <= L->stack_last.p);
    /* for tests, invalidate top for instructions not expecting it */
    lua_assert(luaP_isIT(i) || (cast_void(L->top.p = base), 1));
    {
      vmjump();
#define VM_CAPTURE 0
#define vmreturn return NULL
#define vmsuspend return &suspended
#define vmcase(op) L_fast_##op:
#define directdispatch(op) goto L_fast_##op
#define ret fast_ret
#define l_tforcall fast_tforcall
#define l_tforloop fast_tforloop
#include "lvmexecute.h"
  vmcase(LUA_STRING_RANGE_CALL) {
    unsigned crossed = GET_OPCODE(pc[1]) == OP_MOVE ? 3u : 2u;
    if (l_unlikely(trap || L->poll_remaining <= crossed || gethookmask(L)) ||
        !trystringrangecall(L, vRB(i), tsvalue(KC(i)), vRB(pc[0]), RA(i), crossed)) {
      directdispatch(OP_SELF);
    }
    L->poll_remaining -= crossed;
    pc += crossed;
    ip = pc - 1;
    if (l_unlikely(luaTK_pendingstop(L) != NULL ||
                   (L->policy != NULL && luaCP_exhausted(L)))) {
      savepc(ci);
      luaTK_checkstop(L);
      luaCP_checklimit(L);
    }
    updatetrap(ci);
    vmbreak;
  }
  vmcase(LUA_PROJECT_TABLE) {
    const LuaRecordSite *site = constructorsite(p, ip);
    lua_assert(site != NULL && site->projection != 0);
    if (l_unlikely(trap || ci->u.l.captures != 0 ||
                   L->poll_remaining <= site->projection || gethookmask(L))) {
      directdispatch(OP_NEWTABLE);
    }
    projecttable(L, p, ci, RA(i), pc + 1, site);
    pc += 1 + site->projection;  /* extra argument and projected operations */
    updatetrap(ci);
    vmbreak;
  }
#define RECORDSTORE(field) \
  vmcase(LUA_SETRECORD_##field) { \
    TValue *receiver = s2v(RA(i)); \
    if (l_unlikely(!ttistable(receiver) || \
        !luaH_trysetrecord(L, hvalue(receiver), tsvalue(KB(i)), field, RKC(i)))) { \
      directdispatch(OP_SETFIELD); \
    } \
    vmbreak; \
  }
  RECORDSTORE(0)
  RECORDSTORE(1)
  RECORDSTORE(2)
  RECORDSTORE(3)
#undef RECORDSTORE
/* A non-nil direct store gives the adjacent reload its value at the
** store's publication point. Nil and unresolved writes can run methods.
** Keep both original instructions at hooks and scheduling boundaries. */
#define STORELOAD(op, store, write, index) \
  vmcase(op) { \
    TValue *receiver = s2v(RA(i)); \
    TValue *value = RKC(i); \
    if (l_unlikely(trap || L->poll_remaining == 1 || \
        !ttistable(receiver) || ttisnil(value) || \
        !write(L, hvalue(receiver), index, value, 0))) { \
      directdispatch(store); \
    } \
    setobj2s(L, RA(*pc), value); \
    L->poll_remaining--; \
    pc++; \
    vmbreak; \
  }
  STORELOAD(LUA_RELOAD_FIELD, OP_SETFIELD, luaH_trysetfield, KB(i))
  STORELOAD(LUA_RELOAD_KEY, OP_SETTABLE, luaH_tryset, vRB(i))
  vmcase(LUA_RELOAD_INDEX) {
    TValue *receiver = s2v(RA(i));
    TValue *value = RKC(i);
    TValue key;
    setivalue(&key, GETARG_B(i));
    if (l_unlikely(trap || L->poll_remaining == 1 ||
        !ttistable(receiver) || ttisnil(value) ||
        !luaH_tryset(L, hvalue(receiver), &key, value, 0))) {
      directdispatch(OP_SETI);
    }
    setobj2s(L, RA(*pc), value);
    L->poll_remaining--;
    pc++;
    vmbreak;
  }
#undef STORELOAD
/* The first operation cannot suspend. A hook or poll due at the second
** instruction uses the ordinary fetch so the VM observes the same boundary.
** Enter the second handler with its original PCs before it can raise or park. */
#define VM_PAIR(first,second) \
  vmcase(LUA_PAIR_##first##_##second) { \
    if (l_unlikely(trap || L->poll_remaining == 1)) { directdispatch(first); } \
    execute_##first(i); \
    --L->poll_remaining; \
    ip = pc; \
    i = *pc++; \
    if ((second) == OP_MOVE) { execute_OP_MOVE(i); vmbreak; } \
    directdispatch(second); \
  }
#include "loppairs.h"
#undef VM_PAIR
  vmcase(LUA_NATIVE_LOOP) {
    LuaLoopSite *site;
    LuaLoopCode native;
    uintptr_t address;
    unsigned resume, control;
    if (l_unlikely(trap || gethookmask(L)))
      goto ordinaryloop;
    site = luaJ_loopat(p, cast_int(ip - code));
    control = GETARG_A(ip[site->words - 1]);
    if (!ttisinteger(s2v(base + control + 1))) goto ordinaryloop;
    address = __atomic_load_n(&site->entry, __ATOMIC_ACQUIRE);
    if (l_likely(address > LUA_LOOP_UNAVAILABLE)) native = cast(LuaLoopCode, address);
    else if (address == LUA_LOOP_COLD) {
      ptrdiff_t top = savestack(L, L->top.p);
      ci->u.l.rootpc = NULL;
      ci->u.l.savedpc = ip;
      if (L->top.p < ci->top.p) L->top.p = ci->top.p;
      native = cast(LuaLoopCode, luaJ_prepare(L, cl->p, site));
      updatebase(ci);
      L->top.p = restorestack(L, top);
      updatetrap(ci);
      if (native == NULL || trap || gethookmask(L)) goto ordinaryloop;
    }
    else goto ordinaryloop;
    {
      LuaLoopFrame frame = { .thread = L, .call = ci, .base = base,
                             .closure = cl, .entry = ip,
                             .bounded = (site->flags & LUA_LOOP_BOUNDED) != 0 };
      ++L->poll_remaining;  /* native code charges the already fetched word */
      resume = native(&frame);
#if !defined(HARDSTACKTESTS)
      if (frame.detached != NULL) {
        L->top.p = ci->top.p;
        ci->u.l.rootpc = NULL;  /* uncaptured registers are roots */
        ci->u.l.savedpc = ip + resume;
        luaX_leavesharedread(L);
        luaW_finishnativeframe(L, frame.detached);
        frame.base = ci->func.p + 1;
      }
#endif
      base = frame.base;
    }
    updatetrap(ci);
    if (resume == 0 && L->poll_remaining > 1 && !trap && !gethookmask(L)) {
      --L->poll_remaining;
      goto ordinaryloop;
    }
    pc = ip + resume;
    vmbreak;
   ordinaryloop:
    goto *(&&L_cells + disptab[GET_OPCODE(i)]);
  }
#undef directdispatch
#undef l_tforloop
#undef l_tforcall
#undef ret
#undef vmcase
#undef vmreturn
#undef vmsuspend
#undef VM_CAPTURE
    }
    continue;
   L_cells: {
      LuaInstructionFrame frame;
      unsigned result;
      frame.closure = cl;
      frame.constants = k;
      frame.call = ci;
      frame.base = base;
      frame.pc = pc;
      frame.ip = ip;
      frame.trap = trap;
      result = executecaptured(L, i, &frame);
      ci = frame.call;
      if (result == VM_DONE) return NULL;
      if (result == VM_SUSPENDED) return &suspended;
      if (result == VM_START) goto startfunc;
      if (result == VM_RETURNING) {
        trap = frame.trap;
        goto returning;
      }
      base = frame.base;
      pc = frame.pc;
      trap = frame.trap;
    }
  }
}

void luaV_linkdispatch (Proto *p) {
  const int32_t *offsets = execute(NULL, NULL);
  int pc;
  for (pc = 0; pc < p->sizecode; pc++) {
    unsigned op = p->dispatch[pc].opcode;
    lua_assert(op < LUA_DISPATCH_COUNT);
    p->dispatch[pc].offset = offsets[op];
  }
}

/* The native compiler runs after dispatch linking. Reuse the constructor
** field already resolved by lowering, including nonadjacent stores. */
int luaV_recordfield (const Proto *p, int pc) {
  const int32_t *offsets = execute(NULL, NULL);
  int op;
  for (op = LUA_SETRECORD_0; op <= LUA_SETRECORD_3; ++op)
    if (p->dispatch[pc].offset == offsets[op]) return op - LUA_SETRECORD_0;
  return -1;
}

int luaV_execute (lua_State *L, CallInfo *ci) {
  return execute(L, ci) == &suspended;
}

/* }================================================================== */
