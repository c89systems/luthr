/*
** $Id: lmem.c $
** Interface to Memory Manager
** See Copyright Notice in lua.h
*/

#define lmem_c
#define LUA_CORE

#include "lprefix.h"


#include <stddef.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lmempage.h"
#include "lobject.h"
#include "lstate.h"
#include "ltask.h"



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
#define callfrealloc(g,block,os,ns)    ((*g->frealloc)(g->ud, block, os, ns))


/*
** When an allocation fails, it will try again after an emergency
** collection, except when it cannot run a collection.  The GC should
** not be called while the state is not fully built, as the collector
** is not yet fully initialized. Also, it should not be called when
** 'gcstopem' is true, because then the interpreter is in the middle of
** a collection step.
*/
#define cantryagain(g)	(completestate(g) && !g->gcstopem)




#if defined(EMERGENCYGCTESTS)
/*
** First allocation will fail except when freeing a block (frees never
** fail) and when it cannot try again; this fail will trigger 'tryagain'
** and a full GC cycle at every allocation.
*/
static void *firsttry (global_State *g, void *block, size_t os, size_t ns) {
  if (ns > 0 && cantryagain(g))
    return NULL;  /* fail */
  else  /* normal allocation */
    return callfrealloc(g, block, os, ns);
}
#else
#define firsttry(g,block,os,ns)    callfrealloc(g, block, os, ns)
#endif

#define DEBT_BATCH 65536

