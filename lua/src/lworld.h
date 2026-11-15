/*
** Managed participation and collector rendezvous.
** See Copyright Notice in lua.h
*/
#ifndef lworld_h
#define lworld_h

#include <pthread.h>
#include "llimits.h"

typedef struct LuaWorld {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_t collector;
  unsigned active;
  int stopped;
  int ready;
} LuaWorld;

LUAI_FUNC int luaW_init (LuaWorld *world);
LUAI_FUNC void luaW_destroy (LuaWorld *world);
LUAI_FUNC void luaW_enter (lua_State *L);
LUAI_FUNC void luaW_leave (lua_State *L);
LUAI_FUNC void luaW_stop (lua_State *L);
LUAI_FUNC void luaW_start (lua_State *L);
LUAI_FUNC unsigned luaW_suspend (lua_State *L);
LUAI_FUNC void luaW_restore (lua_State *L, unsigned depth, unsigned native);
LUAI_FUNC unsigned luaW_detach (lua_State *L);
LUAI_FUNC void luaW_attach (lua_State *L, unsigned depth);

#endif
