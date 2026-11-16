/*
** Managed participation and collector rendezvous.
** See Copyright Notice in lua.h
*/
#ifndef lworld_h
#define lworld_h

#include <pthread.h>
#include "llimits.h"
#include "lexec.h"

typedef struct LuaWorld {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  LuaOwner collector;
  LuaOwner heap_owner;
  LuaWaiter *waiters;
  unsigned heap_depth;
  unsigned active;
  int stopped;
  int ready;
} LuaWorld;

typedef struct LuaWorldScope {
  unsigned collection;
  unsigned heap;
} LuaWorldScope;

LUAI_FUNC int luaW_init (LuaWorld *world);
LUAI_FUNC void luaW_destroy (LuaWorld *world);
LUAI_FUNC void luaW_enter (lua_State *L);
LUAI_FUNC void luaW_leave (lua_State *L);
LUAI_FUNC void luaW_stop (lua_State *L);
LUAI_FUNC void luaW_start (lua_State *L);
LUAI_FUNC void luaW_heapenter (lua_State *L);
LUAI_FUNC void luaW_heapleave (lua_State *L);
LUAI_FUNC LuaWorldScope luaW_scope (lua_State *L);
LUAI_FUNC LuaWorldScope luaW_suspend (lua_State *L);
LUAI_FUNC void luaW_restore (lua_State *L, LuaWorldScope scope, unsigned native);
LUAI_FUNC LuaWorldScope luaW_detach (lua_State *L);
LUAI_FUNC void luaW_attach (lua_State *L, LuaWorldScope scope);
/* The caller is an admitted mutator or the collector itself. */
LUAI_FUNC int luaW_iscollector (lua_State *L);

#endif
