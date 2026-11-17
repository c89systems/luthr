/*
** Managed participation and collector rendezvous.
** See Copyright Notice in lua.h
*/
#ifndef lworld_h
#define lworld_h

#include <pthread.h>
#include "llimits.h"
#include "lexec.h"

typedef struct LuaParticipation {
  struct LuaParticipation *next;
  struct LuaParticipation **previous;
  lua_State *thread;
  unsigned active;  /* atomic; overlapping gate retirement/admission is allowed */
} LuaParticipation;

typedef struct LuaWorld {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  LuaOwner collector;
  LuaOwner heap_owner;
  LuaWaiter *waiters;
  struct LuaHeapWaiter *heap_head;
  struct LuaHeapWaiter **heap_tail;
  LuaParticipation *participants;
  unsigned heap_depth;
  int stopped;
  int ready;
} LuaWorld;

typedef struct LuaWorldScope {
  unsigned collection;
  unsigned heap;
} LuaWorldScope;

LUAI_FUNC int luaW_init (LuaWorld *world);
LUAI_FUNC void luaW_destroy (LuaWorld *world);
LUAI_FUNC void luaW_register (lua_State *L);
LUAI_FUNC void luaW_unregister (lua_State *L);
/* Caller holds world.mutex or has exclusive/quiescent VM ownership. */
LUAI_FUNC unsigned luaW_activecount (LuaWorld *world);
/* Admission records are local to a coroutine, not managed execution permits.
** These transitions require world.mutex or the entry/exit handshake below. */
LUAI_FUNC void luaW_activate (lua_State *L);
LUAI_FUNC void luaW_deactivate (lua_State *L);
LUAI_FUNC void luaW_enter (lua_State *L);
/* Cached before releasing stack ownership; never dereference its former owner
** after this call withdraws the last participation reference. */
LUAI_FUNC void luaW_leave (LuaWorld *world, LuaParticipation *participant);
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
