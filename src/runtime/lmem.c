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
      (!luaC_hasreclaimers(L) && !luaC_tracing(L)))
    return;
  epoch = __atomic_load_n(&world->work_epoch, __ATOMIC_ACQUIRE);
  if (L->assist_epoch != epoch) {
    L->assist_epoch = epoch;
    L->assist_debt = 0;
  }
  bytes = cast(lu_mem, 0) - cast(lu_mem, change);
  L->assist_debt = bytes > cast(lu_mem, MAX_LMEM) - L->assist_debt
    ? cast(lu_mem, MAX_LMEM) : L->assist_debt + bytes;
}

void luaM_flushdebt (lua_State *L) {
  l_mem debt = L->allocation_debt;
  l_mem reclaimed = L->reclaim_credit;
  L->allocation_debt = 0;
  L->reclaim_credit = 0;
  accountassist(L, debt);
  if (debt != 0)
    adddebt(G(L), debt, !completestate(G(L)) || luaW_iscollector(L));
  if (reclaimed != 0)
    __atomic_fetch_add(&G(L)->GCreclaimed, reclaimed, __ATOMIC_RELAXED);
}

static void accountowned (lua_State *L, l_mem change, int exclusive) {
  if (L->reclaiming && change > 0) {
    if (change >= DEBT_BATCH) {
      luaM_flushdebt(L);
      __atomic_fetch_add(&G(L)->GCreclaimed, change, __ATOMIC_RELAXED);
    }
    else {
      L->reclaim_credit += change;
      if (L->reclaim_credit >= DEBT_BATCH) luaM_flushdebt(L);
    }
    return;
  }
  if (exclusive || change >= DEBT_BATCH || change <= -DEBT_BATCH) {
    luaM_flushdebt(L);
    if (!exclusive) accountassist(L, change);
    adddebt(G(L), change, exclusive);
  }
  else {
    L->allocation_debt += change;
    if (L->allocation_debt >= DEBT_BATCH || L->allocation_debt <= -DEBT_BATCH)
      luaM_flushdebt(L);
  }
}

static void account (lua_State *L, l_mem change) {
  accountowned(L, change, !completestate(G(L)) || luaW_iscollector(L));
}

#define ALLOC_PAGE_MIN 4096
#define ALLOC_PAGE_MAX 65536
#define ALLOC_PAGE_RESERVE 3

/* One admitted coroutine consumes a cached page. Other owners only return
** blocks; MPSC push never reads a node's successor, and only the consumer
** removes nodes. Collection retires caches before changing page ownership. */
static void returnblock (LuaAllocBlock *block, int exclusive) {
  LuaAllocPage *page = block->block.page;
  LuaAllocBlock *head = __atomic_load_n(&page->free, __ATOMIC_RELAXED);
  if (exclusive) {
    unsigned live = __atomic_load_n(&page->live, __ATOMIC_RELAXED);
    lua_assert(live != 0);
    block->block.next = head;
    __atomic_store_n(&page->free, block, __ATOMIC_RELEASE);
    __atomic_store_n(&page->live, live - 1, __ATOMIC_RELAXED);
    return;
  }
  do {
    block->block.next = head;
  } while (!__atomic_compare_exchange_n(&page->free, &head, block, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED));
  __atomic_fetch_sub(&page->live, 1, __ATOMIC_RELAXED);
}

