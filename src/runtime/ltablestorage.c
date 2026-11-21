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

enum { SCAN_WAITING = 1, SCAN_READER = 2 };

static void notify (LuaWorld *world);

static void wakewaiters (LuaWorld *world) {
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  luaQ_wake(&world->changed, &world->waiters);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

static int after (uint64_t a, uint64_t b) {
  return cast(int64_t, a - b) > 0;
}

void luaHS_scanbegin (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  LuaParticipation *participant = L->admission;
  uint64_t epoch = luaW_epoch(luaW_control(world));
  lua_assert(participant->storage_scan == 0);
  __atomic_store_n(&participant->storage_scan, epoch | 1u, __ATOMIC_SEQ_CST);
  /* If a reclaiming batch advanced the epoch before seeing this record,
  ** acquire that publication before accessing a table. Keep the earlier
  ** epoch in the record as a conservative bound for later batches. */
  (void)luaW_control(world);
  __atomic_fetch_add(&world->storage_scanners, SCAN_READER, __ATOMIC_SEQ_CST);
}

void luaHS_scanend (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  LuaParticipation *participant = L->admission;
  lua_assert(participant->storage_scan != 0);
  __atomic_store_n(&participant->storage_scan, 0, __ATOMIC_SEQ_CST);
  if (__atomic_fetch_sub(&world->storage_scanners, SCAN_READER,
                          __ATOMIC_SEQ_CST) & SCAN_WAITING)
    wakewaiters(world);
}

int luaHS_scanning (lua_State *L) {
  /* Pair with scanner entry on this word. Either the writer observes an
  ** existing scanner, or a later scanner acquires the closed layout before
  ** sampling any pointers. A load alone would not publish that closure. */
  return __atomic_fetch_add(&G(L)->world.storage_scanners, 0,
                            __ATOMIC_SEQ_CST) >= SCAN_READER;
}

static void publish (LuaWorld *world, LuaTableRetirement *block) {
  LuaTableRetirement *head = __atomic_load_n(&world->storage_retired, __ATOMIC_RELAXED);
  do { block->next = head; }
  while (!__atomic_compare_exchange_n(&world->storage_retired, &head, block, 0,
                                      __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
}

void luaHS_publish (lua_State *L, LuaTableRetirement *block, int collector_only) {
  if (!completestate(G(L))) {
    luaM_free_(L, block->array, block->array_bytes);
    luaM_free_(L, block->nodes, block->node_bytes);
    luaM_free(L, block);
    return;
  }
  block->bytes = sizeof(*block) + block->array_bytes + block->node_bytes;
  block->collector_only = collector_only;
  block->next = L->storage_retired;
  if (L->storage_tail == NULL) {
    luaW_trackpublisher(L);
    L->storage_tail = block;
  }
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
  epoch = luaW_epoch(__atomic_fetch_add(&world->control, LUA_WORLD_EPOCH_STEP,
                                        __ATOMIC_SEQ_CST));
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

void luaHS_publishpending (lua_State *L) {
  flush(L, 0);
}

static void notify (LuaWorld *world) {
  if (luaW_control(world) & LUA_WORLD_STORAGE_WAIT) wakewaiters(world);
}

void luaHS_quiescent (lua_State *L) {
  LuaWorld *world = &G(L)->world;
  luaHS_announce(world, L->admission, luaW_epoch(luaW_control(world)));
}

void luaHS_report (LuaWorld *world, LuaParticipation *participant,
                    uint64_t epoch) {
  __atomic_store_n(&participant->storage_epoch, epoch, __ATOMIC_SEQ_CST);
  /* A waiter can register after the entrant sampled the epoch. Its predicate
  ** scan must see our publication, or this fresh check must wake it. */
  notify(world);
}

/* world.mutex keeps the directory alive. Shared layouts depend on all
** managed admissions. Private layouts depend on active scanner borrows. */
static uint64_t minimum (LuaWorld *world, int collector_only) {
  LuaParticipation *participant;
  uint64_t result = luaW_epoch(luaW_control(world));
  uint64_t grace = __atomic_load_n(&world->storage_grace, __ATOMIC_ACQUIRE);
  for (participant = world->participants; participant != NULL;
       participant = participant->next) {
    if (collector_only) {
      uint64_t scan = __atomic_load_n(&participant->storage_scan,
                                      __ATOMIC_SEQ_CST);
      uint64_t epoch = scan & ~cast(uint64_t, 1);
      if (scan != 0 && after(result, epoch)) result = epoch;
    }
    else {
      uintptr_t gate = __atomic_load_n(&participant->thread->stack_gate.word,
                                       __ATOMIC_SEQ_CST);
      if (gate & LUA_GATE_MANAGED) {
        uint64_t epoch = __atomic_load_n(&participant->storage_epoch,
                                         __ATOMIC_SEQ_CST);
        if (after(grace, epoch)) epoch = grace;
        if (after(result, epoch)) result = epoch;
      }
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
static void waitstorage (lua_State *L, const LuaTableRetirement *block) {
  LuaWorld *world = &G(L)->world;
  int collector_only = block != NULL && block->collector_only;
  int wait;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  if (collector_only) {
    if (world->storage_scan_waiters++ == 0)
      __atomic_fetch_or(&world->storage_scanners, SCAN_WAITING, __ATOMIC_SEQ_CST);
  }
  else if (world->storage_waiters++ == 0)
    __atomic_fetch_or(&world->control, LUA_WORLD_STORAGE_WAIT, __ATOMIC_SEQ_CST);
  luaW_deactivate(L);
  luaQ_wake(&world->changed, &world->waiters);
  if (block != NULL)
    wait = !after(minimum(world, collector_only), block->epoch);
  else {
    wait = __atomic_load_n(&world->storage_busy, __ATOMIC_SEQ_CST) ||
      __atomic_load_n(&world->storage_workers, __ATOMIC_SEQ_CST) ||
      (__atomic_load_n(&world->storage_retired, __ATOMIC_SEQ_CST) == NULL &&
       __atomic_load_n(&world->storage_pending, __ATOMIC_ACQUIRE) == NULL);
    wait = wait && __atomic_load_n(&world->storage_bytes, __ATOMIC_ACQUIRE) != 0;
  }
  if (wait) luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
  luaW_waitresume(L);
  luaW_resume(L);
  if (collector_only) {
    if (--world->storage_scan_waiters == 0)
      __atomic_fetch_and(&world->storage_scanners, ~cast_uint(SCAN_WAITING),
                         __ATOMIC_SEQ_CST);
  }
  else if (--world->storage_waiters == 0)
    __atomic_fetch_and(&world->control, ~cast(uint64_t, LUA_WORLD_STORAGE_WAIT),
                       __ATOMIC_SEQ_CST);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  luaC_checksnapshot(L);
}

static void destroywork (lua_State *L, LuaTableRetirement *head, int all) {
  LuaWorld *world = &G(L)->world;
  uint64_t safe[2];
  unsigned count = 0, checked = 0;
  while (head != NULL) {
    LuaTableRetirement *block = head;
    unsigned domain = cast_uint(block->collector_only);
    if (count++ % STORAGE_BATCH == 0) checked = 0;
    if (!(checked & (1u << domain))) {
      luaHS_quiescent(L);
      luaX_checksync(pthread_mutex_lock(&world->mutex));
      safe[domain] = minimum(world, block->collector_only);
      luaX_checksync(pthread_mutex_unlock(&world->mutex));
      checked |= 1u << domain;
    }
    if (after(safe[domain], block->epoch)) {
      size_t bytes = block->bytes;
      head = block->next;
      luaM_free_(L, block->array, block->array_bytes);
      luaM_free_(L, block->nodes, block->node_bytes);
      luaM_free(L, block);
      L->reclaim_pages = 1;  /* return newly empty pool pages at completion */
      __atomic_fetch_sub(&world->storage_bytes, bytes, __ATOMIC_RELAXED);
    }
    else if (all && canwait(L)) {
      waitstorage(L, block);
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
      waitstorage(L, NULL);
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