static void adddebt (global_State *g, l_mem change, int exclusive) {
  __atomic_fetch_sub(&g->GCbytes, change, __ATOMIC_RELAXED);
  if (__atomic_load_n(&g->trace_active, __ATOMIC_ACQUIRE))
    __atomic_fetch_add(&g->GCmarkdebt, change, __ATOMIC_RELAXED);
  if (exclusive) {
    /* Match the word-sized arithmetic of mutator accumulation. */
    lu_mem debt = cast(lu_mem, __atomic_load_n(&g->GCdebt, __ATOMIC_RELAXED));
    __atomic_store_n(&g->GCdebt, cast(l_mem, debt + cast(lu_mem, change)),
                     __ATOMIC_RELAXED);
  }
  else
    __atomic_fetch_add(&g->GCdebt, change, __ATOMIC_RELAXED);
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

void luaM_flushdebt (lua_State *actor, lua_State *L) {
  l_mem debt = L->allocation_debt;
  l_mem reclaimed = L->reclaim_credit;
  L->allocation_debt = 0;
  L->reclaim_credit = 0;
  accountassist(L, debt);
  if (debt != 0)
    adddebt(G(L), debt, !completestate(G(L)) || luaW_iscollector(actor));
  if (reclaimed != 0) {
    __atomic_fetch_sub(&G(L)->GCretired, reclaimed, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&G(L)->GCbytes, reclaimed, __ATOMIC_RELAXED);
  }
}

static void accountowned (lua_State *L, l_mem change, int exclusive) {
  if (exclusive || change >= DEBT_BATCH || change <= -DEBT_BATCH) {
    luaM_flushdebt(L, L);
    if (!exclusive) accountassist(L, change);
    adddebt(G(L), change, exclusive);
  }
  else {
    L->allocation_debt += change;
    if (L->allocation_debt >= DEBT_BATCH || L->allocation_debt <= -DEBT_BATCH)
      luaM_flushdebt(L, L);
  }
}

static void account (lua_State *L, l_mem change) {
  accountowned(L, change, !completestate(G(L)) || luaW_iscollector(L));
}

#define ALLOC_PAGE_MIN 1024
#define ALLOC_PAGE_MAX 65536
#define ALLOC_PAGE_RESERVE 3

int luaM_initpools (lua_State *L) {
  global_State *g = G(L);
  int i, status;
  g->allocation_pages = g->allocation_dirty = g->allocation_pending = NULL;
  g->allocation_retired = g->allocation_returning = NULL;
  g->allocation_return_epoch = 0;
  g->allocation_epoch = 0;
  g->allocation_busy = g->allocation_waiters = 0;
  for (i = 0; i < LUA_ALLOC_POOLS; i++) {
    g->allocation_available[i] = NULL;
    g->allocation_reserving[i] = 0;
    status = pthread_mutex_init(&g->allocation_mutex[i], NULL);
    if (status != 0) {
      while (i-- > 0)
        luaX_checksync(pthread_mutex_destroy(&g->allocation_mutex[i]));
      return status;
    }
  }
  status = pthread_cond_init(&g->allocation_changed, NULL);
  if (status != 0) {
    for (i = 0; i < LUA_ALLOC_POOLS; i++)
      luaX_checksync(pthread_mutex_destroy(&g->allocation_mutex[i]));
    return status;
  }
  return 0;
}

void luaM_destroypools (lua_State *L) {
  int i;
  lua_assert(G(L)->allocation_pages == NULL &&
             G(L)->allocation_dirty == NULL &&
             G(L)->allocation_pending == NULL &&
             G(L)->allocation_retired == NULL &&
             G(L)->allocation_returning == NULL &&
             G(L)->allocation_busy == 0 && G(L)->allocation_waiters == 0);
  luaX_checksync(pthread_cond_destroy(&G(L)->allocation_changed));
  for (i = 0; i < LUA_ALLOC_POOLS; i++) {
    lua_assert(G(L)->allocation_available[i] == NULL);
    luaX_checksync(pthread_mutex_destroy(&G(L)->allocation_mutex[i]));
  }
}

void luaM_releasecache (lua_State *L, LuaAllocationCache *cache) {
  int i;
  for (i = 0; i < LUA_ALLOC_POOLS; i++) {
    LuaAllocPage *page;
    luaX_checksync(pthread_mutex_lock(&G(L)->allocation_mutex[i]));
    page = cache->pages[i];
    if (page != NULL) {
      lua_assert(page->owner == cache);
      cache->pages[i] = NULL;
      page->owner = NULL;
      luaMP_notify(G(L), page);
    }
    luaX_checksync(pthread_mutex_unlock(&G(L)->allocation_mutex[i]));
  }
}

static LuaAllocBlock *takeblock (LuaAllocPage *page) {
  LuaAllocBlock *head;
  if (page->local == NULL) luaMP_collectpage(page);
  head = page->local;
  if (head == NULL) return NULL;
  page->local = head->block.link.next;
  page->used++;
  head->block.page = cast(uintptr_t, page);  /* clear the prior retire credit */
  return head;
}

static LuaAllocationCache *allocationcache (lua_State *L) {
  LuaExecution *execution = luaX_execution(L);
  if (execution != NULL) {
    LuaTaskRoot *task;
    if (execution->allocation_cache != NULL &&
        execution->allocation_cache->global == G(L) &&
        (execution->allocation_cache->permission != NULL ||
         execution->memory_cache == execution->allocation_cache))
      return execution->allocation_cache;
    task = __atomic_load_n(&execution->task, __ATOMIC_ACQUIRE);
    if (task != NULL && task->global == G(L) &&
        execution->memory_cache == &task->allocation)
      return &task->allocation;
  }
  return &L->allocation;
}

static void offerpage (global_State *g, LuaAllocPage *page);

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
    luaX_checksync(pthread_mutex_lock(&g->allocation_mutex[pool]));
    if (!complete && page->owner != NULL) {
      luaX_checksync(pthread_mutex_unlock(&g->allocation_mutex[pool]));
      luaMP_publish(g, page);
      continue;
    }
    if (page->owner != NULL && !luaMC_borrow(L, page->owner, &borrow, exclusive)) {
      luaMC_return(&borrow);
      luaX_checksync(pthread_mutex_unlock(&g->allocation_mutex[pool]));
      luaMP_publish(g, page);
      luaMC_notify(&borrow);
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
      lua_assert(page->owner->pages[pool] == page);
      page->owner->pages[pool] = NULL;
      page->owner = NULL;
    }
    if (empty) {
      if (page->available_previous != NULL) {
        *page->available_previous = page->available_next;
        if (page->available_next != NULL)
          page->available_next->available_previous = page->available_previous;
        page->available_previous = NULL;
      }
      page->retiring = 1;
    }
    else if (page->owner == NULL && page->local != NULL &&
             page->available_previous == NULL)
      offerpage(g, page);
    luaMC_return(&borrow);
    luaX_checksync(pthread_mutex_unlock(&g->allocation_mutex[pool]));
    if (empty) {
      LuaAllocPage *head = __atomic_load_n(&g->allocation_retired, __ATOMIC_RELAXED);
      do { page->dirty_next = head; }
      while (!__atomic_compare_exchange_n(&g->allocation_retired, &head, page,
                  0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    }
    luaMC_notify(&borrow);
  }
  if (count != 0) {
    luaX_checksync(pthread_mutex_lock(&g->world.mutex));
    g->allocation_busy--;
    luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
  }
  return count;
}

static unsigned returnpagebatch (lua_State *L, uint64_t *epoch) {
  global_State *g = G(L);
  LuaAllocPage *pages[64];
  unsigned count = 0, i;
  if (__atomic_load_n(&g->allocation_retired, __ATOMIC_ACQUIRE) == NULL &&
      __atomic_load_n(&g->allocation_returning, __ATOMIC_ACQUIRE) == NULL)
    return 0;
  luaW_heapenter(L);
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
      *page->previous = page->next;
      if (page->next != NULL) page->next->previous = page->previous;
      pages[count++] = page;
    }
  }
  for (i = 0; i < count; i++) {
    LuaAllocPage *page = pages[i];
    callfrealloc(g, page, page->bytes, 0);
  }
  luaW_heapleave(L);
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
      for (participant = g->world.participants; participant != NULL;
           participant = participant->next)
        luaM_releasecache(L, &participant->thread->allocation);
      for (cache = g->slot_caches; cache != NULL; cache = cache->next)
        luaM_releasecache(L, cache);
    }
    for (page = g->allocation_pages; page != NULL; page = page->next)
      if (!page->retiring) luaMP_notify(g, page);
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

