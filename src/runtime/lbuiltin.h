/* Private library entry points used by the VM. See lua.h for license. */
#ifndef lbuiltin_h
#define lbuiltin_h

#include "lobject.h"

LUAI_FUNC int luaB_strsub (lua_State *L);
LUAI_FUNC int luaB_strbyte (lua_State *L);
LUAI_FUNC int luaB_mathabs (lua_State *L);
LUAI_FUNC int luaB_assert (lua_State *L);
LUAI_FUNC int luaB_select (lua_State *L);
LUAI_FUNC int luaB_rawget (lua_State *L);
LUAI_FUNC int luaB_rawequal (lua_State *L);

l_sinline int luaB_isleaf (lua_CFunction f) {
  return f == luaB_strsub || f == luaB_strbyte || f == luaB_mathabs ||
         f == luaB_assert || f == luaB_select || f == luaB_rawget ||
         f == luaB_rawequal;
}

/* Read only the owned argument slots. A negative return needs a C frame;
** zero or one returns are complete without allocation, hooks or parking. */
LUAI_FUNC int luaB_leafrange (lua_State *L, StkId func, int nargs, int bytes,
                              TValue *result);

l_sinline lua_Integer luaB_selectindex (lua_Integer index, int count) {
  if (index < 0) return count + index;
  return index > count ? count : index;
}

l_sinline lua_Integer luaB_absinteger (lua_Integer n) {
  return n < 0 ? l_castU2S(0u - l_castS2U(n)) : n;
}

#endif
