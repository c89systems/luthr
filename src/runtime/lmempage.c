/* Private allocation-page initialization. */
#define lmempage_c
#define LUA_CORE
#include "lprefix.h"

#include "lmempage.h"
#include "lstate.h"

void luaMP_initpage (lua_State *L, LuaAllocPage *page) {
  size_t stride = sizeof(LuaAllocBlock) + luaMP_binbytes(page->bin);
  size_t offset;
  lua_assert(!completestate(G(L)) || luaW_iscollector(L) ||
    __atomic_load_n(&L->admission->active, __ATOMIC_SEQ_CST) != 0);
  UNUSED(L);
  for (offset = LUA_ALLOC_PAGE_HEADER;
       offset + stride <= page->bytes; offset += stride) {
    LuaAllocBlock *block = cast(LuaAllocBlock *, cast_charp(page) + offset);
    block->block.page = page;
    block->block.next = page->free;
    page->free = block;
  }
}
