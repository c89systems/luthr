/* Private library entry points used by the VM. See lua.h for license. */
#ifndef lbuiltin_h
#define lbuiltin_h

#include "lobject.h"

LUAI_FUNC int luaB_strsub (lua_State *L);
LUAI_FUNC int luaB_strbyte (lua_State *L);
LUAI_FUNC int luaB_mathabs (lua_State *L);

l_sinline int luaB_isleaf (lua_CFunction f) {
  return f == luaB_strsub || f == luaB_strbyte || f == luaB_mathabs;
}

/* Read only the owned argument slots. A negative return needs a C frame;
** zero or one returns are complete without allocation, hooks or parking. */
LUAI_FUNC int luaB_leafrange (lua_State *L, StkId func, int nargs, int bytes,
                              TValue *result);

l_sinline lua_Integer luaB_absinteger (lua_Integer n) {
  return n < 0 ? l_castU2S(0u - l_castS2U(n)) : n;
}

#endif
