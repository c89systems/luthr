/* Owned library control calls. See Copyright Notice in lua.h */
#ifndef lcontrol_h
#define lcontrol_h

#include "lua.h"
#include "llimits.h"

/* Private identities let the facade validate the implementations it owns. */
LUAI_FUNC int luaB_pcall (lua_State *L);
LUAI_FUNC int luaB_xpcall (lua_State *L);
LUAI_FUNC int luaB_cocreate (lua_State *L);
LUAI_FUNC int luaB_coresume (lua_State *L);
LUAI_FUNC int luaB_close (lua_State *L);
LUAI_FUNC int luaB_cowrap (lua_State *L);
LUAI_FUNC int luaB_auxwrap (lua_State *L);
LUAI_FUNC int luaB_yield (lua_State *L);

#endif
