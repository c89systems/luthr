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
    luaW_active(L->admission) != 0);
  UNUSED(L);
  _Static_assert(_Alignof(LuaAllocBlock) > LUA_BLOCK_FLAGS, "allocation flags");
  lua_assert((cast(uintptr_t, page) & LUA_BLOCK_FLAGS) == 0);
  for (offset = LUA_ALLOC_PAGE_HEADER;
       offset + stride <= page->bytes; offset += stride) {
    LuaAllocBlock *block = cast(LuaAllocBlock *, cast_charp(page) + offset);
    block->block.page = cast(uintptr_t, page);
    block->block.link.next = page->local;
    page->local = block;
  }
}

void luaMP_collectpage (LuaAllocPage *page) {
  uintptr_t old = __atomic_load_n(&page->remote, __ATOMIC_ACQUIRE);
  LuaAllocBlock *head, *tail;
  unsigned returned = 0;
  do {
    head = cast(LuaAllocBlock *, old & ~LUA_PAGE_NOTIFIED);
    if (head == NULL) return;
  } while (!__atomic_compare_exchange_n(&page->remote, &old,
              old & LUA_PAGE_NOTIFIED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
  tail = head;
  if (head == NULL) return;
  for (;;) {
    returned++;
    if (tail->block.link.next == NULL) break;
    tail = tail->block.link.next;
  }
  lua_assert(page->used >= returned);
  page->used -= returned;
  tail->block.link.next = page->local;
  page->local = head;
}

/* A notification owns one return outside the page's remote list. That block
** remains counted as allocated, pinning the page until its notice is claimed.
** Publication is the producer's LAST page access, so a consumer can retire an
** empty page without joining every mutator or observing a half-done publisher.
*/
void luaMP_publish (global_State *g, LuaAllocPage *page) {
  LuaAllocPage *head = __atomic_load_n(&g->allocation_dirty, __ATOMIC_RELAXED);
  do { page->dirty_next = head; }
  while (!__atomic_compare_exchange_n(&g->allocation_dirty, &head, page, 0,
                                       __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

void luaMP_notify (global_State *g, LuaAllocPage *page) {
  uintptr_t old = __atomic_load_n(&page->remote, __ATOMIC_RELAXED);
  do {
    if (old & LUA_PAGE_NOTIFIED) return;
  } while (!__atomic_compare_exchange_n(&page->remote, &old,
              old | LUA_PAGE_NOTIFIED, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
  page->notified = NULL; /* an allocation/cache handoff, not a returned block */
  luaMP_publish(g, page);
}

void luaMP_return (global_State *g, LuaAllocBlock *block) {
  LuaAllocPage *page = luaMP_page(block);
  uintptr_t old = __atomic_load_n(&page->remote, __ATOMIC_RELAXED);
  for (;;) {
    if (!(old & LUA_PAGE_NOTIFIED)) {
      if (__atomic_compare_exchange_n(&page->remote, &old,
            old | LUA_PAGE_NOTIFIED, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        page->notified = block;
        luaMP_publish(g, page);
        return; /* never touch page after publishing the lifetime pin */
      }
    }
    else {
      block->block.link.next = cast(LuaAllocBlock *, old & ~LUA_PAGE_NOTIFIED);
      if (__atomic_compare_exchange_n(&page->remote, &old,
            cast(uintptr_t, block) | LUA_PAGE_NOTIFIED, 0,
            __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        return; /* ordinary remote publication is also the last page access */
    }
  }
}

void luaMP_resetnotice (LuaAllocPage *page) {
  /* Claim the held return before this reset. Later producers must publish a
  ** new notice BEFORE we collect returns, or their final return could be lost.
  ** The old held block, plus the pool/cache lease, pins this consumer's work. */
  __atomic_fetch_and(&page->remote, ~LUA_PAGE_NOTIFIED, __ATOMIC_ACQ_REL);
}
