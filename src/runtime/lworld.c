/*
** Managed participation and collector rendezvous.
** See Copyright Notice in lua.h
*/
#define lworld_c
#define LUA_CORE
#include "lprefix.h"

#include "lapi.h"
#include "laccess.h"
#include "lgate.h"
#include "lgc.h"
#include "lstate.h"
#include "lmem.h"
#include "ltablestorage.h"
#include "lworld.h"
#include "lscheduler.h"

typedef struct LuaHeapWaiter {
  struct LuaHeapWaiter *next;
  void *owner;
  LuaWaiter *waiters;
  pthread_cond_t changed;
  int collector;
} LuaHeapWaiter;

int luaW_init (global_State *g) {
  LuaWorld *world = &g->world;
  int status, i;
  world->control = 0;
  world->entry_waiting = 0;
  world->heap_depth = 0;
  world->heap_owner.token = world->collector.token = NULL;
  world->participants = NULL;
  world->reclaimers = NULL;
  world->work_epoch = 0;
  world->storage_retired = world->storage_pending = NULL;
  world->storage_grace = 0;
  world->storage_bytes = 0;
  world->storage_busy = world->storage_waiters = world->storage_workers = 0;
  world->waiters = g->phase_waiters = NULL;
  world->heap_head = NULL;
  world->heap_tail = &world->heap_head;
  status = pthread_mutex_init(&world->mutex, NULL);
  if (status != 0)
    return status;
  status = pthread_cond_init(&world->changed, NULL);
  if (status != 0) {
    luaX_checksync(pthread_mutex_destroy(&world->mutex));
    return status;
  }
  status = pthread_cond_init(&g->phase_changed, NULL);
  if (status != 0) {
    luaX_checksync(pthread_cond_destroy(&world->changed));
    luaX_checksync(pthread_mutex_destroy(&world->mutex));
    return status;
  }
  for (i = 0; i < LUA_PUBLISH_SHARDS; i++) {
    g->publishers[i].head = NULL;
    status = pthread_mutex_init(&g->publishers[i].mutex, NULL);
    if (status != 0) {
      while (i-- > 0)
        luaX_checksync(pthread_mutex_destroy(&g->publishers[i].mutex));
      luaX_checksync(pthread_cond_destroy(&g->phase_changed));
      luaX_checksync(pthread_cond_destroy(&world->changed));
      luaX_checksync(pthread_mutex_destroy(&world->mutex));
      return status;
    }
  }
  return 0;
}

void luaW_destroy (global_State *g) {
  LuaWorld *world = &g->world;
  int i;
  lua_assert(!luaW_stopped(world) && world->entry_waiting == 0 &&
             world->participants == NULL &&
             world->heap_depth == 0 && world->waiters == NULL &&
             g->phase_waiters == NULL &&
             world->heap_head == NULL && world->reclaimers == NULL &&
             world->storage_bytes == 0 && world->storage_busy == 0 &&
             world->storage_workers == 0 &&
             world->storage_retired == NULL && world->storage_pending == NULL);
  for (i = 0; i < LUA_PUBLISH_SHARDS; i++) {
    lua_assert(g->publishers[i].head == NULL);
    luaX_checksync(pthread_mutex_destroy(&g->publishers[i].mutex));
  }
  luaX_checksync(pthread_cond_destroy(&g->phase_changed));
  luaX_checksync(pthread_cond_destroy(&world->changed));
  luaX_checksync(pthread_mutex_destroy(&world->mutex));
}

