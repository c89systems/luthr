/*
** $Id: lmem.c $
** Interface to Memory Manager
** See Copyright Notice in lua.h
*/

#define lmem_c
#define LUA_CORE
#define _GNU_SOURCE

#include "lprefix.h"


#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lmempage.h"
#include "lobject.h"
#include "lstate.h"



/*
** About the realloc function:
** void *frealloc (void *ud, void *ptr, size_t osize, size_t nsize);
** ('osize' is the old size, 'nsize' is the new size)
**
** - frealloc(ud, p, x, 0) frees the block 'p' and returns NULL.
** Particularly, frealloc(ud, NULL, 0, 0) does nothing,
** which is equivalent to free(NULL) in ISO C.
**
** - frealloc(ud, NULL, x, s) creates a new block of size 's'
** (no matter 'x'). Returns NULL if it cannot create the new block.
**
** - otherwise, frealloc(ud, b, x, y) reallocates the block 'b' from
** size 'x' to size 'y'. Returns NULL if it cannot reallocate the
** block to the new size.
*/


/*
** Macro to call the allocation function.
*/
#define callfrealloc(L,block,os,ns)    luaAL_realloc(L, block, os, ns)


/*
** When an allocation fails, it will try again after an emergency
** collection, except when it cannot run a collection.  The GC should
** not be called while the state is not fully built, as the collector
** is not yet fully initialized. Also, it should not be called when
** 'gcstopem' is true, because then the interpreter is in the middle of
** a collection step.
*/
#define cantryagain(g)	(completestate(g) && !g->gcstopem)




#define firsttry(L,block,os,ns)    callfrealloc(L, block, os, ns)

#define DEBT_BATCH 65536

static void adddebt (global_State *g, l_mem change) {
  __atomic_fetch_sub(&g->GCbytes, change, __ATOMIC_RELAXED);
  __atomic_fetch_sub(&g->GCused, change, __ATOMIC_RELAXED);
}

static void accountassist (lua_State *L, l_mem change) {
  LuaWorld *world = &G(L)->world;
  uint64_t epoch;
  lu_mem bytes;
  if (change >= 0 || L->reclaiming || !completestate(G(L)) ||
      (!luaC_hasreclaimers(L) && !luaC_tracing(L) && !luaC_sweeping(L)))
    return;
  epoch = __atomic_load_n(&world->work_epoch, __ATOMIC_ACQUIRE);
  if (L->assist_epoch != epoch) {
    L->assist_epoch = epoch;
    L->assist_debt = 0;
    L->trace_debt = 0;
  }
  bytes = cast(lu_mem, 0) - cast(lu_mem, change);
  L->assist_debt = bytes > cast(lu_mem, MAX_LMEM) - L->assist_debt
    ? cast(lu_mem, MAX_LMEM) : L->assist_debt + bytes;
  if (luaC_tracing(L) || luaC_sweeping(L)) {
    lu_mem work = cast(lu_mem, applygcparam(G(L), STEPMUL, cast(l_mem, bytes)));
    L->trace_debt = work > cast(lu_mem, MAX_LMEM) - L->trace_debt
      ? cast(lu_mem, MAX_LMEM) : L->trace_debt + work;
  }
}

static LuaAllocationDebt takewithdrawn (lua_State *L) {
  LuaAllocationDebt withdrawn;
  withdrawn.pending = __atomic_load_n(&L->withdrawn_debt, __ATOMIC_ACQUIRE);
  if (withdrawn.pending != 0)
    withdrawn.pending = __atomic_exchange_n(&L->withdrawn_debt, 0, __ATOMIC_ACQ_REL);
  return withdrawn;
}

void luaM_publishwithdrawn (lua_State *L) {
  LuaAllocationDebt withdrawn = takewithdrawn(L);
  if (withdrawn.batch.total != 0)
    __atomic_fetch_sub(&G(L)->GCbytes, withdrawn.batch.total, __ATOMIC_RELAXED);
  if (withdrawn.batch.shared != 0)
    __atomic_fetch_sub(&G(L)->GCused, withdrawn.batch.shared, __ATOMIC_RELAXED);
}

void luaM_flushdebt (lua_State *L) {
  LuaAllocationDebt withdrawn = takewithdrawn(L);
  l_mem debt = cast(l_mem, L->allocation_debt.batch.total) + withdrawn.batch.total;
  l_mem shared = cast(l_mem, L->allocation_debt.batch.shared) + withdrawn.batch.shared;
  l_mem reclaimed = L->reclaim_credit;
  accountassist(L, L->allocation_debt.batch.shared);
  L->allocation_debt.pending = 0;
  L->reclaim_credit = 0;
  if (debt != 0) __atomic_fetch_sub(&G(L)->GCbytes, debt, __ATOMIC_RELAXED);
  if (shared != 0) __atomic_fetch_sub(&G(L)->GCused, shared, __ATOMIC_RELAXED);
  if (reclaimed != 0) {
    __atomic_fetch_sub(&G(L)->GCretired, reclaimed, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&G(L)->GCbytes, reclaimed, __ATOMIC_RELAXED);
  }
}

void luaM_privatebytes (lua_State *L, l_mem change) {
  l_mem debt = L->allocation_debt.batch.shared + change;
  if (change == 0) return;
  /* Each transfer is bounded by the size of one task nursery. */
  lua_assert(debt >= INT32_MIN && debt <= INT32_MAX);
  L->allocation_debt.batch.shared = cast(int32_t, debt);
  if (debt >= DEBT_BATCH || debt <= -DEBT_BATCH)
    luaM_flushdebt(L);
}

void luaM_publishdebt (lua_State *L) {
  lua_State *peer = L->stack_peer;
  if (L->allocation_debt.pending != 0 || L->reclaim_credit != 0 ||
      __atomic_load_n(&L->withdrawn_debt, __ATOMIC_RELAXED) != 0)
    luaM_flushdebt(L);
  if (peer != NULL && (peer->allocation_debt.pending != 0 ||
      peer->reclaim_credit != 0 ||
      __atomic_load_n(&peer->withdrawn_debt, __ATOMIC_RELAXED) != 0))
    luaM_flushdebt(peer);
}

