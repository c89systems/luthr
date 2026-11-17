/* Private allocation-page layout and initialization. */
#ifndef lmempage_h
#define lmempage_h

#include "lua.h"
#include "llimits.h"
#include <stdint.h>

struct global_State;
struct LuaAllocationCache;
struct LuaExecution;

typedef struct LuaAllocPage LuaAllocPage;
typedef union LuaAllocBlock LuaAllocBlock;
union LuaAllocBlock {
  struct {
    uintptr_t page;  /* aligned page identity, with logical-retirement bit */
    LuaAllocBlock *next;  /* free list, or collector work while allocated */
  } block;
  union { LUAI_MAXALIGN; } alignment;
};

struct LuaAllocPage {
  LuaAllocPage *next;
  LuaAllocPage **previous;  /* metadata-owned allocation directory */
  LuaAllocPage *available_next;
  LuaAllocPage **available_previous;  /* pool mutex; NULL unless offered */
  LuaAllocPage *dirty_next;  /* uniquely published notification/retirement link */
  struct LuaAllocationCache *owner;  /* pool mutex protects cache association */
  LuaAllocBlock *local;  /* sole allocation owner, or a borrowed cache lease */
  uintptr_t remote;  /* atomic returned blocks, with notification ownership bit */
  LuaAllocBlock *notified;  /* first return stays allocated until its notice is claimed */
  size_t bytes;
  unsigned used;  /* owner allocations, less returns collected in batches */
  unsigned bin;
  unsigned pool;
  unsigned retiring;  /* no owner/available/notification references remain */
};

#define LUA_PAGE_NOTIFIED cast(uintptr_t, 1)
#define luaMP_remote(page) cast(LuaAllocBlock *, \
  __atomic_load_n(&(page)->remote, __ATOMIC_ACQUIRE) & ~LUA_PAGE_NOTIFIED)

typedef struct LuaCacheBorrow {
  struct LuaAllocationCache *cache;
  struct global_State *global;
  lua_State *thread;
  int reservation;
  int permission;
  void (*returned)(void *);
  void *context;
} LuaCacheBorrow;

LUAI_FUNC void luaMP_notify (struct global_State *g, LuaAllocPage *page);
LUAI_FUNC void luaMP_publish (struct global_State *g, LuaAllocPage *page);
LUAI_FUNC void luaMP_return (struct global_State *g, LuaAllocBlock *block);
LUAI_FUNC void luaMP_resetnotice (LuaAllocPage *page);
LUAI_FUNC void luaMC_enter (struct LuaExecution *execution);
LUAI_FUNC void luaMC_leave (struct LuaExecution *execution);
/* The caller holds the page's pool mutex throughout the short lease. */
LUAI_FUNC int luaMC_borrow (lua_State *L, struct LuaAllocationCache *cache,
                           LuaCacheBorrow *borrow, int exclusive);
/* Release the pool mutex before notifying a returned lease. */
LUAI_FUNC void luaMC_return (LuaCacheBorrow *borrow);
LUAI_FUNC void luaMC_notify (LuaCacheBorrow *borrow);
LUAI_FUNC void luaMC_acquire (struct LuaAllocationCache *cache);
LUAI_FUNC void luaMC_release (struct LuaAllocationCache *cache);
LUAI_FUNC void luaMC_join (struct LuaAllocationCache *cache);

#define LUA_ALLOC_PAGE_HEADER \
  (((sizeof(LuaAllocPage) + sizeof(LuaAllocBlock) - 1) / sizeof(LuaAllocBlock)) \
    * sizeof(LuaAllocBlock))
#define luaMP_binbytes(bin) (cast_sizet(16) << (bin))
#define luaMP_retired(b) (((b)->block.page & 1u) != 0)
#define luaMP_page(b) \
  cast(LuaAllocPage *, (b)->block.page & ~cast(uintptr_t, 1))

/* No allocation or safepoint. Keep the page private and remain admitted until
** initialization and publication finish; collection then sees complete pages. */
LUAI_FUNC void luaMP_initpage (lua_State *L, LuaAllocPage *page);
/* Sole allocation owner or borrowed cache lease. Consume a whole return list. */
LUAI_FUNC void luaMP_collectpage (LuaAllocPage *page);

#endif
