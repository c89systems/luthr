/* Private allocation-page layout and initialization. */
#ifndef lmempage_h
#define lmempage_h

#include "lua.h"
#include "llimits.h"
#include "lexec.h"
#include <stdint.h>

struct global_State;
struct LuaAllocationCache;
struct LuaExecution;
struct GCObject;

typedef struct LuaAllocPage LuaAllocPage;
#define LUA_PAGE_TRACE_WORDS 16
typedef union LuaAllocBlock LuaAllocBlock;
union LuaAllocBlock {
  struct {
    uintptr_t page;  /* aligned page identity and allocation flags */
    union {
      LuaAllocBlock *next;  /* free blocks only */
      struct GCObject *object;  /* live allocation membership */
    } link;
  } block;
  union { LUAI_MAXALIGN; } alignment;
};

struct LuaAllocPage {
  LuaAllocPage *next;
  LuaAllocPage **previous;  /* pool-mutex allocation directory */
  LuaAllocPage *available_next;
  LuaAllocPage **available_previous;  /* pool mutex; NULL unless offered */
  LuaAllocPage *dirty_next;  /* uniquely published notification/retirement link */
  struct LuaAllocationCache *owner;  /* pool mutex protects cache association */
  LuaAllocBlock *local;  /* sole allocation owner, or a borrowed cache lease */
  uintptr_t remote;  /* atomic returned blocks, with notification ownership bit */
  LuaAllocBlock *notified;  /* first return stays allocated until its notice is claimed */
  size_t bytes;
  uint64_t sweep_epoch;  /* zero until first collectable object */
  unsigned used;  /* owner allocations, less returns collected in batches */
  unsigned blockbytes;  /* payload capacity, excluding the block header */
  unsigned pool;
  unsigned state;  /* protected by the allocation pool mutex */
  LuaAllocPage *trace_next;  /* owned by the page notice until it is retired */
  uint64_t trace_slots[LUA_PAGE_TRACE_WORDS];  /* atomic pending mark slots */
  unsigned trace_pending;  /* atomic word summary and notice ownership */
  unsigned trace_scale;  /* reciprocal of the slot stride in 16-bit units */
  size_t task_bytes;  /* payload of allocated slots while task-owned */
  unsigned task_private;
  unsigned task_backing;
  unsigned task_dirty;
  unsigned task_top;  /* first virgin slot, before any free-list reuse */
  unsigned task_capacity;  /* nonzero through the lifetime of a task page */
  unsigned slot_end;  /* finalizer tickets and private bitmaps follow the slots */
  lu_byte task_mark;  /* inherited by task tables when the page is published */
  LuaAllocPage *sweep_next;
  LuaAllocPage **sweep_previous;  /* pool-mutex collector directory */
};

typedef struct LuaUserdataPools {
  pthread_mutex_t mutex[LUA_ALLOC_USERDATA_BINS];
  LuaAllocPage *pages[LUA_ALLOC_USERDATA_BINS];
  LuaAllocPage *available[LUA_ALLOC_USERDATA_BINS];
  LuaAllocPage *sweep[LUA_ALLOC_USERDATA_BINS];
  LuaAllocPage *cursor[LUA_ALLOC_USERDATA_BINS];
  LuaAllocPage *spare[LUA_ALLOC_USERDATA_BINS];
  LuaAllocPage **spare_tail[LUA_ALLOC_USERDATA_BINS];
} LuaUserdataPools;

LUAI_FUNC unsigned luaMP_sweepcount (struct global_State *g);
LUAI_FUNC LuaAllocPage **luaMP_sweephead (struct global_State *g, unsigned pool);
LUAI_FUNC LuaAllocPage **luaMP_sweepcursor (struct global_State *g, unsigned pool);
LUAI_FUNC pthread_mutex_t *luaMP_sweepmutex (struct global_State *g, unsigned pool);

l_sinline unsigned luaMP_sweeppool (unsigned index) {
  return index < LUA_ALLOC_BINS ? index
    : LUA_ALLOC_USERDATA_POOL + index - LUA_ALLOC_BINS;
}

