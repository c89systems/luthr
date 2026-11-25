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

static void notifyrendezvous (global_State *g, lua_State *L);

/* The owner updates nesting. The queue only checks for zero under its
** mutex, which also serializes transitions into and out of ownership. */
l_sinline unsigned heapcount (const LuaWorld *world) {
  return __atomic_load_n(&world->heap_depth, __ATOMIC_RELAXED);
}

l_sinline void setheapcount (LuaWorld *world, unsigned depth) {
  __atomic_store_n(&world->heap_depth, depth, __ATOMIC_RELAXED);
}

/* With no execution holding metadata, an open arm still owns the phase.
** Grant the next queued request after it publishes readiness. */
static void grantheap (LuaWorld *world, lua_State *L) {
  LuaHeapWaiter *next;
  lua_assert(heapcount(world) == 0);
  next = world->heap_head;
  if (next == NULL || (luaW_control(world) & LUA_WORLD_ARM_TRACE)) {
    __atomic_store_n(&world->heap_owner.token, NULL, __ATOMIC_RELEASE);
    return;
  }
  world->heap_head = next->next;
  if (world->heap_head == NULL) world->heap_tail = &world->heap_head;
  setheapcount(world, 1);
  __atomic_store_n(&world->heap_owner.token, next->owner, __ATOMIC_RELEASE);
  if (L != NULL && next->waiters != NULL && next->waiters->execution != NULL)
    luaQ_requesthandoff(L);
  luaQ_wake(&next->changed, &next->waiters);
}

int luaW_init (global_State *g) {
  LuaWorld *world = &g->world;
  int status, i;
  world->control = bitmask(MAJOR0BIT);
  world->entry_waiting = 0;
  world->stop_slots = 0;
  g->seals_pending = 0;
  world->rendezvous_cursor = NULL;
  world->heap_depth = 0;
  world->heap_owner.token = world->collector.token = NULL;
  world->participants = NULL;
  world->reclaimers = NULL;
  world->work_epoch = 0;
  world->arm_epoch = 0;
  world->storage_retired = world->storage_pending = NULL;
  world->storage_grace = 0;
  world->storage_bytes = 0;
  world->storage_busy = world->storage_waiters = world->storage_workers = 0;
  world->storage_scanners = 0;
  world->storage_scan_waiters = 0;
  world->waiters = g->phase_waiters = g->minor_waiters = g->trace_waiters = NULL;
  world->resume_waiters = world->rendezvous_waiters = NULL;
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
  status = pthread_cond_init(&world->resumed, NULL);
  if (status != 0) goto failed_changed;
  status = pthread_cond_init(&world->rendezvous, NULL);
  if (status != 0) goto failed_resumed;
  status = pthread_cond_init(&g->phase_changed, NULL);
  if (status != 0) goto failed_rendezvous;
  status = pthread_mutex_init(&g->trace_mutex, NULL);
  if (status != 0) goto failed_phase;
  status = pthread_cond_init(&g->trace_changed, NULL);
  if (status != 0) goto failed_trace_mutex;
  for (i = 0; i < LUA_PUBLISH_SHARDS; i++) {
    g->publishers[i].head = NULL;
    status = pthread_mutex_init(&g->publishers[i].mutex, NULL);
    if (status != 0) {
      while (i-- > 0)
        luaX_checksync(pthread_mutex_destroy(&g->publishers[i].mutex));
      goto failed_trace;
    }
  }
  for (i = 0; i < LUA_HASH_WAIT_SHARDS; i++) {
    status = pthread_mutex_init(&g->hash_wait_mutex[i], NULL);
    if (status != 0) {
      while (i-- > 0)
        luaX_checksync(pthread_mutex_destroy(&g->hash_wait_mutex[i]));
      for (i = 0; i < LUA_PUBLISH_SHARDS; i++)
        luaX_checksync(pthread_mutex_destroy(&g->publishers[i].mutex));
      goto failed_trace;
    }
  }
  return 0;
 failed_trace:
  luaX_checksync(pthread_cond_destroy(&g->trace_changed));
 failed_trace_mutex:
  luaX_checksync(pthread_mutex_destroy(&g->trace_mutex));
 failed_phase:
  luaX_checksync(pthread_cond_destroy(&g->phase_changed));
 failed_rendezvous:
  luaX_checksync(pthread_cond_destroy(&world->rendezvous));
 failed_resumed:
  luaX_checksync(pthread_cond_destroy(&world->resumed));
 failed_changed:
  luaX_checksync(pthread_cond_destroy(&world->changed));
  luaX_checksync(pthread_mutex_destroy(&world->mutex));
  return status;
}

