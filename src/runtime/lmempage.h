/* Private allocation-page layout and initialization. */
#ifndef lmempage_h
#define lmempage_h

#include "lua.h"
#include "llimits.h"
#include <stdint.h>

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
  LuaAllocPage *available_next;
  LuaAllocBlock *local;  /* sole allocation owner; transferred at quiescence */
  LuaAllocBlock *remote;  /* atomic returns from arbitrary reclaimers */
  size_t bytes;
  unsigned used;  /* owner allocations, less returns collected in batches */
  unsigned bin;
  unsigned pool;
};

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
/* Sole allocation owner, or quiescent collector. Consume a whole return list. */
LUAI_FUNC void luaMP_collectpage (LuaAllocPage *page);

#endif
