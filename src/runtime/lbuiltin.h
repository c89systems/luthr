/* Private library entry points used by the VM. See lua.h for license. */
#ifndef lbuiltin_h
#define lbuiltin_h

#include "lgc.h"

LUAI_FUNC int luaB_strsub (lua_State *L);
LUAI_FUNC int luaB_strbyte (lua_State *L);
LUAI_FUNC int luaB_mathabs (lua_State *L);
LUAI_FUNC int luaB_assert (lua_State *L);
LUAI_FUNC int luaB_select (lua_State *L);
LUAI_FUNC int luaB_rawget (lua_State *L);
LUAI_FUNC int luaB_rawequal (lua_State *L);
LUAI_FUNC int luaB_tonumber (lua_State *L);

l_sinline int luaB_isleaf (lua_CFunction f) {
  return f == luaB_strsub || f == luaB_strbyte || f == luaB_mathabs ||
         f == luaB_assert || f == luaB_select || f == luaB_rawget ||
         f == luaB_rawequal || f == luaB_tonumber;
}

/*
** Translate a relative initial string position
** (negative means back from end): clip result to [1, inf).
** The length of any string in Lua must fit in a lua_Integer,
** so there are no overflows in the casts.
** The inverted comparison avoids a possible overflow
** computing '-pos'.
*/
l_sinline size_t luaB_firstpos (lua_Integer pos, size_t len) {
  if (pos > 0)
    return (size_t)pos;
  else if (pos == 0)
    return 1;
  else if (pos < -(lua_Integer)len)  /* inverted comparison */
    return 1;  /* clip to 1 */
  else return len + (size_t)pos + 1;
}


/*
** Translate the ending position. Negative means back from end.
** Clip the result to [0, len].
*/
l_sinline size_t luaB_lastpos (lua_Integer pos, size_t len) {
  if (pos > (lua_Integer)len)
    return len;
  else if (pos >= 0)
    return (size_t)pos;
  else if (pos < -(lua_Integer)len)
    return 0;
  else return len + (size_t)pos + 1;
}

/* The caller owns the argument slots and has at least one argument.
** A negative result needs a C frame. Zero or one returns are complete. */
l_sinline int luaB_leafrange (lua_State *L, StkId func, int nargs, int bytes,
                              TValue *result) {
  const char *source;
  size_t len, start, end;
  lua_Integer first = 1, last;
  lua_assert(nargs >= 1);
  if (!ttisstring(s2v(func + 1)))
    return -1;
  if (nargs >= 2 && !ttisnil(s2v(func + 2))) {
    if (!ttisinteger(s2v(func + 2))) return -1;
    first = ivalue(s2v(func + 2));
  }
  else if (!bytes)
    return -1;
  last = bytes ? first : -1;
  if (nargs >= 3 && !ttisnil(s2v(func + 3))) {
    if (!ttisinteger(s2v(func + 3))) return -1;
    last = ivalue(s2v(func + 3));
  }
  source = getlstr(tsvalue(s2v(func + 1)), len);
  start = luaB_firstpos(first, len);
  end = luaB_lastpos(last, len);
  if (start < end) return -1;
  if (bytes) {
    if (start > end) return 0;
    setivalue(result, cast_uchar(source[start - 1]));
  }
  else {
    TString *ts = start > end ? &G(L)->empty_string
                              : &G(L)->byte_strings[cast_uchar(source[start - 1])];
    setsvalue(L, result, ts);
  }
  return 1;
}

l_sinline lua_Integer luaB_selectindex (lua_Integer index, int count) {
  if (index < 0) return count + index;
  return index > count ? count : index;
}

l_sinline lua_Integer luaB_absinteger (lua_Integer n) {
  return n < 0 ? l_castU2S(0u - l_castS2U(n)) : n;
}

#endif