void luaW_destroy (global_State *g) {
  LuaWorld *world = &g->world;
  int i;
  lua_assert(!luaW_stopped(world) && world->entry_waiting == 0 &&
             world->participants == NULL && world->rendezvous_cursor == NULL &&
             !luaW_heapheld(world) && world->waiters == NULL &&
             world->resume_waiters == NULL && world->rendezvous_waiters == NULL &&
             g->seals_pending == 0 &&
             !(luaW_control(world) & (LUA_WORLD_ARM_TRACE | LUA_WORLD_SEAL_NURSERIES)) &&
             g->phase_waiters == NULL && g->minor_waiters == NULL &&
             g->trace_waiters == NULL &&
             world->heap_head == NULL && world->reclaimers == NULL &&
             world->storage_bytes == 0 && world->storage_busy == 0 &&
             world->storage_workers == 0 && world->storage_scanners == 0 &&
             world->storage_scan_waiters == 0 &&
             world->storage_retired == NULL && world->storage_pending == NULL);
  for (i = 0; i < LUA_HASH_WAIT_SHARDS; i++)
    luaX_checksync(pthread_mutex_destroy(&g->hash_wait_mutex[i]));
  for (i = 0; i < LUA_PUBLISH_SHARDS; i++) {
    lua_assert(g->publishers[i].head == NULL);
    luaX_checksync(pthread_mutex_destroy(&g->publishers[i].mutex));
  }
  luaX_checksync(pthread_cond_destroy(&g->trace_changed));
  luaX_checksync(pthread_mutex_destroy(&g->trace_mutex));
  luaX_checksync(pthread_cond_destroy(&g->phase_changed));
  luaX_checksync(pthread_cond_destroy(&world->rendezvous));
  luaX_checksync(pthread_cond_destroy(&world->resumed));
  luaX_checksync(pthread_cond_destroy(&world->changed));
  luaX_checksync(pthread_mutex_destroy(&world->mutex));
}

void luaW_enroll (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  LuaParticipation *participant = &L->participation;
  lua_assert(participant->previous == NULL && luaW_active(participant) == 0);
  if ((__atomic_load_n(&L->stack_gate.word, __ATOMIC_RELAXED) &
       LUA_GATE_DORMANT) && luaW_stopped(world)) {
    world->entry_waiting++;
    luaW_waitresume(L);
    if (--world->entry_waiting == 0) notifyrendezvous(G(L), L);
  }
  /* A new coroutine has no outstanding allocation batch from before the
  ** arm request. Its creator accounts for the construction on its own gate. */
  participant->trace_armed = luaW_armepoch(world);
  participant->next = world->participants;
  participant->previous = &world->participants;
  if (participant->next != NULL)
    participant->next->previous = &participant->next;
  world->participants = participant;
}

void luaW_register (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  luaW_enroll(L);
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
  lua_assert(__atomic_load_n(&L->owned_operations, __ATOMIC_RELAXED) == 0 &&
             __atomic_load_n(&L->operation_roots, __ATOMIC_RELAXED) == 0 &&
             __atomic_load_n(&L->operation, __ATOMIC_RELAXED) == NULL);
  luaM_publishwithdrawn(L);
  unlinkpublisher(L);
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(luaW_active(participant) == 0 && participant->storage_scan == 0);
  if (participant->previous != NULL) {
    *participant->previous = participant->next;
    if (participant->next != NULL)
      participant->next->previous = participant->previous;
    participant->previous = NULL;
  }
  if (world->rendezvous_cursor == participant) {
    world->rendezvous_cursor = participant->next;
    notifyrendezvous(G(L), NULL);
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
    &L->admission->thread->stack_gate.word, LUA_GATE_MANAGED, __ATOMIC_SEQ_CST);
  lua_assert(luaX_gateowner(previous) != NULL &&
             !(previous & (LUA_GATE_ACTIVE | LUA_GATE_DORMANT)));
  UNUSED(previous);
}

