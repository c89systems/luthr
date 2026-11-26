/* Rooted inputs for trusted runtime libraries. See Copyright Notice in lua.h */
#ifndef lauxowned_h
#define lauxowned_h

#include "lauxlib.h"
#include "lapi.h"
#include "lroots.h"

/* Caller owns the stack. A missing or non-string argument leaves root unused. */
l_sinline const char *luaL_tryrootstring (lua_State *L, int arg,
                                         LuaNativeRoot *root, size_t *len) {
  lua_assert(arg > 0);
  if (arg < L->top.p - L->ci->func.p) {
    const TValue *value = s2v(L->ci->func.p + arg);
    if (ttisstring(value)) {
      luaR_pushroot(L, root);
      luaR_setroot(L, root, value);
      return getlstr(tsvalue(value), *len);
    }
  }
  return NULL;
}

/* Return with the stack owned. Numeric conversion and argument errors can
** call back into Lua, so perform them outside this ownership scope. */
l_sinline const char *luaL_ownedrootstring (lua_State *L, int arg,
                                           LuaNativeRoot *root, size_t *len) {
  const char *string;
  while ((string = luaL_tryrootstring(L, arg, root, len)) == NULL) {
    lua_unlock(L);
    luaL_checklstring(L, arg, NULL);
    lua_lock(L);
  }
  return string;
}

/* Positive argument indices only. Storage belongs to the current C activation;
** protected unwinding retires abandoned roots. Call outside state ownership.
** Normal completion removes the contiguous root array in reverse order. */
LUAI_FUNC void luaL_rootarg (lua_State *L, int arg, LuaNativeRoot *root);
LUAI_FUNC const char *luaL_rootstring (lua_State *L, int arg,
                                     LuaNativeRoot *root, size_t *len);
LUAI_FUNC const char *luaL_roottostring (lua_State *L, LuaNativeRoot *root,
                                       size_t *len);
LUAI_FUNC void luaL_pushrooted (lua_State *L, const LuaNativeRoot *root);
LUAI_FUNC int luaL_rootresult (lua_State *L, LuaNativeRoot *roots,
                              unsigned count, int results);
LUAI_FUNC void luaL_pushstringresult (luaL_Buffer *B);

#endif
