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
  unsigned collector;  /* immutable system role; does not expand mutator pacing */
  uint64_t storage_epoch;  /* last point with no optimistic storage borrow */
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
  lua_State *reclaimers;  /* atomic head; list/work ownership uses mutex */
  uint64_t work_epoch;
  struct LuaTableRetirement *storage_retired;  /* atomic producer list */
  struct LuaTableRetirement *storage_pending;  /* sole storage_busy consumer */
  uint64_t storage_epoch, storage_grace;
  size_t storage_bytes;
  unsigned storage_busy, storage_waiting, storage_workers;
  unsigned heap_depth;
  int stopped;
  int ready;
} LuaWorld;

typedef struct LuaWorldScope {
  unsigned collection;
  unsigned heap;
  LuaApiRegion *api_region;  /* detached activation, restored only on attach */
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
LUAI_FUNC void luaW_resume (lua_State *L);
LUAI_FUNC void luaW_deactivate (lua_State *L);
/* Complete an already published admission. Single-gate claims publish it in
** their CAS; paired claims publish only after both reservations are held. */
LUAI_FUNC void luaW_enter (lua_State *L);
LUAI_FUNC int luaW_tryenter (lua_State *L);
/* Global notification after the gate's final release publication. No state or
** participant record remains borrowed; queued means resource waiters exist. */
LUAI_FUNC void luaW_leave (LuaWorld *world, int queued);
/* The gate word is the authoritative admission; no independently retired
** counter can overlap its next owner. Safe while the directory is retained. */
LUAI_FUNC unsigned luaW_active (const LuaParticipation *participant);
LUAI_FUNC void luaW_stop (lua_State *L);
LUAI_FUNC void luaW_start (lua_State *L);
LUAI_FUNC void luaW_heapenter (lua_State *L);
LUAI_FUNC void luaW_collectenter (lua_State *L);
/* Phase polls may defer behind an existing metadata owner without parking. */
LUAI_FUNC int luaW_trycollectenter (lua_State *L);
LUAI_FUNC void luaW_heapleave (lua_State *L);
/* Caller owns world.mutex, retains native depth and borrows no shared work. */
LUAI_FUNC void luaW_waitcollector (lua_State *L);
LUAI_FUNC LuaWorldScope luaW_scope (lua_State *L);
LUAI_FUNC LuaWorldScope luaW_suspend (lua_State *L);
LUAI_FUNC void luaW_restore (lua_State *L, LuaWorldScope scope, unsigned native);
LUAI_FUNC LuaWorldScope luaW_detach (lua_State *L);
LUAI_FUNC void luaW_attach (lua_State *L, LuaWorldScope scope);
/* Suspend an attached runtime C activation for blocking or cross-state work.
** Its existing native-depth pin retains the activation across this pause. */
LUAI_FUNC LuaWorldScope luaW_suspendapi (void);
LUAI_FUNC void luaW_restoreapi (LuaWorldScope scope);
/* The caller is an admitted mutator or the collector itself. */
LUAI_FUNC int luaW_iscollector (lua_State *L);

#endif
