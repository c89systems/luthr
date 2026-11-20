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

/* Control publishes admission requests and the reclamation epoch together.
** Epochs use the masked word so modular comparisons still span 64 bits. */
enum {
  LUA_WORLD_STOPPED = 1,
  LUA_WORLD_STORAGE_WAIT = 2,
  LUA_WORLD_REQUESTS = LUA_WORLD_STOPPED | LUA_WORLD_STORAGE_WAIT,
  LUA_WORLD_EPOCH_STEP = 4
};

typedef struct LuaWorld {
  pthread_mutex_t mutex;
  pthread_cond_t changed, resumed, rendezvous;
  LuaOwner collector;
  LuaOwner heap_owner;
  LuaWaiter *waiters, *resume_waiters, *rendezvous_waiters;
  struct LuaHeapWaiter *heap_head;
  struct LuaHeapWaiter **heap_tail;
  LuaParticipation *participants;
  lua_State *reclaimers;  /* atomic head; list/work ownership uses mutex */
  uint64_t work_epoch;
  struct LuaTableRetirement *storage_retired;  /* atomic producer list */
  struct LuaTableRetirement *storage_pending;  /* sole storage_busy consumer */
  uint64_t control, storage_grace;
  size_t storage_bytes;
  unsigned storage_busy, storage_workers;
  unsigned storage_waiters;  /* mutex-owned count behind the request bit */
  unsigned heap_depth;
  unsigned entry_waiting;  /* mutex-owned batch owed admission after a stop */
} LuaWorld;

l_sinline uint64_t luaW_control (const LuaWorld *world) {
  return __atomic_load_n(&world->control, __ATOMIC_SEQ_CST);
}

l_sinline int luaW_stopped (const LuaWorld *world) {
  return (luaW_control(world) & LUA_WORLD_STOPPED) != 0;
}

l_sinline uint64_t luaW_epoch (uint64_t control) {
  return control & ~cast(uint64_t, LUA_WORLD_REQUESTS);
}

LUAI_FUNC int luaW_init (struct global_State *g);
LUAI_FUNC void luaW_destroy (struct global_State *g);
LUAI_FUNC void luaW_register (lua_State *L);
LUAI_FUNC void luaW_unregister (lua_State *L);
/* Enroll the owner of a nonempty allocation or table-retirement batch. */
LUAI_FUNC void luaW_trackpublisher (lua_State *L);
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
** participant record remains borrowed; flags describe the released gate. */
LUAI_FUNC void luaW_leave (LuaWorld *world, unsigned flags);
/* The gate word is the authoritative admission; no independently retired
** counter can overlap its next owner. Safe while the directory is retained. */
LUAI_FUNC unsigned luaW_active (const LuaParticipation *participant);
LUAI_FUNC void luaW_stop (lua_State *L);
LUAI_FUNC void luaW_start (lua_State *L);
/* Caller retains its stack gate and holds world.mutex while inactive. */
LUAI_FUNC void luaW_waitresume (lua_State *L);
LUAI_FUNC void luaW_heapenter (lua_State *L);
LUAI_FUNC void luaW_collectenter (lua_State *L);
/* Phase polls may defer behind an existing metadata owner without parking. */
LUAI_FUNC int luaW_trycollectenter (lua_State *L);
LUAI_FUNC void luaW_heapleave (lua_State *L);
/* Caller owns world.mutex, retains native depth and borrows no shared work. */
enum { LUA_WAIT_RECLAIM, LUA_WAIT_PHASE, LUA_WAIT_MINOR, LUA_WAIT_TRACE };
LUAI_FUNC void luaW_waitcollector (lua_State *L, int reason);
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
