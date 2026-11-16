/* Rooted inputs for trusted runtime libraries. See Copyright Notice in lua.h */
#ifndef lauxowned_h
#define lauxowned_h

#include "lroots.h"

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

#endif