void luaW_register (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  LuaParticipation *participant = &L->participation;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(participant->previous == NULL && luaW_active(participant) == 0);
  participant->next = world->participants;
  participant->previous = &world->participants;
  if (participant->next != NULL)
    participant->next->previous = &participant->next;
  world->participants = participant;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

static LuaPublishShard *publishershard (lua_State *L) {
  uintptr_t address = cast(uintptr_t, L);
  return &G(L)->publishers[(address >> 8) & (LUA_PUBLISH_SHARDS - 1)];
}

static void unlinkpublisher (lua_State *L) {
  LuaPublishShard *shard;
  lua_State **previous;
  if (__atomic_load_n(&L->publish_previous, __ATOMIC_RELAXED) == NULL) return;
  shard = publishershard(L);
  luaX_checksync(pthread_mutex_lock(&shard->mutex));
  previous = __atomic_load_n(&L->publish_previous, __ATOMIC_RELAXED);
  lua_assert(previous != NULL && L->new_objects == NULL && L->new_tables == NULL &&
             L->storage_retired == NULL);
  *previous = L->publish_next;
  if (L->publish_next != NULL)
    __atomic_store_n(&L->publish_next->publish_previous, previous, __ATOMIC_RELAXED);
  __atomic_store_n(&L->publish_previous, NULL, __ATOMIC_RELAXED);
  luaX_checksync(pthread_mutex_unlock(&shard->mutex));
}

void luaW_unregister (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  LuaParticipation *participant = &L->participation;
  unlinkpublisher(L);
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(luaW_active(participant) == 0);
  if (participant->previous != NULL) {
    *participant->previous = participant->next;
    if (participant->next != NULL)
      participant->next->previous = participant->previous;
    participant->previous = NULL;
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

void luaW_trackpublisher (lua_State *L) {
  LuaPublishShard *shard;
  if (__atomic_load_n(&L->publish_previous, __ATOMIC_RELAXED) != NULL) return;
  shard = publishershard(L);
  luaX_checksync(pthread_mutex_lock(&shard->mutex));
  L->publish_next = shard->head;
  if (L->publish_next != NULL)
    __atomic_store_n(&L->publish_next->publish_previous, &L->publish_next,
                     __ATOMIC_RELAXED);
  __atomic_store_n(&L->publish_previous, &shard->head, __ATOMIC_RELAXED);
  shard->head = L;
  luaX_checksync(pthread_mutex_unlock(&shard->mutex));
}

unsigned luaW_activecount (LuaWorld *world) {
  LuaParticipation *participant;
  unsigned active = 0;
  for (participant = world->participants; participant != NULL;
       participant = participant->next)
    active += luaW_active(participant);
  return active;
}

unsigned luaW_active (const LuaParticipation *participant) {
  return (__atomic_load_n(&participant->thread->stack_gate.word,
                           __ATOMIC_SEQ_CST) & LUA_GATE_ACTIVE) != 0;
}

void luaW_activate (lua_State *L) {
  uintptr_t previous = __atomic_fetch_or(
    &L->admission->thread->stack_gate.word, LUA_GATE_ACTIVE, __ATOMIC_SEQ_CST);
  lua_assert(luaX_gateowner(previous) != NULL && !(previous & LUA_GATE_ACTIVE));
  UNUSED(previous);
}

static void withdraw (lua_State *L) {
  uintptr_t previous;
  /* Every withdrawal here holds world.mutex and retains its stack gate.
  ** A scanner may borrow these stable roots under that mutex, including
  ** when the owner cannot resume until a collection finishes. */
  L->root_waiting = 1;
  if (L->stack_peer != NULL) L->stack_peer->root_waiting = 1;
  previous = __atomic_fetch_and(&L->admission->thread->stack_gate.word,
                                ~cast(uintptr_t, LUA_GATE_ACTIVE),
                                __ATOMIC_SEQ_CST);
  lua_assert(previous & LUA_GATE_ACTIVE);
  UNUSED(previous);
}

void luaW_deactivate (lua_State *L) {
  luaM_publishdebt(L);
  withdraw(L);
}

void luaW_resume (lua_State *L) {
  /* Paired stack operations share an admission but retain both gates. */
  L->root_waiting = 0;
  if (L->stack_peer != NULL) L->stack_peer->root_waiting = 0;
  luaW_activate(L);
}

static void __attribute__((noinline)) awaitentry (lua_State *L, LuaWorld *world) {
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  /* Only the atomic participation record can change before admission. In
  ** particular, a late entrant cannot change roots while a collector scans
  ** this state. Its embedding caller retains the target's ordinary Lua root. */
  withdraw(L);  /* an unadmitted entrant cannot publish the stopped record */
  if (luaW_stopped(world)) {
    world->entry_waiting++;
    luaQ_wake(&world->changed, &world->waiters);
    do {
      luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
    } while (luaW_stopped(world));
    luaW_resume(L);
    if (--world->entry_waiting == 0)
      luaQ_wake(&world->changed, &world->waiters);
  }
  else luaW_resume(L);
  luaC_checksnapshot(L);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

/* Stack ownership may already be reserved, but managed access starts only
** after entry. Either the stop scan observes our active gate, or this load
** observes the stop before any managed access or storage borrow. */
void luaW_enter (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  uint64_t control = luaW_control(world);
  lua_assert(luaW_active(L->admission));
  luaHS_announce(world, L->admission, luaW_epoch(control));
  if (l_unlikely(control & LUA_WORLD_STOPPED)) awaitentry(L, world);
  else if (luaC_tracing(L)) luaC_checksnapshot(L);
}

int luaW_tryenter (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  if (luaW_stopped(world)) {
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
    return 0;
  }
  luaW_activate(L);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  luaHS_quiescent(L);
  luaC_checksnapshot(L);
  return 1;
}

void luaW_leave (LuaWorld *world, int queued) {
  /* The releasing gate has already withdrawn. Its state and participation
  ** record may be gone; notification uses only the retained VM directory. */
  if (queued || (luaW_control(world) & LUA_WORLD_REQUESTS)) {
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    luaQ_wake(&world->changed, &world->waiters);
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
  }
}

/* The requesting mutator retains its stack gate. Other mutators publish their
** roots before leaving managed execution, or finish their current API entry.
** Contending collectors park as pinned, inactive mutators before retrying. */
void luaW_stop (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  if (L->world_depth != 0) {
    L->world_depth++;
    return;
  }
  luaW_collectenter(L);  /* finish the current metadata scope before stopping */
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(!luaW_stopped(world));  /* metadata ownership orders collection requests */
  /* Admit the batch notified by the previous stop before stopping again.
  ** Metadata ownership prevents another stop while this batch drains. */
  if (world->entry_waiting != 0) {
    L->native_depth++;
    luaW_deactivate(L);
    luaQ_wake(&world->changed, &world->waiters);
    do {
      luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
    } while (world->entry_waiting != 0);
    luaW_resume(L);
    luaC_checksnapshot(L);
    L->native_depth--;
  }
  __atomic_store_n(&world->collector.token, luaX_token(L), __ATOMIC_RELEASE);
  __atomic_fetch_or(&world->control, LUA_WORLD_STOPPED, __ATOMIC_SEQ_CST);
  {
    LuaParticipation *participant;
    unsigned active = 0;
    for (participant = world->participants; participant != NULL;
         participant = participant->next)
      if (!participant->collector)
        active += luaW_active(participant);
    if (active > G(L)->pacer.parallelism) G(L)->pacer.parallelism = active;
  }
  luaQ_wake(&world->changed, &world->waiters);
  luaW_deactivate(L);
  while (luaW_activecount(world) != 0)
    luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
  L->world_depth = 1;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  luaC_publish(L);
  /* Publication drains the parked owners' retirement batches. Tentative
  ** entries cannot touch these buffers until the world resumes. */
  __atomic_store_n(&world->storage_grace,
    luaW_epoch(luaW_control(world)), __ATOMIC_RELEASE);
  luaC_drainbarriers(L);
}

void luaW_start (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  lua_assert(L->world_depth != 0);
  if (--L->world_depth != 0)
    return;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(luaW_stopped(world) && luaX_isowner(L, &world->collector));
  /* Late entrants may have published a tentative reference after the stop
  ** scan. They cannot access managed data until stopped becomes false. */
  luaW_resume(L);  /* the collector resumes as an ordinary mutator */
  __atomic_fetch_and(&world->control, ~cast(uint64_t, LUA_WORLD_STOPPED),
                     __ATOMIC_SEQ_CST);
  luaQ_wake(&world->changed, &world->waiters);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  luaSC_notifygc(G(L));
  luaC_checksnapshot(L);
  luaW_heapleave(L);
}

int luaW_iscollector (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  if (L->world_depth != 0) {
    lua_assert(luaW_stopped(world) &&
               luaX_isowner(L, &world->collector));
    return 1;
  }
  return luaW_stopped(world) &&
         luaX_isowner(L, &world->collector);
}

/* Reentrant within one execution (or unmanaged carrier), including operations
** on another coroutine's stack. A metadata waiter is pinned but not a mutator.
** The outermost acquisition must be a GC-safe boundary, before mutation. */
static void heapenter (lua_State *L, int collector) {
  LuaWorld *world = &G(L)->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  if (world->heap_depth != 0 &&
      luaX_isowner(L, &world->heap_owner)) {
    world->heap_depth++;
  }
  else {
    if (world->heap_depth != 0 || luaW_stopped(world)) {
      LuaHeapWaiter wait = {NULL, NULL, NULL, PTHREAD_COND_INITIALIZER, 0};
      lua_assert(L->world_depth == 0 &&
        luaW_active(L->admission) != 0);
      L->native_depth++;
      L->heap_waiting++;
      wait.owner = luaX_token(L);
      wait.collector = collector;
      if (collector) {
        LuaHeapWaiter **link = &world->heap_head;
        /* Allocation refills must not postpone an overdue collection behind
        ** a whole fanout of ordinary metadata requests. Preserve FIFO within
        ** each class, and never revoke a grant already handed to a waiter. */
        while (*link != NULL && (*link)->collector) link = &(*link)->next;
        wait.next = *link;
        *link = &wait;
        if (wait.next == NULL) world->heap_tail = &wait.next;
      }
      else {
        *world->heap_tail = &wait;
        world->heap_tail = &wait.next;
      }
      luaW_deactivate(L);
      luaQ_wake(&world->changed, &world->waiters);
      while (!luaX_isowner(L, &world->heap_owner))
        luaQ_wait(L, &world->mutex, &wait.changed, &wait.waiters);
      /* Ownership is reserved directly for this recipient. Fresh arrivals
      ** cannot steal it while its saved stack waits for execution capacity. */
      lua_assert(world->heap_depth == 1 && !luaW_stopped(world));
      luaX_checksync(pthread_cond_destroy(&wait.changed));
      luaW_resume(L);
      luaC_checksnapshot(L);
      L->heap_waiting--;
      L->native_depth--;
    }
    else {
      world->heap_depth = 1;
      __atomic_store_n(&world->heap_owner.token, luaX_token(L), __ATOMIC_RELEASE);
    }
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

void luaW_heapenter (lua_State *L) {
  heapenter(L, 0);
}

void luaW_collectenter (lua_State *L) {
  heapenter(L, 1);
}

int luaW_trycollectenter (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  int acquired = 0;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  if (world->heap_depth != 0 && luaX_isowner(L, &world->heap_owner)) {
    world->heap_depth++;
    acquired = 1;
  }
  else if (world->heap_depth == 0 && !luaW_stopped(world)) {
    world->heap_depth = 1;
    __atomic_store_n(&world->heap_owner.token, luaX_token(L), __ATOMIC_RELEASE);
    acquired = 1;
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  return acquired;
}

void luaW_heapleave (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(world->heap_depth != 0 &&
             luaX_isowner(L, &world->heap_owner));
  if (--world->heap_depth == 0) {
    LuaHeapWaiter *next = world->heap_head;
    if (next == NULL)
      __atomic_store_n(&world->heap_owner.token, NULL, __ATOMIC_RELEASE);
    else {
      world->heap_head = next->next;
      if (world->heap_head == NULL) world->heap_tail = &world->heap_head;
      world->heap_depth = 1;
      __atomic_store_n(&world->heap_owner.token, next->owner, __ATOMIC_RELEASE);
      if (next->waiters != NULL && next->waiters->execution != NULL)
        luaQ_requesthandoff(L);
      luaQ_wake(&next->changed, &next->waiters);
    }
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

static unsigned heapdepth (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  return luaW_ownsheap(L) ? world->heap_depth : 0;
}

void luaW_waitcollector (lua_State *L, int reason) {
  global_State *g = G(L);
  LuaWorld *world = &g->world;
  lua_assert(L->native_depth != 0);
  luaW_deactivate(L);
  if (reason == LUA_WAIT_RECLAIM || (luaW_control(world) & LUA_WORLD_REQUESTS))
    luaQ_wake(&world->changed, &world->waiters);
  if (reason == LUA_WAIT_RECLAIM)
    luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
  else if (!luaW_stopped(world))
    luaQ_wait(L, &world->mutex, &g->phase_changed, &g->phase_waiters);
  while (luaW_stopped(world))
    luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
  luaW_resume(L);
  luaC_checksnapshot(L);
}

static void restoreheap (lua_State *L, unsigned depth) {
  unsigned current = heapdepth(L);
  while (current > depth) {
    luaW_heapleave(L);
    current--;
  }
  while (current < depth) {
    luaW_heapenter(L);
    current++;
  }
}

LuaWorldScope luaW_scope (lua_State *L) {
  LuaWorldScope scope;
  scope.api_region = NULL;
  scope.collection = L->world_depth;
  scope.heap = heapdepth(L);
  if (scope.collection != 0) {
    lua_assert(scope.heap != 0);
    scope.heap--;  /* exclude the collector's implicit metadata ownership */
  }
  return scope;
}

LuaWorldScope luaW_suspend (lua_State *L) {
  LuaWorldScope scope = luaW_scope(L);
  unsigned remaining = scope.heap;
  if (scope.collection != 0) {
    L->world_depth = 1;
    luaW_start(L);
  }
  while (remaining-- != 0)
    luaW_heapleave(L);
  return scope;
}

void luaW_restore (lua_State *L, LuaWorldScope scope, unsigned native) {
  if (L->world_depth != scope.collection) {
    if (L->world_depth != 0) {
      L->world_depth = 1;
      luaW_start(L);
    }
    restoreheap(L, scope.heap);
    if (scope.collection != 0) {
      luaW_stop(L);
      L->world_depth = scope.collection;
    }
  }
  else
    restoreheap(L, scope.heap + (scope.collection != 0));
  L->native_depth = native;
}

/* These boundaries retain the coroutine while native code blocks or reenters.
** Protected unwinding restores the enclosing depth if a callback never returns. */
LuaWorldScope luaW_detach (lua_State *L) {
  LuaWorldScope scope;
#if defined(LUAI_ASSERT)
  LuaObjectScope *object;
  for (object = L->object_scopes; object != NULL; object = object->previous)
    lua_assert(!object->held);  /* callbacks never inherit object ownership */
#endif
  L->native_depth++;
  scope = luaW_suspend(L);
  scope.api_region = luaX_context(L)->api_region;
  luaX_context(L)->api_region = NULL;
  luaX_unlockstate(L);
  return scope;
}

void luaW_attach (lua_State *L, LuaWorldScope scope) {
  luaX_lockstate(L);
  luaW_restore(L, scope, L->native_depth);
  lua_assert(L->native_depth != 0);
  L->native_depth--;
  luaX_context(L)->api_region = scope.api_region;
}

LuaWorldScope luaW_suspendapi (void) {
  LuaWorldScope scope = {0};
  NativeStackContext *context = luaE_getnativestackcontext();
  if (context->api_region != NULL) {
    lua_State *L = context->api_region->state;
    lua_assert(L->native_depth != 0);
    scope = luaW_suspend(L);
    scope.api_region = context->api_region;
    context->api_region = NULL;
    luaX_unlockstate(L);
  }
  return scope;
}

void luaW_restoreapi (LuaWorldScope scope) {
  if (scope.api_region != NULL) {
    lua_State *L = scope.api_region->state;
    luaX_lockstate(L);
    luaW_restore(L, scope, L->native_depth);
    luaX_context(L)->api_region = scope.api_region;
  }
}