/* Task pages reserve this storage after their slots. Keep the same slot
** boundaries after publication, until the backing page is reused. */
typedef struct LuaTaskPageBits {
  uint64_t live[LUA_PAGE_TRACE_WORDS];
  uint64_t young[LUA_PAGE_TRACE_WORDS];
} LuaTaskPageBits;

l_sinline size_t luaMP_slotend (const LuaAllocPage *page) {
  return page->slot_end;
}

l_sinline LuaTaskPageBits *luaMP_taskbits (LuaAllocPage *page) {
  lua_assert(page->task_capacity != 0);
  return cast(LuaTaskPageBits *, cast_charp(page) + page->bytes -
                                sizeof(LuaTaskPageBits));
}

#define LUA_PAGE_NOTIFIED cast(uintptr_t, 1)
#define luaMP_remote(page) cast(LuaAllocBlock *, \
  __atomic_load_n(&(page)->remote, __ATOMIC_ACQUIRE) & ~LUA_PAGE_NOTIFIED)

typedef struct LuaCacheBorrow {
  void **permission;
  lua_State *thread;
  int reservation;
  void (*returned)(void *);
  void *context;
} LuaCacheBorrow;

LUAI_FUNC void luaMP_notify (struct global_State *g, LuaAllocPage *page);
LUAI_FUNC void luaMP_publish (struct global_State *g, LuaAllocPage *page);
LUAI_FUNC void luaMP_return (struct global_State *g, LuaAllocBlock *block);
LUAI_FUNC void luaMP_returnbatch (struct global_State *g, LuaAllocPage *page,
                                LuaAllocBlock *first, LuaAllocBlock *last);
LUAI_FUNC void luaM_freeobjectbatch (lua_State *L, LuaAllocPage *page,
                                  LuaAllocBlock *first, LuaAllocBlock *last,
                                    unsigned count, l_mem bytes, int private);
LUAI_FUNC void luaMP_resetnotice (LuaAllocPage *page);
LUAI_FUNC void *luaM_trytasktable (lua_State *L, size_t size, int refill);
LUAI_FUNC void *luaM_trytaskstring (lua_State *L, size_t size, int refill);
LUAI_FUNC void *luaM_trytaskuserdata (lua_State *L, size_t size, int refill);
LUAI_FUNC void luaM_adopttaskpage (lua_State *L, LuaAllocPage *page,
                                 uint64_t epoch, lu_byte mark);
LUAI_FUNC void luaM_discardtaskpage (lua_State *L, LuaAllocPage *page);
LUAI_FUNC void luaM_trimtaskpages (lua_State *L);
/* The caller holds the page's pool mutex throughout the short lease. */
LUAI_FUNC int luaMC_borrow (lua_State *L, struct LuaAllocationCache *cache,
                           LuaCacheBorrow *borrow, int exclusive);
/* Release the pool mutex before notifying a returned lease. */
LUAI_FUNC void luaMC_return (LuaCacheBorrow *borrow);
LUAI_FUNC void luaMC_notify (LuaCacheBorrow *borrow);

#define LUA_ALLOC_PAGE_HEADER \
  (((sizeof(LuaAllocPage) + sizeof(LuaAllocBlock) - 1) / sizeof(LuaAllocBlock)) \
    * sizeof(LuaAllocBlock))
#define LUA_ALLOC_MAX 512
#define LUA_ALLOC_USERDATA_MAX 2048
#define LUA_ALLOC_PAGE_MAX 65536

l_sinline unsigned luaMP_slotindex (const LuaAllocPage *page,
                                    const LuaAllocBlock *block) {
  unsigned offset = cast_uint(cast(const char *, block) -
    cast(const char *, page)) - LUA_ALLOC_PAGE_HEADER;
  unsigned slot = (offset * page->trace_scale) >> 16;
  lua_assert(offset < LUA_ALLOC_PAGE_MAX &&
    slot * (page->blockbytes + sizeof(*block)) == offset);
  return slot;
}

