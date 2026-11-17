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
  lua_assert((cast(uintptr_t, page) & 1u) == 0);
  for (offset = LUA_ALLOC_PAGE_HEADER;
       offset + stride <= page->bytes; offset += stride) {
    LuaAllocBlock *block = cast(LuaAllocBlock *, cast_charp(page) + offset);
    block->block.page = cast(uintptr_t, page);
    block->block.next = page->local;
    page->local = block;
  }
}

void luaMP_collectpage (LuaAllocPage *page) {
  LuaAllocBlock *head = __atomic_exchange_n(&page->remote, NULL,
                                            __ATOMIC_ACQUIRE);
  LuaAllocBlock *tail = head;
  unsigned returned = 0;
  if (head == NULL) return;
  for (;;) {
    returned++;
    if (tail->block.next == NULL) break;
    tail = tail->block.next;
  }
  lua_assert(page->used >= returned);
  page->used -= returned;
  tail->block.next = page->local;
  page->local = head;
}