void luaM_withdrawdebt (lua_State *L) {
  LuaAllocationDebt previous, next, local = L->allocation_debt;
  if (L->reclaim_credit != 0 || L->stack_peer != NULL || L->task_owner == NULL) {
    luaM_publishdebt(L);
    return;
  }
  if (local.pending == 0) return;
  previous.pending = __atomic_load_n(&L->withdrawn_debt, __ATOMIC_RELAXED);
  do {
    next.batch.total = previous.batch.total + local.batch.total;
    next.batch.shared = previous.batch.shared + local.batch.shared;
    if (next.batch.total >= DEBT_BATCH || next.batch.total <= -DEBT_BATCH ||
        next.batch.shared >= DEBT_BATCH || next.batch.shared <= -DEBT_BATCH) {
      luaM_flushdebt(L);
      return;
    }
  } while (!__atomic_compare_exchange_n(&L->withdrawn_debt, &previous.pending,
             next.pending, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
  luaW_trackpublisher(L);
  L->allocation_debt.pending = 0;
  accountassist(L, local.batch.shared);
}

static void accountowned (lua_State *L, l_mem change, int exclusive) {
  if (exclusive || change >= DEBT_BATCH || change <= -DEBT_BATCH) {
    luaM_flushdebt(L);
    if (!exclusive) accountassist(L, change);
    adddebt(G(L), change);
  }
  else {
    int32_t total = L->allocation_debt.batch.total + cast(int32_t, change);
    int32_t shared = L->allocation_debt.batch.shared + cast(int32_t, change);
    L->allocation_debt.batch.total = total;
    L->allocation_debt.batch.shared = shared;
    if (total >= DEBT_BATCH || total <= -DEBT_BATCH ||
        shared >= DEBT_BATCH || shared <= -DEBT_BATCH)
      luaM_flushdebt(L);
  }
}

static void account (lua_State *L, l_mem change) {
  accountowned(L, change, !completestate(G(L)) || luaW_iscollector(L));
}

#define ALLOC_PAGE_MIN 1024
#define ALLOC_SPARE_RESIDENT (16u * 1024 * 1024)
#define ALLOC_SPARE_LIMIT (64u * 1024 * 1024)

l_sinline pthread_mutex_t *poolmutex (global_State *g, unsigned pool) {
  return pool < LUA_ALLOC_POOLS ? &g->allocation_mutex[pool]
      : &g->slot_allocation_mutex[pool - LUA_ALLOC_POOLS];
}

l_sinline LuaAllocPage **poolpages (global_State *g, unsigned pool) {
  return pool < LUA_ALLOC_POOLS ? &g->allocation_pages[pool]
      : &g->slot_allocation_pages[pool - LUA_ALLOC_POOLS];
}

l_sinline LuaAllocPage **poolavailable (global_State *g, unsigned pool) {
  return pool < LUA_ALLOC_POOLS ? &g->allocation_available[pool]
      : &g->slot_allocation_available[pool - LUA_ALLOC_POOLS];
}

l_sinline LuaAllocPage **cachepage (LuaAllocationCache *cache, unsigned pool) {
  if (pool < LUA_ALLOC_POOLS) return &cache->pages[pool];
  lua_assert(cache->slot_pages != NULL);
  return &cache->slot_pages->pages[pool - LUA_ALLOC_POOLS];
}

int luaM_initpools (lua_State *L) {
  global_State *g = G(L);
  int i, status;
  g->allocation_dirty = g->allocation_pending = NULL;
  g->allocation_retired = g->allocation_returning = NULL;
  memset(g->allocation_spare, 0, sizeof(g->allocation_spare));
  for (i = 0; i < LUA_ALLOC_BINS; i++)
    g->allocation_spare_tail[i] = &g->allocation_spare[i];
  g->allocation_spare_bytes = 0;
  g->allocation_return_epoch = 0;
  g->allocation_epoch = 0;
  g->allocation_busy = 0;
  for (i = 0; i < LUA_ALLOC_TOTAL_POOLS; i++) {
    *poolavailable(g, i) = NULL;
    *poolpages(g, i) = NULL;
    status = pthread_mutex_init(poolmutex(g, i), NULL);
    if (status != 0) {
      while (i-- > 0)
        luaX_checksync(pthread_mutex_destroy(poolmutex(g, i)));
      return status;
    }
  }
  return 0;
}

void luaM_destroypools (lua_State *L) {
  int i;
  lua_assert(G(L)->allocation_dirty == NULL &&
             G(L)->allocation_pending == NULL &&
             G(L)->allocation_retired == NULL &&
             G(L)->allocation_returning == NULL &&
             G(L)->allocation_busy == 0 && G(L)->allocation_spare_bytes == 0);
  for (i = 0; i < LUA_ALLOC_TOTAL_POOLS; i++) {
    lua_assert(*poolavailable(G(L), i) == NULL);
    lua_assert(*poolpages(G(L), i) == NULL);
    luaX_checksync(pthread_mutex_destroy(poolmutex(G(L), i)));
  }
}

void luaM_releasecache (lua_State *L, LuaAllocationCache *cache) {
  int i;
  int end = cache->slot_pages != NULL ? LUA_ALLOC_TOTAL_POOLS : LUA_ALLOC_POOLS;
  for (i = 0; i < end; i++) {
    LuaAllocPage *page;
    luaX_checksync(pthread_mutex_lock(poolmutex(G(L), i)));
    page = *cachepage(cache, i);
    if (page != NULL) {
      lua_assert(page->owner == cache);
      *cachepage(cache, i) = NULL;
      page->owner = NULL;
      luaMP_notify(G(L), page);
    }
    luaX_checksync(pthread_mutex_unlock(poolmutex(G(L), i)));
  }
}

static LuaAllocBlock *takeblock (LuaAllocPage *page) {
  LuaAllocBlock *head;
  if (page->local == NULL) luaMP_collectpage(page);
  head = page->local;
  if (head == NULL) return NULL;
  page->local = head->block.link.next;
  page->used++;
  __atomic_store_n(&head->block.page, cast(uintptr_t, page), __ATOMIC_RELAXED);
  return head;
}

static LuaAllocationCache *allocationcache (lua_State *L) {
  LuaExecution *execution = luaX_execution(L);
  if (execution != NULL) {
    LuaAllocationCache *cache = execution->allocation_cache;
    if (cache != NULL && cache->global == G(L)) return cache;
  }
  return &L->allocation;
}

static void offerpage (global_State *g, LuaAllocPage *page);

static void queuereturn (global_State *g, LuaAllocPage *page) {
  LuaAllocPage *head = __atomic_load_n(&g->allocation_retired, __ATOMIC_RELAXED);
  do { page->dirty_next = head; }
  while (!__atomic_compare_exchange_n(&g->allocation_retired, &head, page,
              0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

void luaM_releasepage (lua_State *L, LuaAllocPage *page, l_mem bytes) {
  global_State *g = G(L);
  unsigned pool = page->pool;
  luaX_checksync(pthread_mutex_lock(poolmutex(g, pool)));
  lua_assert(page->state == LUA_PAGE_SWEEPING && page->owner == NULL &&
             page->available_previous == NULL &&
             __atomic_load_n(&page->remote, __ATOMIC_RELAXED) == 0);
  page->used = 0;
  /* Drop table membership until the backing-return queue unlinks the page. */
  __atomic_store_n(&page->table_epoch, 0, __ATOMIC_RELAXED);
  page->state = LUA_PAGE_SWEPT;
  luaX_checksync(pthread_mutex_unlock(poolmutex(g, pool)));
  account(L, bytes);
  queuereturn(g, page);  /* last page access */
}

/* The notification pins each claimed page. Cache borrowing is nonblocking;
** busy owners keep their notices for a later finite snapshot. No private page
** or cache reference survives a safepoint or backing-allocator admission. */
static unsigned reclaimpagebatch (lua_State *L, uint64_t *epoch, int exclusive,
                                  int complete) {
  global_State *g = G(L);
  LuaAllocPage *pages[64], *pending;
  unsigned count = 0, i;
  luaX_checksync(pthread_mutex_lock(&g->world.mutex));
  if (*epoch == 0 && g->allocation_pending == NULL && g->allocation_busy == 0) {
    pending = __atomic_exchange_n(&g->allocation_dirty, NULL, __ATOMIC_ACQUIRE);
    __atomic_store_n(&g->allocation_pending, pending, __ATOMIC_RELEASE);
    if (pending != NULL) g->allocation_epoch++;
  }
  if (*epoch == 0) *epoch = g->allocation_epoch;
  pending = __atomic_load_n(&g->allocation_pending, __ATOMIC_RELAXED);
  if (*epoch == g->allocation_epoch) {
    while (pending != NULL && count < 64) {
      pages[count++] = pending;
      pending = pending->dirty_next;
    }
    __atomic_store_n(&g->allocation_pending, pending, __ATOMIC_RELEASE);
  }
  if (count != 0) g->allocation_busy++;
  luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
  for (i = 0; i < count; i++) {
    LuaAllocPage *page = pages[i];
    LuaCacheBorrow borrow;
    LuaAllocBlock *held;
    unsigned pool = page->pool;
    int empty;
    memset(&borrow, 0, sizeof(borrow));
    luaX_checksync(pthread_mutex_lock(poolmutex(g, pool)));
    if (page->state == LUA_PAGE_SWEEPING) {
      luaX_checksync(pthread_mutex_unlock(poolmutex(g, pool)));
      luaMP_publish(g, page);
      continue;
    }
    if (!complete && page->owner != NULL) {
      luaX_checksync(pthread_mutex_unlock(poolmutex(g, pool)));
      luaMP_publish(g, page);
      continue;
    }
    if (page->owner != NULL && !luaMC_borrow(L, page->owner, &borrow, exclusive)) {
      luaX_checksync(pthread_mutex_unlock(poolmutex(g, pool)));
      luaMP_publish(g, page);
      continue;
    }
    held = page->notified;
    luaMP_resetnotice(page);
    luaMP_collectpage(page);
    if (held != NULL) {
      lua_assert(page->used != 0);
      page->used--;
      held->block.link.next = page->local;
      page->local = held;
    }
    empty = page->used == 0;
    if ((empty || g->gcemergency) && page->owner != NULL) {
      lua_assert(*cachepage(page->owner, pool) == page);
      *cachepage(page->owner, pool) = NULL;
      page->owner = NULL;
    }
    if (empty) {
      if (page->available_previous != NULL) {
        *page->available_previous = page->available_next;
        if (page->available_next != NULL)
          page->available_next->available_previous = page->available_previous;
        page->available_previous = NULL;
      }
      page->state = LUA_PAGE_RETURNING;
    }
    else if (page->owner == NULL && page->local != NULL &&
             page->available_previous == NULL)
      offerpage(g, page);
    luaMC_return(&borrow);
    luaX_checksync(pthread_mutex_unlock(poolmutex(g, pool)));
    if (empty) queuereturn(g, page);
    luaMC_notify(&borrow);
  }
  if (count != 0) {
    luaX_checksync(pthread_mutex_lock(&g->world.mutex));
    g->allocation_busy--;
    luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
  }
  return count;
}

/* Only complete OS pages inside the allocation payload can be discarded.
** The page header and the host allocator's boundary metadata stay resident. */
static int adviseunused (LuaAllocPage *page) {
#if defined(__linux__) && defined(MADV_FREE)
  long physical = sysconf(_SC_PAGESIZE);
  uintptr_t first, end;
  int saved, result;
  if (physical <= 0) return 0;
  first = cast(uintptr_t, page) + LUA_ALLOC_PAGE_HEADER;
  first += (physical - first % physical) % physical;
  end = cast(uintptr_t, page) + page->bytes;
  end -= end % physical;
  if (end <= first || end - first < page->bytes / 2) return 0;
  saved = errno;
  result = madvise(cast(void *, first), end - first, MADV_FREE);
  errno = saved;
  return result == 0;
#else
  UNUSED(page);
  return 0;
#endif
}

/* The caller has already detached the page from every allocation and sweep
** cursor. A refill builds a fresh image even if the OS kept the old bytes. */
static int keepsparepage (global_State *g, LuaAllocPage *page) {
  size_t bytes;
  unsigned pool = page->pool;
  int discard;
  if (pool >= LUA_ALLOC_BINS || page->bytes != LUA_ALLOC_PAGE_MAX ||
      g->allocator.function != luaAL_builtin || g->gcemergency ||
      (g->gcstp & GCSTPCLS)) return 0;
  bytes = __atomic_load_n(&g->allocation_spare_bytes, __ATOMIC_RELAXED);
  do {
    if (bytes > ALLOC_SPARE_LIMIT - page->bytes) return 0;
  } while (!__atomic_compare_exchange_n(&g->allocation_spare_bytes, &bytes,
              bytes + page->bytes, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
  discard = bytes >= ALLOC_SPARE_RESIDENT;
  if (discard && !adviseunused(page)) {
    __atomic_fetch_sub(&g->allocation_spare_bytes, page->bytes, __ATOMIC_RELAXED);
    return 0;
  }
  luaX_checksync(pthread_mutex_lock(poolmutex(g, pool)));
  /* Supply recent pages first to keep the kernel out of allocation churn. */
  if (discard) {
    page->next = NULL;
    *g->allocation_spare_tail[pool] = page;
    g->allocation_spare_tail[pool] = &page->next;
  }
  else {
    page->next = g->allocation_spare[pool];
    if (page->next == NULL) g->allocation_spare_tail[pool] = &page->next;
    g->allocation_spare[pool] = page;
  }
  luaX_checksync(pthread_mutex_unlock(poolmutex(g, pool)));
  return 1;
}

void luaM_drainsparepages (lua_State *L) {
  global_State *g = G(L);
  unsigned pool;
  lua_assert(!completestate(g) || luaW_iscollector(L));
  for (pool = 0; pool < LUA_ALLOC_BINS; pool++) {
    LuaAllocPage *page = g->allocation_spare[pool];
    g->allocation_spare[pool] = NULL;
    g->allocation_spare_tail[pool] = &g->allocation_spare[pool];
    while (page != NULL) {
      LuaAllocPage *next = page->next;
      luaAL_builtin(NULL, page, page->bytes, 0);
      page = next;
    }
  }
  g->allocation_spare_bytes = 0;
}

static unsigned returnpagebatch (lua_State *L, uint64_t *epoch) {
  global_State *g = G(L);
  LuaAllocPage *pages[64];
  unsigned count = 0, i;
  int admitted;
  if (__atomic_load_n(&g->allocation_retired, __ATOMIC_ACQUIRE) == NULL &&
      __atomic_load_n(&g->allocation_returning, __ATOMIC_ACQUIRE) == NULL)
    return 0;
  /* Claim work only after backing admission. No empty-page batch is stranded
  ** on an inactive stack while emergency collection needs those bytes. */
  admitted = luaAL_enter(L);
  luaX_checksync(pthread_mutex_lock(&g->world.mutex));
  if (*epoch == 0 && g->allocation_returning == NULL) {
    LuaAllocPage *pending = __atomic_exchange_n(&g->allocation_retired, NULL,
                                                 __ATOMIC_ACQUIRE);
    __atomic_store_n(&g->allocation_returning, pending, __ATOMIC_RELEASE);
    if (g->allocation_returning != NULL) g->allocation_return_epoch++;
  }
  if (*epoch == 0) *epoch = g->allocation_return_epoch;
  if (*epoch == g->allocation_return_epoch) {
    while (g->allocation_returning != NULL && count < 64) {
      LuaAllocPage *page = g->allocation_returning;
      __atomic_store_n(&g->allocation_returning, page->dirty_next, __ATOMIC_RELEASE);
      pages[count++] = page;
    }
  }
  luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
  for (i = 0; i < count; i++) {
    LuaAllocPage *page = pages[i];
    unsigned pool = page->pool;
    int swept;
    luaX_checksync(pthread_mutex_lock(poolmutex(g, pool)));
    swept = page->state == LUA_PAGE_SWEPT;
    if (pool < LUA_ALLOC_BINS && g->sweep_plan.pages[pool] == page)
      g->sweep_plan.pages[pool] = page->next;
    *page->previous = page->next;
    if (page->next != NULL) page->next->previous = page->previous;
    luaX_checksync(pthread_mutex_unlock(poolmutex(g, pool)));
    /* Keep complete cohorts from large table allocations. Ordinary empty
    ** pools release their backing as before, including small-object churn. */
    if (!swept || !keepsparepage(g, page))
      callfrealloc(L, page, page->bytes, 0);
  }
  luaAL_leave(L, admitted);
  return count;
}

static void reclaimpages (lua_State *L, int complete) {
  global_State *g = G(L);
  LuaGCFrame frame;
  uint64_t epoch = 0, returning = 0;
  int exclusive = !completestate(g) || luaW_iscollector(L);
  luaC_entergc(L, &frame);
  if (exclusive && ((g->gcstp & GCSTPCLS) || g->gcemergency)) {
    LuaAllocPage *page;
    if (g->gcstp & GCSTPCLS) {
      LuaParticipation *participant;
      LuaAllocationCache *cache;
      luaM_drainsparepages(L);
      for (participant = g->world.participants; participant != NULL;
           participant = participant->next)
        luaM_releasecache(L, &participant->thread->allocation);
      for (cache = g->slot_caches; cache != NULL; cache = cache->next)
        luaM_releasecache(L, cache);
    }
    {
      unsigned pool;
      for (pool = 0; pool < LUA_ALLOC_TOTAL_POOLS; pool++)
        for (page = *poolpages(g, pool); page != NULL; page = page->next)
          if (page->state == LUA_PAGE_OPEN) luaMP_notify(g, page);
    }
  }
  for (;;) {
    unsigned count = reclaimpagebatch(L, &epoch, exclusive, complete);
    while (returnpagebatch(L, &returning) == 64 && complete) {
      if (!exclusive) { luaX_pollrequests(L); luaQ_poll(L); }
    }
    if (!complete || count == 0) {
      if (exclusive && complete &&
          __atomic_load_n(&g->allocation_dirty, __ATOMIC_ACQUIRE) != NULL) {
        epoch = returning = 0;
        continue;
      }
      break;
    }
    if (!exclusive) { luaX_pollrequests(L); luaQ_poll(L); }
    /* A separate finite backing-return snapshot can start after each completed
    ** page batch; producers of page notices never extend this invocation. */
    returning = 0;
  }
  luaC_leavegc(L, &frame);
}

void luaM_reclaimpages (lua_State *L) {
  reclaimpages(L, 1);
}

static void assistpages (lua_State *L) {
  global_State *g = G(L);
  if (L->reclaiming || luaC_ingc(L) || L->world_depth != 0 ||
      L->object_scopes != NULL || luaW_scope(L).heap != 0) return;
  if (__atomic_load_n(&g->allocation_dirty, __ATOMIC_ACQUIRE) != NULL ||
      __atomic_load_n(&g->allocation_pending, __ATOMIC_ACQUIRE) != NULL ||
      __atomic_load_n(&g->allocation_retired, __ATOMIC_ACQUIRE) != NULL ||
      __atomic_load_n(&g->allocation_returning, __ATOMIC_ACQUIRE) != NULL)
    reclaimpages(L, 0);
}

/* Rejoin before selecting/calling the backing allocator, but preserve the
** auxiliary library's unaccounted buffer storage
** and failure handling; it is not a collectable Lua allocation. */
void *luaM_reallocbuffer (lua_State *L, void *block, size_t osize, size_t nsize) {
  void *replacement;
  lua_lock(L);
  replacement = callfrealloc(L, block, osize, nsize);
  lua_unlock(L);
  return replacement;
}





/*
** {==================================================================
** Functions to allocate/deallocate arrays for the Parser
** ===================================================================
*/

/*
** Minimum size for arrays during parsing, to avoid overhead of
** reallocating to size 1, then 2, and then 4. All these arrays
** will be reallocated to exact sizes or erased when parsing ends.
*/
#define MINSIZEARRAY	4


void *luaM_growaux_ (lua_State *L, void *block, int nelems, int *psize,
                     unsigned size_elems, int limit, const char *what) {
  void *newblock;
  int size = *psize;
  if (nelems + 1 <= size)  /* does one extra element still fit? */
    return block;  /* nothing to be done */
  if (size >= limit / 2) {  /* cannot double it? */
    if (l_unlikely(size >= limit))  /* cannot grow even a little? */
      luaG_runerror(L, "too many %s (limit is %d)", what, limit);
    size = limit;  /* still have at least one free place */
  }
  else {
    size *= 2;
    if (size < MINSIZEARRAY)
      size = MINSIZEARRAY;  /* minimum size */
  }
  lua_assert(nelems + 1 <= size && size <= limit);
  /* 'limit' ensures that multiplication will not overflow */
  newblock = luaM_saferealloc_(L, block, cast_sizet(*psize) * size_elems,
                                         cast_sizet(size) * size_elems);
  *psize = size;  /* update only when everything else is OK */
  return newblock;
}


/*
** In prototypes, the size of the array is also its number of
** elements (to save memory). So, if it cannot shrink an array
** to its number of elements, the only option is to raise an
** error.
*/
void *luaM_shrinkvector_ (lua_State *L, void *block, int *size,
                          int final_n, unsigned size_elem) {
  void *newblock;
  size_t oldsize = cast_sizet(*size) * size_elem;
  size_t newsize = cast_sizet(final_n) * size_elem;
  lua_assert(newsize <= oldsize);
  newblock = luaM_saferealloc_(L, block, oldsize, newsize);
  *size = final_n;
  return newblock;
}

/* }================================================================== */


l_noret luaM_toobig (lua_State *L) {
  luaG_runerror(L, "memory allocation error: block too big");
}

l_sinline void returnpooled (lua_State *L, LuaAllocPage *page,
                             LuaAllocBlock *first, LuaAllocBlock *last,
                             unsigned count) {
  LuaAllocationCache *cache = allocationcache(L);
  /* The allocation consumer's lease protects its local list. Empty pages
  ** still notify reclamation so idle caches release their backing storage. */
  if ((page->pool < LUA_ALLOC_POOLS || cache->slot_pages != NULL) &&
      *cachepage(cache, page->pool) == page) {
    lua_assert(page->owner == cache && page->used >= count);
    page->used -= count;
    last->block.link.next = page->local;
    page->local = first;
    if (page->used == 0) luaMP_notify(G(L), page);
  }
  else if (count == 1) luaMP_return(G(L), first);
  else luaMP_returnbatch(G(L), page, first, last);
}

void luaM_freeobjectbatch (lua_State *L, LuaAllocPage *page,
                          LuaAllocBlock *first, LuaAllocBlock *last,
                          unsigned count, l_mem bytes) {
  lua_assert(count != 0 && bytes > 0);
  if (__atomic_load_n(&page->task_private, __ATOMIC_ACQUIRE)) {
    lua_assert(page->used >= count && page->task_bytes >= cast_sizet(bytes));
    page->used -= count;
    page->task_bytes -= cast_sizet(bytes);
    last->block.link.next = page->local;
    page->local = first;
  }
  else returnpooled(L, page, first, last, count);
  account(L, bytes);
}


/*
** Free memory
*/
void luaM_free_ (lua_State *L, void *block, size_t osize) {
  LuaAllocBlock *allocation;
  LuaAllocPage *page;
  uintptr_t header;
  int exclusive, retired;
  lua_assert((osize == 0) == (block == NULL));
  if (block == NULL) return;
  exclusive = !completestate(G(L)) || luaW_iscollector(L);
  allocation = cast(LuaAllocBlock *, block) - 1;
  header = luaMP_header(allocation);
  retired = (header & LUA_BLOCK_RETIRED) != 0;
  page = luaMP_pageof(header);
  if (page != NULL) {
    if (header & LUA_BLOCK_TABLE)
      __atomic_store_n(&allocation->block.page, cast(uintptr_t, page),
                       __ATOMIC_RELAXED);
    returnpooled(L, page, allocation, allocation, 1);
  }
  else {
    callfrealloc(L, allocation, osize + sizeof(LuaAllocBlock), 0);
  }
  if (retired) {
    /* Logical retirement already paid this storage's pacing credit. Physical
    ** return changes charged/pending bytes only, without a collector phase. */
    if (osize >= DEBT_BATCH) {
      luaM_flushdebt(L);
      __atomic_fetch_sub(&G(L)->GCretired, cast(l_mem, osize), __ATOMIC_RELAXED);
      __atomic_fetch_sub(&G(L)->GCbytes, cast(l_mem, osize), __ATOMIC_RELAXED);
    }
    else {
      L->reclaim_credit += cast(l_mem, osize);
      if (L->reclaim_credit >= DEBT_BATCH) luaM_flushdebt(L);
    }
  }
  else accountowned(L, cast(l_mem, osize), exclusive);
}

void luaM_creditretired (lua_State *L, l_mem bytes) {
  global_State *g = G(L);
  __atomic_fetch_add(&g->GCretired, bytes, __ATOMIC_RELAXED);
  __atomic_fetch_sub(&g->GCused, bytes, __ATOMIC_RELAXED);
  if (issweepphase(g) && !(g->gcstp & GCSTPCLS))
    __atomic_fetch_sub(&g->GCcyclelive, bytes, __ATOMIC_RELAXED);
}

void luaM_retire_ (lua_State *L, void *memory, size_t size) {
  global_State *g = G(L);
  LuaAllocBlock *block;
  LuaGCFrame *frame;
  uintptr_t header;
  if (memory == NULL) { lua_assert(size == 0); return; }
  lua_assert(!completestate(g) || luaW_iscollector(L) || luaC_sweeping(L));
  block = cast(LuaAllocBlock *, memory) - 1;
  header = luaMP_header(block);
  lua_assert(!(header & LUA_BLOCK_RETIRED));
  __atomic_store_n(&block->block.page,
    (header & ~cast(uintptr_t, LUA_BLOCK_FLAGS)) | LUA_BLOCK_RETIRED,
    __ATOMIC_RELAXED);
  frame = luaC_gcframe();
  if (frame != NULL && frame->global == g && frame->batch) {
    lua_assert(frame->retired_bytes <= MAX_LMEM - cast(l_mem, size));
    frame->retired_bytes += cast(l_mem, size);
  }
  else luaM_creditretired(L, cast(l_mem, size));
}


/*
** In case of allocation fail, this function will do an emergency
** collection to free some memory and then try the allocation again.
*/
static void *tryagain (lua_State *L, void *block,
                       size_t osize, size_t nsize) {
  global_State *g = G(L);
  int retry = 0;
  luaW_heapenter(L);
  if (cantryagain(g)) {
    luaC_fullgc(L, 1);  /* try to free some memory... */
    retry = 1;
  }
  luaW_heapleave(L);
  return retry ? callfrealloc(L, block, osize, nsize) : NULL;
}

static LuaAllocPage *availablepage (lua_State *L, unsigned pool) {
  global_State *g = G(L);
  LuaAllocationCache *cache = allocationcache(L);
  LuaAllocPage *page;
  /* The slow handoff uses a size-class mutex. Reoffering a page can then never
  ** create an ABA pop, and unlinking an empty offered page is constant work.
  ** No allocation, safepoint or public allocator call occurs under this lock. */
  luaX_checksync(pthread_mutex_lock(poolmutex(g, pool)));
  page = *poolavailable(g, pool);
  if (page != NULL) {
    *poolavailable(g, pool) = page->available_next;
    if (page->available_next != NULL)
      page->available_next->available_previous = poolavailable(g, pool);
    page->available_previous = NULL;
    lua_assert(page->owner == NULL && *cachepage(cache, pool) == NULL);
    page->owner = cache;
    *cachepage(cache, pool) = page;
  }
  luaX_checksync(pthread_mutex_unlock(poolmutex(g, pool)));
  return page;
}

static LuaAllocPage *takesparepage (global_State *g, unsigned pool) {
  LuaAllocPage *page;
  if (pool >= LUA_ALLOC_BINS ||
      __atomic_load_n(&g->allocation_spare_bytes, __ATOMIC_RELAXED) == 0)
    return NULL;
  luaX_checksync(pthread_mutex_lock(poolmutex(g, pool)));
  page = g->allocation_spare[pool];
  if (page != NULL) {
    g->allocation_spare[pool] = page->next;
    if (page->next == NULL)
      g->allocation_spare_tail[pool] = &g->allocation_spare[pool];
    __atomic_fetch_sub(&g->allocation_spare_bytes, page->bytes, __ATOMIC_RELAXED);
  }
  luaX_checksync(pthread_mutex_unlock(poolmutex(g, pool)));
  return page;
}

static void preparepage (LuaAllocPage *page,
                         unsigned blockbytes, unsigned pool, size_t bytes) {
  page->local = NULL;
  page->remote = 0;
  page->notified = NULL;
  page->blockbytes = blockbytes;
  page->pool = pool;
  page->available_next = NULL;
  page->available_previous = NULL;
  page->dirty_next = NULL;
  page->state = LUA_PAGE_OPEN;
  page->owner = NULL;
  page->used = 0;
  page->bytes = bytes;
  page->table_epoch = 0;
  page->trace_next = NULL;
  memset(page->trace_slots, 0, sizeof(page->trace_slots));
  page->trace_pending = 0;
  page->task_private = page->task_backing = page->task_dirty = 0;
  page->task_top = page->task_capacity = 0;
  page->task_mark = 0;
  page->task_bytes = 0;
  {
    unsigned stride = blockbytes + sizeof(LuaAllocBlock);
    page->trace_scale = (LUA_ALLOC_PAGE_MAX + stride - 1) / stride;
  }
}

static LuaAllocPage *installpage (lua_State *L, LuaAllocPage *page,
                                 unsigned blockbytes, unsigned pool,
                                 size_t bytes) {
  global_State *g = G(L);
  LuaAllocationCache *cache = allocationcache(L);
  /* Participation keeps the cache lease stable during initialization. */
  preparepage(page, blockbytes, pool, bytes);
  luaMP_initpage(L, page);
  luaX_checksync(pthread_mutex_lock(poolmutex(g, pool)));
  page->next = *poolpages(g, pool);
  page->previous = poolpages(g, pool);
  if (page->next != NULL) page->next->previous = &page->next;
  *poolpages(g, pool) = page;
  lua_assert(*cachepage(cache, pool) == NULL);
  page->owner = cache;
  *cachepage(cache, pool) = page;
  luaX_checksync(pthread_mutex_unlock(poolmutex(g, pool)));
  return page;
}

/* Caller owns the pool mutex. */
static void offerpage (global_State *g, LuaAllocPage *page) {
  LuaAllocPage **head = poolavailable(g, page->pool);
  lua_assert(page->owner == NULL && page->available_previous == NULL);
  page->available_next = *head;
  page->available_previous = head;
  if (*head != NULL) (*head)->available_previous = &page->available_next;
  *head = page;
}

/* Publish one mark for the live slots and install the page ahead of any
** frozen sweep cursor. Its slots join the next sweep cohort. */
void luaM_adopttaskpage (lua_State *L, LuaAllocPage *page, uint64_t epoch,
                         lu_byte mark) {
  global_State *g = G(L);
  unsigned pool = page->pool;
  unsigned slot;
  lua_assert(page->task_private && page->owner == NULL);
  /* Shared allocation uses a free list; virgin slots need it only if the
  ** page escapes. Unpublished pages can allocate these slots in order. */
  for (slot = page->task_top; slot < page->task_capacity; slot++) {
    LuaAllocBlock *block = luaMP_slotblock(page, slot);
    block->block.page = cast(uintptr_t, page);
    block->block.link.next = page->local;
    page->local = block;
  }
  page->table_epoch = page->used != 0 ? epoch + 1 : 0;
  page->task_mark = mark;
  __atomic_store_n(&page->task_private, 0, __ATOMIC_RELEASE);
  luaX_checksync(pthread_mutex_lock(poolmutex(g, pool)));
  page->next = *poolpages(g, pool);
  page->previous = poolpages(g, pool);
  if (page->next != NULL) page->next->previous = &page->next;
  *poolpages(g, pool) = page;
  if (page->used == 0) luaMP_notify(g, page);
  else if (page->local != NULL) offerpage(g, page);
  luaX_checksync(pthread_mutex_unlock(poolmutex(g, pool)));
}

/* The page has no external readers and has been unlinked from the task. */
void luaM_discardtaskpage (lua_State *L, LuaAllocPage *page) {
  size_t bytes = page->task_bytes;
  lua_assert(page->task_private);
  callfrealloc(L, page, page->bytes, 0);
  if (bytes != 0) account(L, cast(l_mem, bytes));
}

l_sinline int taskpageavailable (const LuaAllocPage *page) {
  return page != NULL &&
    (page->local != NULL || page->task_top < page->task_capacity);
}

void luaM_trimtaskpages (lua_State *L) {
  LuaAllocPage **link = &L->task_pages, *discard = NULL, *page;
  while ((page = *link) != NULL) {
    unsigned pool = page->pool;
    lua_assert(!page->task_dirty);
    if (page->used == 0 && page != L->task_alloc_pages[pool]) {
      *link = page->next;
      L->task_page_bytes -= page->bytes;
      page->next = discard;
      discard = page;
    }
    else {
      LuaAllocPage *current = L->task_alloc_pages[pool];
      if (taskpageavailable(page) && !taskpageavailable(current))
        L->task_alloc_pages[pool] = page;
      link = &page->next;
    }
  }
  while (discard != NULL) {
    page = discard;
    discard = page->next;
    luaM_discardtaskpage(L, page);
  }
}

void *luaM_trytasktable (lua_State *L, size_t size) {
  unsigned pool = luaMP_sizeclass(size);
  LuaAllocPage *page = L->task_alloc_pages[pool];
  LuaAllocBlock *block;
  unsigned slot;
  uint64_t bit;
  if (!taskpageavailable(page)) {
    size_t bytes = page == NULL ? LUA_ALLOC_PAGE_MAX / 8
                                 : page->bytes * 2;
    if (bytes > LUA_ALLOC_PAGE_MAX) bytes = LUA_ALLOC_PAGE_MAX;
    if (!luaC_taskpagefits(L, bytes)) {
      luaC_sealtask(L);
      return NULL;
    }
    page = cast(LuaAllocPage *, firsttry(L, NULL, 0, bytes));
    if (page == NULL) return NULL;
    /* Backing admission may have transferred the previous cohort. */
    if (!luaC_tasktableeligible(L, size) || !luaC_taskpagefits(L, bytes)) {
      callfrealloc(L, page, bytes, 0);
      return NULL;
    }
    preparepage(page, cast_uint(luaMP_binbytes(pool)), pool, bytes);
    page->task_private = 1;
    page->task_capacity = cast_uint((bytes - LUA_ALLOC_PAGE_HEADER -
      sizeof(LuaTaskPageBits)) / (sizeof(LuaAllocBlock) + page->blockbytes));
    memset(luaMP_taskbits(page), 0, sizeof(LuaTaskPageBits));
    page->next = L->task_pages;
    page->previous = NULL;
    L->task_pages = L->task_alloc_pages[pool] = page;
    L->task_page_bytes += bytes;
  }
  if (page->local != NULL) {
    block = page->local;
    page->local = block->block.link.next;
    slot = luaMP_slotindex(page, block);
  }
  else {
    slot = page->task_top++;
    block = luaMP_slotblock(page, slot);
  }
  page->used++;
  __atomic_store_n(&block->block.page, cast(uintptr_t, page) | LUA_BLOCK_TABLE,
                    __ATOMIC_RELAXED);
  bit = UINT64_C(1) << (slot % 64);
  lua_assert(!(page->trace_slots[slot / 64] & bit));
  page->trace_slots[slot / 64] |= bit;
  luaMP_taskbits(page)->young[slot / 64] |= bit;
  if (!page->task_dirty) {
    page->task_dirty = 1;
    page->dirty_next = L->task_young_pages;
    L->task_young_pages = page;
  }
  page->task_bytes += size;
  account(L, -cast(l_mem, size));
  return block + 1;
}

static void abandonpage (lua_State *L, unsigned pool) {
  global_State *g = G(L);
  LuaAllocationCache *cache = allocationcache(L);
  LuaAllocPage *page;
  luaX_checksync(pthread_mutex_lock(poolmutex(g, pool)));
  page = *cachepage(cache, pool);
  if (page != NULL) {
    lua_assert(page->owner == cache);
    *cachepage(cache, pool) = NULL;
    page->owner = NULL;
    /* A full page has no transferable capacity. Its first actual return
    ** supplies the notification; refills must not queue fruitless sweep work. */
    lua_assert(page->local == NULL);
  }
  luaX_checksync(pthread_mutex_unlock(poolmutex(g, pool)));
}

/* Metadata admission can lend this slot to another task. Keep the requested
** capacity in the cache record, rather than only on a parked caller's stack:
** otherwise each new borrower restarts a hot size class with a cold chunk. */
static size_t growcache (lua_State *L, LuaAllocationCache *cache, unsigned pool,
                         size_t minimum, size_t requested) {
  LuaExecution *execution = luaX_execution(L);
  unsigned char *growth;
  size_t bytes;
  /* Only a provided cache can be lent to another task during admission.
  ** State/task-owned caches keep this request's demand on its retained stack;
  ** carrying old parser demand into their later allocations strands storage. */
  if (execution == NULL || execution->allocation_cache != cache)
    return requested > minimum ? requested : minimum;
  if (cache->growth_epoch != luaC_traceepoch(cache->global)) {
    memset(cache->growth, 0, sizeof(cache->growth));
    if (cache->slot_pages != NULL)
      memset(cache->slot_pages->growth, 0, sizeof(cache->slot_pages->growth));
    cache->growth_epoch = luaC_traceepoch(cache->global);
  }
  growth = pool < LUA_ALLOC_POOLS ? &cache->growth[pool]
      : &cache->slot_pages->growth[pool - LUA_ALLOC_POOLS];
  bytes = minimum << *growth;
  if (bytes > LUA_ALLOC_PAGE_MAX) bytes = LUA_ALLOC_PAGE_MAX;
  while (bytes < requested && bytes < LUA_ALLOC_PAGE_MAX) {
    bytes *= 2;
    if (bytes > LUA_ALLOC_PAGE_MAX) bytes = LUA_ALLOC_PAGE_MAX;
    (*growth)++;
  }
  return bytes < minimum ? minimum : bytes;
}

/* Either an assist or backing admission can resume on a different slot.
** Keep its usable cache; abandon a full one before claiming another page. */
static LuaAllocPage *reusecache (lua_State *L, unsigned pool, size_t minimum,
                                size_t *bytes, uint64_t epoch) {
  LuaAllocPage *page = *cachepage(allocationcache(L), pool);
  if (luaC_traceepoch(G(L)) != epoch) *bytes = minimum;
  if (page != NULL) {
    size_t grown;
    if (page->local != NULL || luaMP_remote(page) != NULL) return page;
    grown = page->bytes <= LUA_ALLOC_PAGE_MAX / 2
      ? page->bytes * 2 : LUA_ALLOC_PAGE_MAX;
    if (grown > *bytes) *bytes = grown;
    abandonpage(L, pool);
  }
  return NULL;
}

static LuaAllocPage *refill (lua_State *L, unsigned blockbytes, unsigned pool,
                             int emergency) {
  global_State *g = G(L);
  LuaAllocPage *page = *cachepage(allocationcache(L), pool);
  uint64_t epoch = luaC_traceepoch(g);
  int admitted, fresh = 0;
  /* Cold size classes need only a small block supply. These are allocator
  ** chunks, not OS mappings: reserving 4 KiB per cold coroutine/class strands
  ** most of a small public heap. Always fit at least two complete blocks. */
  size_t minimum = LUA_ALLOC_PAGE_HEADER +
    2 * (sizeof(LuaAllocBlock) + blockbytes);
  size_t bytes;
  if (minimum < ALLOC_PAGE_MIN) minimum = ALLOC_PAGE_MIN;
  bytes = growcache(L, allocationcache(L), pool, minimum, page == NULL ? 0
    : page->bytes <= LUA_ALLOC_PAGE_MAX / 2 ? page->bytes * 2 : LUA_ALLOC_PAGE_MAX);
  if (page != NULL) abandonpage(L, pool);
  assistpages(L);  /* reuse abandoned capacity before growing backing storage */
  page = reusecache(L, pool, minimum, &bytes, epoch);
  if (page != NULL) return page;
  /* Copy the growth hint before metadata admission can collect the old page.
  ** Cold bins remain small; hot bins amortize serialized backing allocation. */
  page = availablepage(L, pool);
  if (page != NULL) {
    return page;
  }
  page = takesparepage(g, pool);
  if (page != NULL) return installpage(L, page, blockbytes, pool, page->bytes);
  admitted = luaAL_enter(L);
  /* Admission can park and borrow a different slot. Its cache may already
  ** have been refilled, so retain usable pages instead of overwriting them. */
  page = reusecache(L, pool, minimum, &bytes, epoch);
  if (page != NULL) goto ready;
  bytes = growcache(L, allocationcache(L), pool, minimum, bytes);
  page = availablepage(L, pool);
  if (page == NULL) {
    page = cast(LuaAllocPage *, firsttry(L, NULL, 0, bytes));
    if (page == NULL && bytes > minimum) {
      /* Growth is optional: a tight allocator can still supply a small page. */
      bytes = minimum;
      page = cast(LuaAllocPage *, firsttry(L, NULL, 0, bytes));
    }
    if (page == NULL && emergency) {
      int retry = 0;
      luaAL_leave(L, admitted);
      luaW_heapenter(L);
      if (cantryagain(g)) { luaC_fullgc(L, 1); retry = 1; }
      luaW_heapleave(L);
      admitted = luaAL_enter(L);
      page = reusecache(L, pool, minimum, &bytes, epoch);
      if (page != NULL) goto ready;
      page = availablepage(L, pool);
      if (page != NULL) goto ready;
      if (retry) page = cast(LuaAllocPage *, callfrealloc(L, NULL, 0, bytes));
    }
    fresh = page != NULL;
  }
 ready:
  luaAL_leave(L, admitted);
  return fresh ? installpage(L, page, blockbytes, pool, bytes) : page;
}

static void *allocatebacking (lua_State *L, size_t size, int tag,
                               int emergency) {
  LuaAllocBlock *block;
  size_t bytes;
  if (size > MAX_SIZE - sizeof(LuaAllocBlock)) return NULL;
  bytes = size + sizeof(LuaAllocBlock);
  block = cast(LuaAllocBlock *, firsttry(L, NULL, cast_sizet(tag), bytes));
  if (block == NULL && emergency)
    block = cast(LuaAllocBlock *, tryagain(L, NULL, cast_sizet(tag), bytes));
  if (block != NULL) {
    block->block.page = 0;
    account(L, -cast(l_mem, size));
  }
  return block != NULL ? block + 1 : NULL;
}

/* Persistent SDK records must not pin transient object pages. Retain normal
** accounting and retry behavior, while allocating their exact backing size. */
void *luaM_alloccontrol (lua_State *L, size_t size) {
  return allocatebacking(L, size, 0, 1);
}

void *luaM_tryalloc (lua_State *L, size_t size) {
  return allocatebacking(L, size, 0, 0);
}

static void *allocatepooled (lua_State *L, size_t size, unsigned blockbytes,
                             unsigned pool, int emergency) {
  LuaAllocPage *page = *cachepage(allocationcache(L), pool);
  LuaAllocBlock *block = page != NULL ? takeblock(page) : NULL;
  lua_assert(size <= blockbytes);
  if (block == NULL) {
    page = refill(L, blockbytes, pool, emergency);
    if (page == NULL) return NULL;
    block = takeblock(page);
    lua_assert(block != NULL);
  }
  account(L, -cast(l_mem, size));
  return block + 1;
}

static void *allocateblock (lua_State *L, size_t size, int tag,
                            unsigned poolbase, int emergency) {
  if (size <= LUA_ALLOC_MAX) {
    unsigned bin = luaMP_sizeclass(size);
    return allocatepooled(L, size, cast_uint(luaMP_binbytes(bin)),
                          bin + poolbase, emergency);
  }
  if (tag == LUA_TTHREAD && allocationcache(L)->slot_pages != NULL) {
    /* Coroutine records have one uniform layout. Unlike host-side creation,
    ** managed creation can amortize these pages across all tasks on a slot. */
    size_t width = sizeof(LuaAllocBlock);
    unsigned blockbytes = cast_uint(((sizeof(LX) + width - 1) / width) * width);
    void *block;
    lua_assert(size == sizeof(LX));
    block = allocatepooled(L, size, blockbytes, LUA_ALLOC_THREAD_POOL, 0);
    if (block != NULL) return block;
  }
  return allocatebacking(L, size, tag, emergency);
}


/*
** Generic allocation routine.
*/
void *luaM_realloc_ (lua_State *L, void *block, size_t osize, size_t nsize) {
  void *newblock;
  LuaAllocBlock *allocation;
  lua_assert((osize == 0) == (block == NULL));
  if (nsize == 0) {
    luaM_free_(L, block, osize);
    return NULL;
  }
  /* A growing table or buffer may bypass object creation entirely. The
  ** allocator cannot run the collector before the caller publishes it. */
  if (nsize > osize) L->gc_resize = 1;
  allocation = block != NULL ? cast(LuaAllocBlock *, block) - 1 : NULL;
  lua_assert(allocation == NULL || !luaMP_retired(allocation));
  if (allocation != NULL && luaMP_page(allocation) != NULL &&
      nsize <= luaMP_page(allocation)->blockbytes) {
    account(L, cast(l_mem, osize) - cast(l_mem, nsize));
    return block;
  }
  if (allocation != NULL && luaMP_page(allocation) == NULL &&
      nsize > luaMP_binbytes(LUA_ALLOC_BINS - 1)) {
    LuaAllocBlock *replacement;
    size_t oldbytes = osize + sizeof(LuaAllocBlock);
    size_t bytes;
    if (nsize > MAX_SIZE - sizeof(LuaAllocBlock)) return NULL;
    bytes = nsize + sizeof(LuaAllocBlock);
    replacement = cast(LuaAllocBlock *, firsttry(L, allocation, oldbytes, bytes));
    if (replacement == NULL)
      replacement = cast(LuaAllocBlock *, tryagain(L, allocation, oldbytes, bytes));
    if (replacement != NULL) {
      replacement->block.page = 0;
      account(L, cast(l_mem, osize) - cast(l_mem, nsize));
    }
    return replacement != NULL ? replacement + 1 : NULL;
  }
  newblock = allocateblock(L, nsize, 0, 0, 1);
  if (newblock == NULL) return NULL;
  if (block != NULL) {
    memcpy(newblock, block, osize < nsize ? osize : nsize);
    luaM_free_(L, block, osize);
  }
  return newblock;
}


void *luaM_saferealloc_ (lua_State *L, void *block, size_t osize,
                                                    size_t nsize) {
  void *newblock = luaM_realloc_(L, block, osize, nsize);
  if (l_unlikely(newblock == NULL && nsize > 0))  /* allocation failed? */
    luaM_error(L);
  return newblock;
}


void *luaM_malloc_ (lua_State *L, size_t size, int tag) {
  if (size == 0)
    return NULL;  /* that's all */
  else {
    void *block = allocateblock(L, size, tag, 0, 1);
    if (block == NULL) luaM_error(L);
    return block;
  }
}

/* Retained call frames must not pin large pages of short-lived table nodes
** or objects. Unlike luaM_malloc_, this reports failure for optional frames. */
void *luaM_allocframes (lua_State *L, size_t size) {
  return allocateblock(L, size, 0, LUA_ALLOC_BINS, 1);
}

void *luaM_allocstack (lua_State *L, size_t size, int emergency) {
  static const unsigned short sizes[LUA_ALLOC_STACK_CLASSES] =
    {768, 1152, 1664, 2560, 3840, 6144};
  if (size > LUA_ALLOC_MAX && size <= sizes[LUA_ALLOC_STACK_CLASSES - 1] &&
      allocationcache(L)->slot_pages != NULL) {
    unsigned i = 0;
    void *block;
    while (sizes[i] < size) i++;
    block = allocatepooled(L, size, sizes[i], LUA_ALLOC_STACK_POOL + i, 0);
    if (block != NULL) return block;
  }
  return allocateblock(L, size, 0, 0, emergency);
}