l_sinline LuaAllocBlock *luaMP_slotblock (LuaAllocPage *page, unsigned slot) {
  size_t stride = sizeof(LuaAllocBlock) + page->blockbytes;
  return cast(LuaAllocBlock *, cast_charp(page) + LUA_ALLOC_PAGE_HEADER +
                              slot * stride);
}

l_sinline uint64_t *luaMP_finalizerstamp (LuaAllocPage *page,
                                        const LuaAllocBlock *block) {
  return cast(uint64_t *, cast_charp(page) + luaMP_slotend(page)) +
         luaMP_slotindex(page, block);
}

l_sinline size_t luaMP_binbytes (unsigned bin) {
  static const unsigned short sizes[LUA_ALLOC_BINS] =
    {16, 32, 64, 96, 112, 128, 160, 192, 256, 288, LUA_ALLOC_MAX};
  lua_assert(bin < LUA_ALLOC_BINS);
  return sizes[bin];
}

/* Class lookup in 16-byte units. 96 and 112 fit compact records.
** The 288-byte class also fits eight hash nodes beside their table header. */
l_sinline unsigned luaMP_sizeclass (size_t bytes) {
  static const unsigned char bins[LUA_ALLOC_MAX / 16 + 1] = {
    0, 0, 1, 2, 2, 3, 3, 4, 5, 6, 6, 7, 7, 8, 8, 8, 8,
    9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10
  };
  lua_assert(bytes <= LUA_ALLOC_MAX);
  return bins[(bytes + 15) / 16];
}
enum { LUA_PAGE_OPEN, LUA_PAGE_RETURNING, LUA_PAGE_SWEEPING, LUA_PAGE_SWEPT };
enum { LUA_BLOCK_RETIRED = 1, LUA_BLOCK_COLLECTABLE = 2, LUA_BLOCK_NEW = 4,
       LUA_BLOCK_PARITY = 8, LUA_BLOCK_BIRTH = LUA_BLOCK_NEW | LUA_BLOCK_PARITY,
       LUA_BLOCK_FLAGS = LUA_BLOCK_RETIRED | LUA_BLOCK_COLLECTABLE | LUA_BLOCK_BIRTH };
#define luaMP_header(b) __atomic_load_n(&(b)->block.page, __ATOMIC_ACQUIRE)
#define luaMP_retired(b) ((luaMP_header(b) & LUA_BLOCK_RETIRED) != 0)
#define luaMP_pageof(header) cast(LuaAllocPage *, (header) & ~cast(uintptr_t, LUA_BLOCK_FLAGS))
#define luaMP_page(b) \
  luaMP_pageof(luaMP_header(b))

l_sinline uintptr_t luaMP_birth (uint64_t epoch) {
  return LUA_BLOCK_NEW | ((epoch & 1) ? LUA_BLOCK_PARITY : 0);
}

l_sinline void luaMP_young (LuaAllocPage *page, uint64_t epoch) {
  if (__atomic_load_n(&page->sweep_epoch, __ATOMIC_RELAXED) != epoch)
    __atomic_store_n(&page->sweep_epoch, epoch, __ATOMIC_RELAXED);
}

/* The first collectable allocation enrols the page before the next sweep. */
LUAI_FUNC void luaM_enrollsweep (lua_State *L, LuaAllocPage *page,
                                 uint64_t epoch);
l_sinline void luaMP_publishslot (lua_State *L, LuaAllocPage *page,
                                   uint64_t epoch) {
  uint64_t previous = __atomic_load_n(&page->sweep_epoch, __ATOMIC_RELAXED);
  if (l_unlikely(previous == 0))
    luaM_enrollsweep(L, page, epoch);
  else if (previous != epoch)
    __atomic_store_n(&page->sweep_epoch, epoch, __ATOMIC_RELAXED);
}

/* No allocation or safepoint. Keep the page private and remain admitted until
** initialization and publication finish; collection then sees complete pages. */
LUAI_FUNC void luaMP_initpage (lua_State *L, LuaAllocPage *page);
/* Sole allocation owner or borrowed cache lease. Consume a whole return list. */
LUAI_FUNC void luaMP_collectpage (LuaAllocPage *page);

#endif