/* Start the walk after publishing the request. An entrant whose gate was
** already passed must see the request before accessing Lua storage. During
** trace arming, only managed gates can owe heap publication. */
l_sinline int rendezvousready (LuaWorld *world, int arming) {
  LuaParticipation *part;
  unsigned active = arming ? LUA_GATE_MANAGED : LUA_GATE_ACTIVE;
  uint64_t epoch = arming ? luaW_armepoch(world) : 0;
  while ((part = world->rendezvous_cursor) != NULL) {
    if ((__atomic_load_n(&part->thread->stack_gate.word, __ATOMIC_SEQ_CST) &
         active) && (!arming || part->trace_armed != epoch)) return 0;
    /* A pre-arm owner settled its active debt when acknowledging, or
    ** sealed it in the withdrawn batch before releasing its admission. */
    if (arming) {
      luaX_joinvalues(&part->thread->stack_gate);
      luaM_publishwithdrawn(part->thread);
    }
    world->rendezvous_cursor = part->next;
  }
  return 1;
}

static void withdraw (lua_State *L) {
  uintptr_t previous;
  /* Every withdrawal here holds world.mutex and retains its stack gate.
  ** A scanner may borrow these stable roots under that mutex, including
  ** when the owner cannot resume until a collection finishes. */
  __atomic_store_n(&L->root_waiting, 1, __ATOMIC_SEQ_CST);
  if (L->stack_peer != NULL)
    __atomic_store_n(&L->stack_peer->root_waiting, 1, __ATOMIC_SEQ_CST);
  previous = __atomic_fetch_and(&L->admission->thread->stack_gate.word,
                                ~cast(uintptr_t, LUA_GATE_ACTIVE),
                                __ATOMIC_SEQ_CST);
  lua_assert((previous & LUA_GATE_ACTIVE) == LUA_GATE_MANAGED);
  UNUSED(previous);
}

/* The last acknowledgement or withdrawal publishes shared tracing work.
** Stops still have an owner; only a completed stop predicate needs a wake. */
static void notifyrendezvous (global_State *g, lua_State *L) {
  LuaWorld *world = &g->world;
  uint64_t control = luaW_control(world);
  if (control & LUA_WORLD_ARM_TRACE) {
    if (!rendezvousready(world, 1)) return;
    luaC_publishtraceplan(g);
    __atomic_fetch_and(&world->control, ~cast(uint64_t, LUA_WORLD_ARM_TRACE),
                       __ATOMIC_SEQ_CST);
    if (heapcount(world) == 0) grantheap(world, L);
    luaQ_wake(&g->phase_changed, &g->phase_waiters);
    luaSC_notifygc(g);
    return;
  }
  if (world->rendezvous_waiters == NULL) return;
  if (control & LUA_WORLD_STOPPED) {
    if (!rendezvousready(world, 0)) return;
  }
  else if (world->entry_waiting != 0) return;
  luaQ_wake(&world->rendezvous, &world->rendezvous_waiters);
}

static void acknowledge (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  if (luaW_control(world) & LUA_WORLD_ARM_TRACE) {
    uint64_t epoch = luaW_armepoch(world);
    if (L->admission->trace_armed == epoch) return;
    L->admission->trace_armed = epoch;
    luaM_publishdebt(L);
    notifyrendezvous(G(L), L);
  }
}

