/* Private bounded chunk pipes. See Copyright Notice in lua.h */
#ifndef lpipe_h
#define lpipe_h

#include "lua.h"
#include "llimits.h"

enum { LUA_PIPE_CHUNK_MAX = 1024, LUA_PIPE_SLOTS_MAX = 64 };
/* Push a pipe userdata. Zero selects one chunk slot. Methods are trusted
** runtime closures: send(string), recv(), close(), stats(). Close drains queued
** chunks before EOF; finalization aborts queued data and defers mutex retirement
** until entered operations finish. Registration is not a public SDK contract. */
LUAI_FUNC void luaP_newpipe (lua_State *L, unsigned slots);

#endif