/* Native buffer work is detached. Rejoin before selecting/calling its
** allocator, but preserve the auxiliary library's unaccounted buffer storage
** and failure handling; it is not a collectable Lua allocation. */
void *luaM_reallocbuffer (lua_State *L, void *block, size_t osize, size_t nsize) {
  void *replacement;
  lua_lock(L);
  luaW_heapenter(L);
  replacement = callfrealloc(G(L), block, osize, nsize);
  luaW_heapleave(L);
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


/*
** Free memory
*/
void luaM_free_ (lua_State *L, void *block, size_t osize) {
  LuaAllocBlock *allocation;
  int exclusive, retired;
  lua_assert((osize == 0) == (block == NULL));
  if (block == NULL) return;
  exclusive = !completestate(G(L)) || luaW_iscollector(L);
  allocation = cast(LuaAllocBlock *, block) - 1;
  retired = luaMP_retired(allocation);
  if (luaMP_page(allocation) != NULL)
    luaMP_return(G(L), allocation);
  else {
    luaW_heapenter(L);
    callfrealloc(G(L), allocation, osize + sizeof(LuaAllocBlock), 0);
    luaW_heapleave(L);
  }
  if (retired) {
    /* Logical retirement already paid this storage's pacing credit. Physical
    ** return changes charged/pending bytes only, without a collector phase. */
    if (osize >= DEBT_BATCH) {
      luaM_flushdebt(L, L);
      __atomic_fetch_sub(&G(L)->GCretired, cast(l_mem, osize), __ATOMIC_RELAXED);
      __atomic_fetch_sub(&G(L)->GCbytes, cast(l_mem, osize), __ATOMIC_RELAXED);
    }
    else {
      L->reclaim_credit += cast(l_mem, osize);
      if (L->reclaim_credit >= DEBT_BATCH) luaM_flushdebt(L, L);
    }
  }
  else accountowned(L, cast(l_mem, osize), exclusive);
}

void luaM_creditretired (lua_State *L, l_mem bytes) {
  global_State *g = G(L);
  __atomic_fetch_add(&g->GCretired, bytes, __ATOMIC_RELAXED);
  __atomic_fetch_add(&g->GCdebt, bytes, __ATOMIC_RELAXED);
  if (issweepphase(g) && !(g->gcstp & GCSTPCLS))
    __atomic_fetch_sub(&g->GCcyclelive, bytes, __ATOMIC_RELAXED);
}

void luaM_retire_ (lua_State *L, void *memory, size_t size) {
  global_State *g = G(L);
  LuaAllocBlock *block;
  LuaGCFrame *frame;
  if (memory == NULL) { lua_assert(size == 0); return; }
  lua_assert(!completestate(g) || luaW_iscollector(L) || luaC_sweeping(L));
  block = cast(LuaAllocBlock *, memory) - 1;
  lua_assert(!luaMP_retired(block));
  block->block.page |= 1u;
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
  if (cantryagain(g)) {
    luaC_fullgc(L, 1);  /* try to free some memory... */
    return callfrealloc(g, block, osize, nsize);  /* try again */
  }
  else return NULL;  /* cannot run an emergency collection */
}

static LuaAllocPage *availablepage (lua_State *L, unsigned pool) {
  global_State *g = G(L);
  LuaAllocationCache *cache = allocationcache(L);
  LuaAllocPage *page;
  /* The slow handoff uses a size-class mutex. Reoffering a page can then never
  ** create an ABA pop, and unlinking an empty offered page is constant work.
  ** No allocation, safepoint or public allocator call occurs under this lock. */
  luaX_checksync(pthread_mutex_lock(&g->allocation_mutex[pool]));
  page = g->allocation_available[pool];
  if (page != NULL) {
    g->allocation_available[pool] = page->available_next;
    if (page->available_next != NULL)
      page->available_next->available_previous = &g->allocation_available[pool];
    page->available_previous = NULL;
    lua_assert(page->owner == NULL && cache->pages[pool] == NULL);
    page->owner = cache;
    cache->pages[pool] = page;
  }
  luaX_checksync(pthread_mutex_unlock(&g->allocation_mutex[pool]));
  return page;
}

static void preparepage (global_State *g, LuaAllocPage *page,
                         unsigned bin, unsigned pool, size_t bytes) {
  page->local = NULL;
  page->remote = 0;
  page->notified = NULL;
  page->bin = bin;
  page->pool = pool;
  page->available_next = NULL;
  page->available_previous = NULL;
  page->dirty_next = NULL;
  page->retiring = 0;
  page->owner = NULL;
  page->used = 0;
  page->bytes = bytes;
  page->next = g->allocation_pages;
  page->previous = &g->allocation_pages;
  if (page->next != NULL) page->next->previous = &page->next;
  g->allocation_pages = page;
}

/* Amortize backing admission for a parallel team. Refill reaches this point
** only after finding its pool empty under metadata ownership. At most three
** spare pages remain per pool, independent of task count. Reserves are optional:
** failure stops the batch without collection or failing the requested block. */
static unsigned reservepages (lua_State *L, unsigned bin, unsigned pool,
                              size_t bytes, LuaAllocPage **reserved) {
  global_State *g = G(L);
  LuaExecution *execution = luaX_execution(L);
  unsigned count, i, expected = 0, used = 0;
  if (g->gcstopem || luaW_iscollector(L) || execution == NULL ||
      execution->allocation_cache == NULL ||
      execution->allocation_cache->global != g)
    return 0;
  count = execution->allocation_cache->parallelism;
  if (count > ALLOC_PAGE_RESERVE + 1) count = ALLOC_PAGE_RESERVE + 1;
  if (count <= 1 ||
      !__atomic_compare_exchange_n(&g->allocation_reserving[pool], &expected,
          1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
    return 0;
  /* A previous initializer may have published while this backing request
  ** waited. One publisher per pool bounds both pending and offered reserves. */
  {
    int available;
    luaX_checksync(pthread_mutex_lock(&g->allocation_mutex[pool]));
    available = g->allocation_available[pool] != NULL;
    luaX_checksync(pthread_mutex_unlock(&g->allocation_mutex[pool]));
    if (available) goto done;
  }
  for (i = 1; i < count; i++) {
    LuaAllocPage *page = cast(LuaAllocPage *, firsttry(g, NULL, 0, bytes));
    if (page == NULL) break;
    preparepage(g, page, bin, pool, bytes);
    reserved[used++] = page;
  }
 done:
  if (used == 0)
    __atomic_store_n(&g->allocation_reserving[pool], 0, __ATOMIC_RELEASE);
  return used;
}

/* Caller owns the pool mutex. */
static void offerpage (global_State *g, LuaAllocPage *page) {
  LuaAllocPage **head = &g->allocation_available[page->pool];
  lua_assert(page->owner == NULL && page->available_previous == NULL);
  page->available_next = *head;
  page->available_previous = head;
  if (*head != NULL) (*head)->available_previous = &page->available_next;
  *head = page;
}

static void abandonpage (lua_State *L, unsigned pool) {
  global_State *g = G(L);
  LuaAllocationCache *cache = allocationcache(L);
  LuaAllocPage *page;
  luaX_checksync(pthread_mutex_lock(&g->allocation_mutex[pool]));
  page = cache->pages[pool];
  if (page != NULL) {
    lua_assert(page->owner == cache);
    cache->pages[pool] = NULL;
    page->owner = NULL;
    /* A full page has no transferable capacity. Its first actual return
    ** supplies the notification; refills must not queue fruitless sweep work. */
    lua_assert(page->local == NULL);
  }
  luaX_checksync(pthread_mutex_unlock(&g->allocation_mutex[pool]));
}

/* Metadata admission can lend this slot to another task. Keep the requested
** capacity in the cache record, rather than only on a parked caller's stack:
** otherwise each new borrower restarts a hot size class with a cold chunk. */
static size_t growcache (lua_State *L, LuaAllocationCache *cache, unsigned pool,
                         size_t minimum, size_t requested) {
  LuaExecution *execution = luaX_execution(L);
  size_t bytes;
  /* Only a provided cache can be lent to another task during admission.
  ** State/task-owned caches keep this request's demand on its retained stack;
  ** carrying old parser demand into their later allocations strands storage. */
  if (execution == NULL || execution->allocation_cache != cache)
    return requested > minimum ? requested : minimum;
  if (cache->growth_epoch != cache->global->trace_epoch) {
    memset(cache->growth, 0, sizeof(cache->growth));
    cache->growth_epoch = cache->global->trace_epoch;
  }
  bytes = minimum << cache->growth[pool];
  if (bytes > ALLOC_PAGE_MAX) bytes = ALLOC_PAGE_MAX;
  while (bytes < requested && bytes < ALLOC_PAGE_MAX) {
    bytes *= 2;
    if (bytes > ALLOC_PAGE_MAX) bytes = ALLOC_PAGE_MAX;
    cache->growth[pool]++;
  }
  return bytes < minimum ? minimum : bytes;
}

/* Either an assist or backing admission can resume on a different slot.
** Keep its usable cache; abandon a full one before claiming another page. */
static LuaAllocPage *reusecache (lua_State *L, unsigned pool, size_t minimum,
                                size_t *bytes, uint64_t epoch) {
  LuaAllocPage *page = allocationcache(L)->pages[pool];
  if (G(L)->trace_epoch != epoch) *bytes = minimum;
  if (page != NULL) {
    size_t grown;
    if (page->local != NULL || luaMP_remote(page) != NULL) return page;
    grown = page->bytes <= ALLOC_PAGE_MAX / 2
      ? page->bytes * 2 : ALLOC_PAGE_MAX;
    if (grown > *bytes) *bytes = grown;
    abandonpage(L, pool);
  }
  return NULL;
}

static LuaAllocPage *refill (lua_State *L, unsigned bin, unsigned pool) {
  global_State *g = G(L);
  LuaAllocPage *page = allocationcache(L)->pages[pool];
  LuaAllocPage *reserved[ALLOC_PAGE_RESERVE];
  unsigned count = 0, i;
  uint64_t epoch = g->trace_epoch;
  int fresh = 0;
  /* Cold size classes need only a small block supply. These are allocator
  ** chunks, not OS mappings: reserving 4 KiB per cold coroutine/class strands
  ** most of a small public heap. Always fit at least two complete blocks. */
  size_t minimum = LUA_ALLOC_PAGE_HEADER +
    2 * (sizeof(LuaAllocBlock) + luaMP_binbytes(bin));
  size_t bytes;
  if (minimum < ALLOC_PAGE_MIN) minimum = ALLOC_PAGE_MIN;
  bytes = growcache(L, allocationcache(L), pool, minimum, page == NULL ? 0
    : page->bytes <= ALLOC_PAGE_MAX / 2 ? page->bytes * 2 : ALLOC_PAGE_MAX);
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
  luaW_heapenter(L);
  /* Admission can park and borrow a different slot. Its cache may already
  ** have been refilled, so retain usable pages instead of overwriting them. */
  page = reusecache(L, pool, minimum, &bytes, epoch);
  if (page != NULL) goto ready;
  bytes = growcache(L, allocationcache(L), pool, minimum, bytes);
  page = availablepage(L, pool);
  if (page == NULL) {
    page = cast(LuaAllocPage *, firsttry(g, NULL, 0, bytes));
    if (page == NULL && bytes > minimum) {
      /* Growth is optional: a tight allocator can still supply a small page. */
      bytes = minimum;
      page = cast(LuaAllocPage *, firsttry(g, NULL, 0, bytes));
    }
    if (page == NULL && cantryagain(g)) {
      luaC_fullgc(L, 1);
      page = availablepage(L, pool);
      if (page != NULL) goto ready;
      page = cast(LuaAllocPage *, callfrealloc(g, NULL, 0, bytes));
    }
    if (page != NULL) {
      preparepage(g, page, bin, pool, bytes);
      fresh = 1;
      count = reservepages(L, bin, pool, bytes, reserved);
    }
  }
 ready:
  luaW_heapleave(L);
  if (fresh) {
    /* No safepoint below: participation keeps these linked, incomplete pages
    ** alive until every page is initialized and the spare batch is published.
    ** The deferred metadata handoff cannot switch this stack in the meantime. */
    luaMP_initpage(L, page);
    luaX_checksync(pthread_mutex_lock(&g->allocation_mutex[pool]));
    page->owner = allocationcache(L);
    page->owner->pages[pool] = page;
    luaX_checksync(pthread_mutex_unlock(&g->allocation_mutex[pool]));
    for (i = 0; i < count; i++) {
      luaMP_initpage(L, reserved[i]);
      luaX_checksync(pthread_mutex_lock(&g->allocation_mutex[pool]));
      offerpage(g, reserved[i]);
      luaX_checksync(pthread_mutex_unlock(&g->allocation_mutex[pool]));
    }
    if (count != 0)
      __atomic_store_n(&g->allocation_reserving[pool], 0, __ATOMIC_RELEASE);
  }
  return page;
}

static void *allocatebacking (lua_State *L, size_t size, int tag) {
  LuaAllocBlock *block;
  size_t bytes;
  global_State *g = G(L);
  if (size > MAX_SIZE - sizeof(LuaAllocBlock)) return NULL;
  bytes = size + sizeof(LuaAllocBlock);
  luaW_heapenter(L);
  block = cast(LuaAllocBlock *, firsttry(g, NULL, cast_sizet(tag), bytes));
  if (block == NULL)
    block = cast(LuaAllocBlock *, tryagain(L, NULL, cast_sizet(tag), bytes));
  if (block != NULL) {
    block->block.page = 0;
    account(L, -cast(l_mem, size));
  }
  luaW_heapleave(L);
  return block != NULL ? block + 1 : NULL;
}

/* Persistent SDK records must not pin transient object pages. Retain normal
** accounting and retry behavior, while allocating their exact backing size. */
void *luaM_alloccontrol (lua_State *L, size_t size) {
  return allocatebacking(L, size, 0);
}

static void *allocateblock (lua_State *L, size_t size, int tag,
                            unsigned poolbase) {
  unsigned bin;
  for (bin = 0; bin < LUA_ALLOC_BINS; bin++) {
    if (size <= luaMP_binbytes(bin)) {
      unsigned pool = bin + poolbase;
      LuaAllocPage *page = allocationcache(L)->pages[pool];
      LuaAllocBlock *block = page != NULL ? takeblock(page) : NULL;
      if (block == NULL) {
        page = refill(L, bin, pool);
        if (page == NULL) return NULL;
        block = takeblock(page);
        lua_assert(block != NULL);
      }
      account(L, -cast(l_mem, size));
      return block + 1;
    }
  }
  return allocatebacking(L, size, tag);
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
  allocation = block != NULL ? cast(LuaAllocBlock *, block) - 1 : NULL;
  lua_assert(allocation == NULL || !luaMP_retired(allocation));
  if (allocation != NULL && luaMP_page(allocation) != NULL &&
      nsize <= luaMP_binbytes(luaMP_page(allocation)->bin)) {
    account(L, cast(l_mem, osize) - cast(l_mem, nsize));
    return block;
  }
  if (allocation != NULL && luaMP_page(allocation) == NULL &&
      nsize > luaMP_binbytes(LUA_ALLOC_BINS - 1)) {
    global_State *g = G(L);
    LuaAllocBlock *replacement;
    size_t oldbytes = osize + sizeof(LuaAllocBlock);
    size_t bytes;
    if (nsize > MAX_SIZE - sizeof(LuaAllocBlock)) return NULL;
    bytes = nsize + sizeof(LuaAllocBlock);
    luaW_heapenter(L);
    replacement = cast(LuaAllocBlock *, firsttry(g, allocation, oldbytes, bytes));
    if (replacement == NULL)
      replacement = cast(LuaAllocBlock *, tryagain(L, allocation, oldbytes, bytes));
    if (replacement != NULL) {
      replacement->block.page = 0;
      account(L, cast(l_mem, osize) - cast(l_mem, nsize));
    }
    luaW_heapleave(L);
    return replacement != NULL ? replacement + 1 : NULL;
  }
  newblock = allocateblock(L, nsize, 0, 0);
  if (newblock == NULL) return NULL;
  if (block != NULL) {
    int backing = luaMP_page(allocation) == NULL;
    if (backing) luaW_heapenter(L);
    memcpy(newblock, block, osize < nsize ? osize : nsize);
    luaM_free_(L, block, osize);
    if (backing) luaW_heapleave(L);
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
    void *block = allocateblock(L, size, tag, 0);
    if (block == NULL) luaM_error(L);
    return block;
  }
}

/* Retained call frames must not pin large pages of short-lived table nodes
** or objects. Unlike luaM_malloc_, this reports failure for optional frames. */
CallInfo *luaM_newframe (lua_State *L) {
  return cast(CallInfo *, allocateblock(L, sizeof(CallInfo), 0, LUA_ALLOC_BINS));
}