static LuaAllocBlock *takeblock (LuaAllocPage *page) {
  LuaAllocBlock *head = __atomic_load_n(&page->free, __ATOMIC_ACQUIRE);
  while (head != NULL) {
    LuaAllocBlock *next = head->block.next;
    if (__atomic_compare_exchange_n(&page->free, &head, next, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
      __atomic_fetch_add(&page->live, 1, __ATOMIC_RELAXED);
      return head;
    }
  }
  return NULL;
}

static LuaAllocPage **allocationcache (lua_State *L) {
  LuaExecution *execution = luaQ_current();
  return execution != NULL && execution->allocation_cache != NULL &&
         execution->allocation_cache->global == G(L)
    ? execution->allocation_cache->pages : L->allocation_cache;
}

void luaM_reclaimpages (lua_State *L) {
  global_State *g = G(L);
  LuaParticipation *participant;
  LuaAllocationCache *cache;
  LuaAllocPage **link;
  int i;
  for (i = 0; i < LUA_ALLOC_POOLS; i++) {
    lua_assert(__atomic_load_n(&g->allocation_reserving[i],
                               __ATOMIC_RELAXED) == 0);
    __atomic_store_n(&g->allocation_available[i], NULL, __ATOMIC_RELAXED);
  }
  for (participant = g->world.participants; participant != NULL;
       participant = participant->next)
    for (i = 0; i < LUA_ALLOC_POOLS; i++)
      participant->thread->allocation_cache[i] = NULL;
  for (cache = g->slot_caches; cache != NULL; cache = cache->next)
    for (i = 0; i < LUA_ALLOC_POOLS; i++) cache->pages[i] = NULL;
  for (link = &g->allocation_pages; *link != NULL;) {
    LuaAllocPage *page = *link;
    if (__atomic_load_n(&page->live, __ATOMIC_RELAXED) == 0) {
      *link = page->next;
      callfrealloc(g, page, page->bytes, 0);
    }
    else {
      if (__atomic_load_n(&page->free, __ATOMIC_RELAXED) != NULL) {
        page->available_next = __atomic_load_n(
          &g->allocation_available[page->pool], __ATOMIC_RELAXED);
        __atomic_store_n(&g->allocation_available[page->pool], page,
                         __ATOMIC_RELEASE);
      }
      link = &page->next;
    }
  }
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
  int exclusive;
  lua_assert((osize == 0) == (block == NULL));
  if (block == NULL) return;
  exclusive = !completestate(G(L)) || luaW_iscollector(L);
  allocation = cast(LuaAllocBlock *, block) - 1;
  if (allocation->block.page != NULL)
    returnblock(allocation, exclusive);
  else {
    luaW_heapenter(L);
    callfrealloc(G(L), allocation, osize + sizeof(LuaAllocBlock), 0);
    luaW_heapleave(L);
  }
  accountowned(L, cast(l_mem, osize), exclusive);
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
  LuaAllocPage **head = &G(L)->allocation_available[pool];
  LuaAllocPage *page = __atomic_load_n(head, __ATOMIC_ACQUIRE);
  /* Consumers remain admitted throughout this pop. Only quiescent collection
  ** reinserts or frees pages, so a removed node cannot reappear during a CAS.
  ** Keep its successor immutable until that collection: another consumer may
  ** still read it before noticing that its own CAS lost. */
  while (page != NULL) {
    LuaAllocPage *next = page->available_next;
    if (__atomic_compare_exchange_n(head, &page, next, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
      return page;
  }
  return NULL;
}

static void preparepage (global_State *g, LuaAllocPage *page,
                         unsigned bin, unsigned pool, size_t bytes) {
  page->free = NULL;
  page->bin = bin;
  page->pool = pool;
  page->available_next = NULL;
  page->live = 0;
  page->bytes = bytes;
  page->next = g->allocation_pages;
  g->allocation_pages = page;
}

/* Amortize backing admission for a parallel team. Refill reaches this point
** only after finding its pool empty under metadata ownership. At most three
** spare pages remain per pool, independent of task count. Reserves are optional:
** failure stops the batch without collection or failing the requested block. */
static unsigned reservepages (lua_State *L, unsigned bin, unsigned pool,
                              size_t bytes, LuaAllocPage **reserved) {
  global_State *g = G(L);
  LuaExecution *execution = luaQ_current();
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
  if (__atomic_load_n(&g->allocation_available[pool], __ATOMIC_ACQUIRE) != NULL)
    goto done;
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

static void publishpage (global_State *g, LuaAllocPage *page) {
  LuaAllocPage **head = &g->allocation_available[page->pool];
  LuaAllocPage *next = __atomic_load_n(head, __ATOMIC_RELAXED);
  do {
    page->available_next = next;
  } while (!__atomic_compare_exchange_n(head, &next, page, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

static LuaAllocPage *refill (lua_State *L, unsigned bin, unsigned pool) {
  global_State *g = G(L);
  LuaAllocPage *page = allocationcache(L)[pool];
  LuaAllocPage *reserved[ALLOC_PAGE_RESERVE];
  unsigned count = 0, i;
  int fresh = 0;
  size_t bytes = page == NULL ? ALLOC_PAGE_MIN
    : page->bytes < ALLOC_PAGE_MAX ? page->bytes * 2 : ALLOC_PAGE_MAX;
  /* Copy the growth hint before metadata admission can collect the old page.
  ** Cold bins remain small; hot bins amortize serialized backing allocation. */
  page = availablepage(L, pool);
  if (page != NULL) {
    allocationcache(L)[pool] = page;
    return page;
  }
  luaW_heapenter(L);
  /* Admission can park and borrow a different slot. Its cache may already
  ** have been refilled, so retain usable pages instead of overwriting them. */
  page = allocationcache(L)[pool];
  if (page != NULL) {
    size_t grown;
    if (__atomic_load_n(&page->free, __ATOMIC_ACQUIRE) != NULL) goto ready;
    grown = page->bytes < ALLOC_PAGE_MAX ? page->bytes * 2 : ALLOC_PAGE_MAX;
    if (grown > bytes) bytes = grown;
  }
  page = availablepage(L, pool);
  if (page == NULL) {
    page = cast(LuaAllocPage *, firsttry(g, NULL, 0, bytes));
    if (page == NULL && bytes > ALLOC_PAGE_MIN) {
      /* Growth is optional: a tight allocator can still supply a small page. */
      bytes = ALLOC_PAGE_MIN;
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
    for (i = 0; i < count; i++) {
      luaMP_initpage(L, reserved[i]);
      publishpage(g, reserved[i]);
    }
    if (count != 0)
      __atomic_store_n(&g->allocation_reserving[pool], 0, __ATOMIC_RELEASE);
  }
  allocationcache(L)[pool] = page;
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
    block->block.page = NULL;
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
      LuaAllocPage *page = allocationcache(L)[pool];
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
  if (allocation != NULL && allocation->block.page != NULL &&
      nsize <= luaMP_binbytes(allocation->block.page->bin)) {
    account(L, cast(l_mem, osize) - cast(l_mem, nsize));
    return block;
  }
  if (allocation != NULL && allocation->block.page == NULL &&
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
      replacement->block.page = NULL;
      account(L, cast(l_mem, osize) - cast(l_mem, nsize));
    }
    luaW_heapleave(L);
    return replacement != NULL ? replacement + 1 : NULL;
  }
  newblock = allocateblock(L, nsize, 0, 0);
  if (newblock == NULL) return NULL;
  if (block != NULL) {
    int backing = allocation->block.page == NULL;
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
