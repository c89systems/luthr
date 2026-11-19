/* Table buffers outlive every optimistic read that could see them. See lua.h. */
#define LUA_CORE
#include "lprefix.h"
#include "ltablestorage.h"
#include "lgc.h"
#include "lmem.h"
#include "lstate.h"
#include "lworld.h"

#define STORAGE_BATCH 64
#define STORAGE_PRESSURE 65536

static void notify (LuaWorld *world);

static int after (uint64_t a, uint64_t b) {
  return cast(int64_t, a - b) > 0;
}

static void publish (LuaWorld *world, LuaTableRetirement *block) {
  LuaTableRetirement *head = __atomic_load_n(&world->storage_retired, __ATOMIC_RELAXED);
  do { block->next = head; }
  while (!__atomic_compare_exchange_n(&world->storage_retired, &head, block, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
}

void luaHS_publish (lua_State *L, LuaTableRetirement *block) {
  if (!completestate(G(L))) {
    luaM_free_(L, block->array, block->array_bytes);
    luaM_free_(L, block->nodes, block->node_bytes);
    luaM_free(L, block);
    return;
  }
  block->bytes = sizeof(*block) + block->array_bytes + block->node_bytes;
  block->next = L->storage_retired;
  if (L->storage_tail == NULL) L->storage_tail = block;
  L->storage_retired = block;
  L->storage_bytes += block->bytes;
}

static LuaTableRetirement *finishbatch (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  LuaTableRetirement *head = L->storage_retired, *block;
  uint64_t epoch;
  if (head == NULL) return NULL;
  /* The replacement's release-publication precedes this epoch. An entrant
  ** announcing a later epoch must observe the replacement. Older entrants
  ** remain registered until a safepoint or API withdrawal. */
  epoch = __atomic_fetch_add(&world->storage_epoch, 1, __ATOMIC_SEQ_CST);
  for (block = head; block != NULL; block = block->next) block->epoch = epoch;
  __atomic_fetch_add(&world->storage_bytes, L->storage_bytes, __ATOMIC_RELAXED);
  L->storage_retired = L->storage_tail = NULL;
  L->storage_bytes = 0;
  return head;
}

static void flush (lua_State *L, int wake) {
  LuaWorld *world = &G(L)->world;
  LuaTableRetirement *tail = L->storage_tail, *batch = finishbatch(L), *head;
  if (batch == NULL) return;
  head = __atomic_load_n(&world->storage_retired, __ATOMIC_RELAXED);
  do { tail->next = head; }
  while (!__atomic_compare_exchange_n(&world->storage_retired, &head,
    batch, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
  if (wake) notify(world);
}

void luaHS_publishstopped (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  LuaParticipation *participant;
  /* The caller holds world.mutex after every mutator has withdrawn. Publish
  ** parked owners' small batches before certifying this grace period, so an
  ** emergency collection can recover them without waiting for their return. */
  /* Late entrants may already have tentative atomic admissions, but cannot
  ** touch managed state until the stopped flag is cleared. */
  lua_assert(world->stopped && luaW_iscollector(L));
  for (participant = world->participants; participant != NULL;
       participant = participant->next)
    flush(participant->thread, 0);
}

static void notify (LuaWorld *world) {
  if (__atomic_load_n(&world->storage_waiting, __ATOMIC_SEQ_CST)) {
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    luaQ_wake(&world->changed, &world->waiters);
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
  }
}

void luaHS_quiescent (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  LuaParticipation *participant = L->admission;
  uint64_t epoch = __atomic_load_n(&world->storage_epoch, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(&participant->storage_epoch, __ATOMIC_RELAXED) != epoch) {
    __atomic_store_n(&participant->storage_epoch, epoch, __ATOMIC_SEQ_CST);
    notify(world);
  }
}

/* world.mutex keeps the directory alive; only atomic participation fields are
** observed. A completed global rendezvous also certifies earlier retirements. */
static uint64_t minimum (LuaWorld *world) {
  LuaParticipation *participant;
  uint64_t result = __atomic_load_n(&world->storage_epoch, __ATOMIC_SEQ_CST);
  uint64_t grace = __atomic_load_n(&world->storage_grace, __ATOMIC_ACQUIRE);
  for (participant = world->participants; participant != NULL;
       participant = participant->next) {
    if (luaW_active(participant)) {
      uint64_t epoch = __atomic_load_n(&participant->storage_epoch, __ATOMIC_SEQ_CST);
      if (after(grace, epoch)) epoch = grace;
      if (after(result, epoch)) result = epoch;
    }
  }
  return result;
}

/* Only list transfer is serialized. Helpers return disjoint private batches
** outside the queue's ownership, including across metadata allocation waits. */
static LuaTableRetirement *takework (LuaWorld *world, int all,
                                    LuaTableRetirement **extra, int *claimed) {
  LuaTableRetirement *head, *last;
  unsigned expected = 0, count;
  if (!__atomic_compare_exchange_n(&world->storage_busy, &expected, 1, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return NULL;
  *claimed = 1;
  head = __atomic_load_n(&world->storage_pending, __ATOMIC_RELAXED);
  if (all) {
    __atomic_store_n(&world->storage_pending, NULL, __ATOMIC_RELEASE);
    *extra = __atomic_exchange_n(&world->storage_retired, NULL, __ATOMIC_SEQ_CST);
  }
  else {
    if (head == NULL)
      head = __atomic_exchange_n(&world->storage_retired, NULL, __ATOMIC_SEQ_CST);
    last = head;
    for (count = 1; last != NULL && count < STORAGE_BATCH &&
         last->next != NULL; count++) last = last->next;
    __atomic_store_n(&world->storage_pending,
      last != NULL ? last->next : NULL, __ATOMIC_RELEASE);
    if (last != NULL) last->next = NULL;
  }
  if (head != NULL || *extra != NULL)
    __atomic_fetch_add(&world->storage_workers, 1, __ATOMIC_SEQ_CST);
  __atomic_store_n(&world->storage_busy, 0, __ATOMIC_SEQ_CST);
  notify(world);
  return head;
}

static int canwait (lua_State *L) {
  return L->world_depth == 0 && luaW_scope(L).heap == 0;
}

/* Registration precedes the final atomic predicate scan. A publisher either
** observes this waiter or its change is visible to that scan. */
static void waitstorage (lua_State *L, uint64_t needed, int reader) {
  LuaWorld *world = &G(L)->world;
  int wait;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  __atomic_fetch_add(&world->storage_waiting, 1, __ATOMIC_SEQ_CST);
  luaW_deactivate(L);
  luaQ_wake(&world->changed, &world->waiters);
  if (reader) wait = after(needed, minimum(world));
  else {
    wait = __atomic_load_n(&world->storage_busy, __ATOMIC_SEQ_CST) ||
      __atomic_load_n(&world->storage_workers, __ATOMIC_SEQ_CST) ||
      (__atomic_load_n(&world->storage_retired, __ATOMIC_SEQ_CST) == NULL &&
       __atomic_load_n(&world->storage_pending, __ATOMIC_ACQUIRE) == NULL);
    wait = wait && __atomic_load_n(&world->storage_bytes, __ATOMIC_ACQUIRE) != 0;
  }
  if (wait) luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
  while (world->stopped)
    luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
  luaW_resume(L);
  __atomic_fetch_sub(&world->storage_waiting, 1, __ATOMIC_SEQ_CST);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  luaC_checksnapshot(L);
}

static void destroywork (lua_State *L, LuaTableRetirement *head, int all) {
  LuaWorld *world = &G(L)->world;
  uint64_t safe = 0;
  unsigned count = 0;
  while (head != NULL) {
    LuaTableRetirement *block = head;
    if (count++ % STORAGE_BATCH == 0) {
      luaHS_quiescent(L);
      luaX_checksync(pthread_mutex_lock(&world->mutex));
      safe = minimum(world);
      luaX_checksync(pthread_mutex_unlock(&world->mutex));
    }
    if (after(safe, block->epoch)) {
      size_t bytes = block->bytes;
      head = block->next;
      luaM_free_(L, block->array, block->array_bytes);
      luaM_free_(L, block->nodes, block->node_bytes);
      luaM_free(L, block);
      L->reclaim_pages = 1;  /* return newly empty pool pages at completion */
      __atomic_fetch_sub(&world->storage_bytes, bytes, __ATOMIC_RELAXED);
    }
    else if (all && canwait(L)) {
      waitstorage(L, block->epoch + 1, 1);
      count = 0;  /* refresh the grace-period view before retrying */
    }
    else {
      head = block->next;
      publish(world, block);
    }
  }
}

void luaHS_reclaim (lua_State *L, int mode) {
  LuaWorld *world = &G(L)->world;
  size_t bytes = __atomic_load_n(&world->storage_bytes, __ATOMIC_RELAXED);
  int all = mode == LUAHS_DRAIN;
  /* A producer owns its pressure batch through the grace period and frees its
  ** own buffers. Other executions neither steal that batch nor inherit its
  ** waiting obligation. An API withdrawal publishes smaller unfinished work. */
  if (!all && L->storage_retired != NULL &&
      (mode == LUAHS_WITHDRAW ||
       L->storage_bytes > STORAGE_PRESSURE || bytes > STORAGE_PRESSURE)) {
    int pressure = L->storage_bytes > STORAGE_PRESSURE || bytes > STORAGE_PRESSURE;
    LuaTableRetirement *batch = finishbatch(L);
    L->native_depth++;
    __atomic_fetch_add(&world->storage_workers, 1, __ATOMIC_SEQ_CST);
    destroywork(L, batch, pressure);
    __atomic_fetch_sub(&world->storage_workers, 1, __ATOMIC_SEQ_CST);
    notify(world);
    L->native_depth--;
  }
  else if (mode != LUAHS_ASSIST) flush(L, 1);
  if (mode != LUAHS_ASSIST || bytes > STORAGE_PRESSURE)
    bytes = __atomic_load_n(&world->storage_bytes, __ATOMIC_RELAXED);
  /* Amortize grace-period scans across a bounded amount of retired storage.
  ** Scanning the participant directory at every VM poll serializes otherwise
  ** independent writers. Explicit collection still drains small batches. */
  if (bytes == 0 || (!all && bytes <= STORAGE_PRESSURE)) return;
  L->native_depth++;
  for (;;) {
    LuaTableRetirement *extra = NULL, *head;
    int claimed = 0;
    luaHS_quiescent(L);
    head = takework(world, all, &extra, &claimed);
    if (all && !claimed && canwait(L)) {
      waitstorage(L, 0, 0);
      continue;
    }
    if (head != NULL || extra != NULL) {
      destroywork(L, head, all);
      destroywork(L, extra, all);
      __atomic_fetch_sub(&world->storage_workers, 1, __ATOMIC_SEQ_CST);
      notify(world);
    }
    /* Full collection drains a finite published snapshot. New producers do
    ** not extend it forever. Ordinary pressure is bounded by in-flight resizes. */
    break;
  }
  L->native_depth--;
}