void luaW_acktrace (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  if (L->admission->trace_armed == luaW_armepoch(world)) return;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  acknowledge(L);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

void luaW_armtrace (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  lua_assert(luaW_ownsheap(L) && L->world_depth == 0);
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(world->rendezvous_cursor == NULL);
  world->rendezvous_cursor = world->participants;
  __atomic_store_n(&world->arm_epoch, luaC_traceepoch(G(L)), __ATOMIC_RELEASE);
  __atomic_fetch_or(&world->control, LUA_WORLD_ARM_TRACE, __ATOMIC_SEQ_CST);
  acknowledge(L);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

void luaW_deactivate (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  luaM_publishdebt(L);
  withdraw(L);
  if (luaC_waitingstacks(G(L))) {
    luaC_readystack(G(L), cast(uintptr_t, L));
    if (L->stack_peer != NULL)
      luaC_readystack(G(L), cast(uintptr_t, L->stack_peer));
  }
  notifyrendezvous(G(L), L);
  if (l_unlikely(luaW_control(world) & LUA_WORLD_SEAL_NURSERIES))
    luaQ_wake(&world->changed, &world->waiters);
}

void luaW_resume (lua_State *L) {
  /* Paired stack operations share an admission but retain both gates. */
  __atomic_store_n(&L->root_waiting, 0, __ATOMIC_RELAXED);
  if (L->stack_peer != NULL)
    __atomic_store_n(&L->stack_peer->root_waiting, 0, __ATOMIC_RELAXED);
  luaW_activate(L);
  acknowledge(L);
}

void luaW_waitresume (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  while (luaW_stopped(world)) {
    if (world->stop_slots & LUA_WORLD_RETAIN_MUTATORS)
      luaQ_waitcarrier(L, &world->mutex, &world->resumed, &world->resume_waiters);
    else
      luaQ_wait(L, &world->mutex, &world->resumed, &world->resume_waiters);
  }
}

static void __attribute__((noinline)) awaitentry (lua_State *L, LuaWorld *world) {
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  /* Only the atomic participation record can change before admission. In
  ** particular, a late entrant cannot change roots while a collector scans
  ** this state. Its embedding caller retains the target's ordinary Lua root. */
  withdraw(L);  /* an unadmitted entrant cannot publish the stopped record */
  if (luaW_stopped(world)) {
    world->entry_waiting++;
    notifyrendezvous(G(L), L);
    luaW_waitresume(L);
    luaW_resume(L);
    if (--world->entry_waiting == 0)
      notifyrendezvous(G(L), L);
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
  else {
    if (l_unlikely(control & LUA_WORLD_ARM_TRACE)) luaW_acktrace(L);
    if (l_unlikely(control & LUA_WORLD_SEAL_NURSERIES)) luaC_pollseal(L);
    if (luaC_tracing(L)) luaC_checksnapshot(L);
  }
}

int luaW_tryenter (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  if (luaW_stopped(world)) {
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
    return 0;
  }
  luaW_activate(L);
  acknowledge(L);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  luaHS_quiescent(L);
  luaC_checksnapshot(L);
  return 1;
}

void luaW_leave (global_State *g, uintptr_t stack, unsigned flags,
                 uint64_t control) {
  LuaWorld *world = &g->world;
  int ready = 0;
  /* The releasing gate has already withdrawn. Its state and participation
  ** record may be gone. Serialize notification with any stop request, which
  ** owns the trace lists after all participants leave. */
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  if ((control & LUA_WORLD_STACK_ROOTS) && !luaW_stopped(world))
    ready = luaC_readystack(g, stack);
  notifyrendezvous(g, NULL);
  if ((flags & LUA_GATE_WAITING) ||
      (control & LUA_WORLD_SEAL_NURSERIES) ||
      (!(flags & LUA_GATE_STACK) &&
       (luaW_control(world) & LUA_WORLD_STORAGE_WAIT)))
    luaQ_wake(&world->changed, &world->waiters);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  if (ready) luaSC_notifygc(g);
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
  lua_assert(!(luaW_control(world) & (LUA_WORLD_STOPPED | LUA_WORLD_ARM_TRACE)));
  /* Admit the batch notified by the previous stop before stopping again.
  ** Metadata ownership prevents another stop while this batch drains. */
  if (world->entry_waiting != 0) {
    L->native_depth++;
    luaW_deactivate(L);
    luaQ_wake(&world->changed, &world->waiters);
    do {
      luaQ_wait(L, &world->mutex, &world->rendezvous, &world->rendezvous_waiters);
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
    lua_assert(world->rendezvous_cursor == NULL);
    for (participant = world->participants; participant != NULL;
         participant = participant->next) {
      luaX_joinvalues(&participant->thread->stack_gate);
      if (luaW_active(participant)) {
        if (world->rendezvous_cursor == NULL && participant != L->admission)
          world->rendezvous_cursor = participant;
        active += !participant->collector;
      }
    }
    if (active > G(L)->pacer.parallelism) G(L)->pacer.parallelism = active;
  }
  luaQ_wake(&world->changed, &world->waiters);
  luaW_deactivate(L);
  while (!rendezvousready(world, 0)) {
    if (world->stop_slots & LUA_WORLD_RETAIN_COLLECTOR)
      luaQ_waitcarrier(L, &world->mutex, &world->rendezvous, &world->rendezvous_waiters);
    else
      luaQ_wait(L, &world->mutex, &world->rendezvous, &world->rendezvous_waiters);
  }
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
  world->stop_slots = 0;
  __atomic_fetch_and(&world->control, ~cast(uint64_t, LUA_WORLD_STOPPED),
                     __ATOMIC_SEQ_CST);
  /* A gate can release during a stop that does not consume marking work. */
  if (luaC_waitingstacks(G(L))) luaC_readystack(G(L), 0);
  luaQ_wake(&world->resumed, &world->resume_waiters);
  luaQ_wake(&world->changed, &world->waiters);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  luaSC_worldresumed(G(L));
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
  NativeStackContext *context = luaX_context(L);
  if (__atomic_load_n(&world->heap_owner.token, __ATOMIC_ACQUIRE) == context) {
    setheapcount(world, heapcount(world) + 1);
    return;
  }
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  if (heapcount(world) != 0 ||
      (luaW_control(world) & (LUA_WORLD_STOPPED | LUA_WORLD_ARM_TRACE))) {
    LuaHeapWaiter wait = {NULL, NULL, NULL, PTHREAD_COND_INITIALIZER, 0};
    lua_assert(L->world_depth == 0 &&
      luaW_active(L->admission) != 0);
    L->native_depth++;
    L->heap_waiting++;
    wait.owner = context;
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
    while (__atomic_load_n(&world->heap_owner.token, __ATOMIC_ACQUIRE) != context)
      luaQ_wait(L, &world->mutex, &wait.changed, &wait.waiters);
    /* Ownership is reserved directly for this recipient. Fresh arrivals
    ** cannot steal it while its saved stack waits for execution capacity. */
    lua_assert(heapcount(world) == 1 && !luaW_stopped(world));
    luaX_checksync(pthread_cond_destroy(&wait.changed));
    luaW_resume(L);
    luaC_checksnapshot(L);
    L->heap_waiting--;
    L->native_depth--;
  }
  else {
    setheapcount(world, 1);
    __atomic_store_n(&world->heap_owner.token, context, __ATOMIC_RELEASE);
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
  NativeStackContext *context = luaX_context(L);
  int acquired = 0;
  if (__atomic_load_n(&world->heap_owner.token, __ATOMIC_ACQUIRE) == context) {
    setheapcount(world, heapcount(world) + 1);
    return 1;
  }
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  if (heapcount(world) == 0 &&
      !(luaW_control(world) & (LUA_WORLD_STOPPED | LUA_WORLD_ARM_TRACE))) {
    setheapcount(world, 1);
    __atomic_store_n(&world->heap_owner.token, context, __ATOMIC_RELEASE);
    acquired = 1;
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  return acquired;
}

void luaW_heapleave (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  unsigned depth = heapcount(world);
  lua_assert(luaW_ownsheap(L) && depth != 0);
  if (depth > 1) {
    setheapcount(world, depth - 1);
    return;
  }
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  setheapcount(world, 0);
  grantheap(world, L);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

static unsigned heapdepth (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  return luaW_ownsheap(L) ? heapcount(world) : 0;
}

void luaW_waitcollector (lua_State *L, int reason) {
  global_State *g = G(L);
  LuaWorld *world = &g->world;
  uint64_t epoch = reason == LUA_WAIT_ARM ? luaC_traceepoch(g)
    : __atomic_load_n(&world->work_epoch, __ATOMIC_ACQUIRE);
  lua_assert(L->native_depth != 0);
  luaW_deactivate(L);
  if (reason == LUA_WAIT_RECLAIM || (luaW_control(world) & LUA_WORLD_STORAGE_WAIT))
    luaQ_wake(&world->changed, &world->waiters);
  if (reason == LUA_WAIT_TRACE) {
    /* Withdraw before taking the work lock, and recheck before parking.
    ** A later cycle cannot inherit this wait across the unlocked interval. */
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
    luaX_checksync(pthread_mutex_lock(&g->trace_mutex));
    if (luaC_scanready(g) &&
        __atomic_load_n(&world->work_epoch, __ATOMIC_ACQUIRE) == epoch &&
        luaC_traceblocked(g) &&
        !luaC_markready(g))
      luaQ_wait(L, &g->trace_mutex, &g->trace_changed, &g->trace_waiters);
    luaX_checksync(pthread_mutex_unlock(&g->trace_mutex));
    luaX_checksync(pthread_mutex_lock(&world->mutex));
  }
  else if (reason == LUA_WAIT_RECLAIM)
    luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
  else if (reason == LUA_WAIT_START) {
    while (__atomic_load_n(&g->gcstep_pending, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&world->work_epoch, __ATOMIC_ACQUIRE) == epoch)
      luaQ_wait(L, &world->mutex, &g->phase_changed, &g->phase_waiters);
  }
  else if (reason == LUA_WAIT_ARM) {
    while (luaC_traceepoch(g) == epoch && (luaC_markstate(g) & LUA_MARK_ARMING))
      luaQ_wait(L, &world->mutex, &g->phase_changed, &g->phase_waiters);
  }
  else if (!luaW_stopped(world))
    luaQ_wait(L, &world->mutex, &g->phase_changed,
      reason == LUA_WAIT_MINOR ? &g->minor_waiters : &g->phase_waiters);
  luaW_waitresume(L);
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
