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
  uint64_t table_epoch;  /* zero until first table, then last eligible sweep */
  unsigned used;  /* owner allocations, less returns collected in batches */
  unsigned blockbytes;  /* payload capacity, excluding the block header */
  unsigned pool;
  unsigned state;  /* protected by the allocation pool mutex */
};

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
                                  unsigned count, l_mem bytes);
LUAI_FUNC void luaMP_resetnotice (LuaAllocPage *page);
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
#define LUA_ALLOC_PAGE_MAX 65536

l_sinline size_t luaMP_binbytes (unsigned bin) {
  static const unsigned short sizes[LUA_ALLOC_BINS] =
    {16, 32, 64, 112, 128, 160, 192, 256, LUA_ALLOC_MAX};
  lua_assert(bin < LUA_ALLOC_BINS);
  return sizes[bin];
}

/* Class lookup in 16-byte units. 112 fits small tables with inline arrays.
** 160 and 192 keep larger inline layouts below the 256-byte class. */
l_sinline unsigned luaMP_sizeclass (size_t bytes) {
  static const unsigned char bins[LUA_ALLOC_MAX / 16 + 1] = {
    0, 0, 1, 2, 2, 3, 3, 3, 4, 5, 5, 6, 6, 7, 7, 7, 7,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8
  };
  lua_assert(bytes <= LUA_ALLOC_MAX);
  return bins[(bytes + 15) / 16];
}
enum { LUA_PAGE_OPEN, LUA_PAGE_RETURNING, LUA_PAGE_SWEEPING, LUA_PAGE_SWEPT };
enum { LUA_BLOCK_RETIRED = 1, LUA_BLOCK_TABLE = 2, LUA_BLOCK_NEW = 4,
       LUA_BLOCK_PARITY = 8, LUA_BLOCK_BIRTH = LUA_BLOCK_NEW | LUA_BLOCK_PARITY,
       LUA_BLOCK_FLAGS = LUA_BLOCK_RETIRED | LUA_BLOCK_TABLE | LUA_BLOCK_BIRTH };
#define luaMP_header(b) __atomic_load_n(&(b)->block.page, __ATOMIC_ACQUIRE)
#define luaMP_retired(b) ((luaMP_header(b) & LUA_BLOCK_RETIRED) != 0)
#define luaMP_pageof(header) cast(LuaAllocPage *, (header) & ~cast(uintptr_t, LUA_BLOCK_FLAGS))
#define luaMP_page(b) \
  luaMP_pageof(luaMP_header(b))

l_sinline uintptr_t luaMP_birth (uint64_t epoch) {
  return LUA_BLOCK_NEW | ((epoch & 1) ? LUA_BLOCK_PARITY : 0);
}

l_sinline void luaMP_young (LuaAllocPage *page, uint64_t epoch) {
  if (__atomic_load_n(&page->table_epoch, __ATOMIC_RELAXED) != epoch)
    __atomic_store_n(&page->table_epoch, epoch, __ATOMIC_RELAXED);
}

/* No allocation or safepoint. Keep the page private and remain admitted until
** initialization and publication finish; collection then sees complete pages. */
LUAI_FUNC void luaMP_initpage (lua_State *L, LuaAllocPage *page);
/* Sole allocation owner or borrowed cache lease. Consume a whole return list. */
LUAI_FUNC void luaMP_collectpage (LuaAllocPage *page);

#endif
