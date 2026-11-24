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
  unsigned collector;  /* world.mutex; scheduler root until retired */
  uint64_t storage_epoch;  /* last point with no optimistic storage borrow */
  uint64_t storage_scan;  /* tagged epoch of an active collector buffer borrow */
  uint64_t trace_armed;  /* written with the stack gate and world.mutex */
} LuaParticipation;

/* A single snapshot covers collector colours, admission requests, and the
** storage epoch. Epoch arithmetic uses the masked word and wraps at 64 bits.
** The low byte also supplies the initial colour of a new object. */
enum {
  LUA_MARK_YOUNG = 1u << 8,
  LUA_MARK_ACTIVE = 1u << 9,
  LUA_MARK_ARMING = 1u << 10,
  LUA_WORLD_MARK_MASK = (1u << 11) - 1,
  LUA_WORLD_STOPPED = 1u << 11,
  LUA_WORLD_STORAGE_WAIT = 1u << 12,
  LUA_WORLD_ARM_TRACE = 1u << 13,
  LUA_WORLD_STACK_ROOTS = 1u << 14,
  LUA_WORLD_SEAL_NURSERIES = 1u << 15,
  LUA_WORLD_REQUESTS = LUA_WORLD_STOPPED | LUA_WORLD_STORAGE_WAIT |
                       LUA_WORLD_ARM_TRACE | LUA_WORLD_SEAL_NURSERIES,
  LUA_WORLD_EPOCH_STEP = 1u << 16
};

enum { LUA_WORLD_RETAIN_COLLECTOR = 1, LUA_WORLD_RETAIN_MUTATORS = 2 };

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
  uint64_t arm_epoch;  /* atomic epoch published before the arm request */
  struct LuaTableRetirement *storage_retired;  /* atomic producer list */
  struct LuaTableRetirement *storage_pending;  /* sole storage_busy consumer */
  uint64_t control, storage_grace;
  size_t storage_bytes;
  unsigned storage_busy, storage_workers;
  unsigned storage_scanners;  /* atomic count above the collector wait bit */
  unsigned storage_waiters;  /* mutex-owned count behind the request bit */
  unsigned storage_scan_waiters;  /* mutex-owned count behind the scan wait bit */
  unsigned heap_depth;  /* owner-only after the outermost grant */
  unsigned entry_waiting;  /* mutex-owned batch owed admission after a stop */
  unsigned stop_slots;  /* metadata owner sets before stopping, cleared at resume */
  LuaParticipation *rendezvous_cursor;  /* world.mutex, unchecked suffix */
} LuaWorld;

l_sinline uint64_t luaW_control (const LuaWorld *world) {
  return __atomic_load_n(&world->control, __ATOMIC_SEQ_CST);
}

l_sinline uint64_t luaW_armepoch (const LuaWorld *world) {
  return __atomic_load_n(&world->arm_epoch, __ATOMIC_ACQUIRE);
}

l_sinline int luaW_stopped (const LuaWorld *world) {
  return (luaW_control(world) & LUA_WORLD_STOPPED) != 0;
}

l_sinline int luaW_heapheld (const LuaWorld *world) {
  return __atomic_load_n(&world->heap_owner.token, __ATOMIC_ACQUIRE) != NULL;
}

l_sinline uint64_t luaW_epoch (uint64_t control) {
  return control & ~cast(uint64_t, LUA_WORLD_EPOCH_STEP - 1);
}

LUAI_FUNC int luaW_init (struct global_State *g);
LUAI_FUNC void luaW_destroy (struct global_State *g);
LUAI_FUNC void luaW_register (lua_State *L);
/* First entry holds an inactive reservation and world.mutex. */
LUAI_FUNC void luaW_enroll (lua_State *L);
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
LUAI_FUNC void luaW_leave (struct global_State *g, uintptr_t stack,
                           unsigned flags, uint64_t control);
/* The gate word is the authoritative admission; no independently retired
** counter can overlap its next owner. Safe while the directory is retained. */
LUAI_FUNC unsigned luaW_active (const LuaParticipation *participant);
LUAI_FUNC void luaW_stop (lua_State *L);
LUAI_FUNC void luaW_start (lua_State *L);
LUAI_FUNC void luaW_armtrace (lua_State *L);
LUAI_FUNC void luaW_acktrace (lua_State *L);
/* Caller retains its stack gate and holds world.mutex while inactive. */
LUAI_FUNC void luaW_waitresume (lua_State *L);
LUAI_FUNC void luaW_heapenter (lua_State *L);
LUAI_FUNC void luaW_collectenter (lua_State *L);
/* Phase polls may defer behind an existing metadata owner without parking. */
LUAI_FUNC int luaW_trycollectenter (lua_State *L);
LUAI_FUNC void luaW_heapleave (lua_State *L);
/* Caller owns world.mutex, retains native depth and borrows no shared work. */
enum { LUA_WAIT_RECLAIM, LUA_WAIT_PHASE, LUA_WAIT_MINOR, LUA_WAIT_TRACE,
       LUA_WAIT_ARM, LUA_WAIT_START };
LUAI_FUNC void luaW_waitcollector (lua_State *L, int reason);
LUAI_FUNC LuaWorldScope luaW_scope (lua_State *L);
LUAI_FUNC LuaWorldScope luaW_suspend (lua_State *L);
LUAI_FUNC void luaW_restore (lua_State *L, LuaWorldScope scope, unsigned native);
LUAI_FUNC LuaWorldScope luaW_detach (lua_State *L);
LUAI_FUNC void luaW_attach (lua_State *L, LuaWorldScope scope);
/* A returning C activation reattaches before ending native admission. */
LUAI_FUNC void luaW_returnnative (lua_State *L, LuaWorldScope scope,
                                   struct NativeStackContext *context);
/* Suspend an attached runtime C activation for blocking or cross-state work.
** Its existing native-depth pin retains the activation across this pause. */
LUAI_FUNC LuaWorldScope luaW_suspendapi (void);
LUAI_FUNC void luaW_restoreapi (LuaWorldScope scope);
/* The caller is an admitted mutator or the collector itself. */
LUAI_FUNC int luaW_iscollector (lua_State *L);

#endif
