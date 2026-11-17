/* Private allocation-page layout and initialization. */
#ifndef lmempage_h
#define lmempage_h

#include "lua.h"
#include "llimits.h"

typedef struct LuaAllocPage LuaAllocPage;
typedef union LuaAllocBlock LuaAllocBlock;
union LuaAllocBlock {
  struct {
    LuaAllocPage *page;
    LuaAllocBlock *next;
  } block;
  union { LUAI_MAXALIGN; } alignment;
};

struct LuaAllocPage {
  LuaAllocPage *next;
  LuaAllocPage *available_next;
  LuaAllocBlock *free;
  size_t bytes;
  unsigned live;
  unsigned bin;
  unsigned pool;
};

#define LUA_ALLOC_PAGE_HEADER \
  (((sizeof(LuaAllocPage) + sizeof(LuaAllocBlock) - 1) / sizeof(LuaAllocBlock)) \
    * sizeof(LuaAllocBlock))
#define luaMP_binbytes(bin) (cast_sizet(16) << (bin))

/* No allocation or safepoint. Keep the page private and remain admitted until
** initialization and publication finish; collection then sees complete pages. */
LUAI_FUNC void luaMP_initpage (lua_State *L, LuaAllocPage *page);

#endif
