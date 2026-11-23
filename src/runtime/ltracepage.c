/* Page-local table mark queues. See Copyright Notice in lua.h. */
#define LUA_CORE
#include "lprefix.h"
#include "lgc.h"

#define PAGE_TRACE_QUEUED (1u << LUA_PAGE_TRACE_WORDS)

_Static_assert(LUA_ALLOC_PAGE_MAX == (1u << 16), "page slot reciprocal");
_Static_assert(LUA_ALLOC_PAGE_MAX / (sizeof(Table) + sizeof(LuaAllocBlock)) <=
               64 * LUA_PAGE_TRACE_WORDS, "page bitmap covers table slots");

l_sinline unsigned tableslot (const LuaAllocPage *page, const LuaAllocBlock *block) {
  unsigned offset = cast_uint(cast(const char *, block) -
    cast(const char *, page)) - LUA_ALLOC_PAGE_HEADER;
  /* Slot offsets are exact stride multiples below 2^16. Rounding the
  ** reciprocal up therefore introduces less than one slot of error. */
  unsigned slot = (offset * page->trace_scale) >> 16;
  lua_assert(offset < LUA_ALLOC_PAGE_MAX &&
    slot * (page->blockbytes + sizeof(*block)) == offset);
  return slot;
}

/* Slot bits are installed before the ready word. A producer observing the
** queued bit leaves announcement to the owner of that page notice. */
static void queue (global_State *g, LuaAllocPage *page, unsigned word,
                   uint64_t bits) {
  unsigned pending;
  lua_assert(bits != 0 && word < LUA_PAGE_TRACE_WORDS);
  __atomic_fetch_or(&page->trace_slots[word], bits, __ATOMIC_RELAXED);
  pending = __atomic_fetch_or(&page->trace_pending,
                              PAGE_TRACE_QUEUED | (1u << word),
                              __ATOMIC_ACQ_REL);
  if (!(pending & PAGE_TRACE_QUEUED)) {
    LuaAllocPage *head = __atomic_load_n(&g->trace_page_incoming, __ATOMIC_RELAXED);
    do {
      page->trace_next = head;
    } while (!__atomic_compare_exchange_n(&g->trace_page_incoming, &head, page,
                                          0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
  }
}

void luaC_queuetracepage (global_State *g, GCObject *object) {
  LuaAllocBlock *block = cast(LuaAllocBlock *, object) - 1;
  LuaAllocPage *page = luaMP_page(block);
  unsigned slot = tableslot(page, block);
  queue(g, page, slot / 64, UINT64_C(1) << (slot % 64));
}

void luaC_flushtracepage (LuaTraceWork *work) {
  if (work->staged_page != NULL) {
    queue(work->global, work->staged_page, work->staged_word, work->staged_bits);
    work->staged_page = NULL;
  }
}

void luaC_stagetracepage (LuaTraceWork *work, GCObject *object) {
  LuaAllocBlock *block = cast(LuaAllocBlock *, object) - 1;
  LuaAllocPage *page = luaMP_page(block);
  unsigned slot = tableslot(page, block);
  unsigned word = slot / 64;
  if (page != work->staged_page || word != work->staged_word) {
    luaC_flushtracepage(work);
    work->staged_page = page;
    work->staged_word = word;
    work->staged_bits = 0;
  }
  work->staged_bits |= UINT64_C(1) << (slot % 64);
}

l_sinline void retirenotice (global_State *g, LuaAllocPage *page) {
  unsigned pending = PAGE_TRACE_QUEUED;
  /* A producer may reuse the link as soon as this notice is retired. */
  LuaAllocPage *next = page->trace_next;
  if (__atomic_compare_exchange_n(&page->trace_pending, &pending, 0, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    g->trace_pages = next;
}

/* trace_mutex owns the imported list. Clear a word's summary before taking
** its bits so a racing producer either contributes here or advertises it
** again. Retire a notice only if a producer has not published another word. */
void luaC_claimtracepage (LuaTraceWork *work, unsigned limit) {
  global_State *g = work->global;
  LuaAllocPage *page;
  lua_assert(limit <= 64 && work->count < limit);
  if (g->trace_pages == NULL &&
      __atomic_load_n(&g->trace_page_incoming, __ATOMIC_RELAXED) != NULL)
    g->trace_pages = __atomic_exchange_n(&g->trace_page_incoming, NULL,
                                          __ATOMIC_ACQUIRE);
  while ((page = g->trace_pages) != NULL && work->count < limit &&
         work->page_count < LUA_TRACE_PAGE_SLICES) {
    unsigned pending = __atomic_load_n(&page->trace_pending, __ATOMIC_ACQUIRE);
    unsigned words = pending & ~PAGE_TRACE_QUEUED;
    lua_assert(pending & PAGE_TRACE_QUEUED);
    if (words != 0) {
      unsigned word = cast_uint(__builtin_ctz(words));
      unsigned remaining = limit - work->count;
      uint64_t bits;
      LuaTracePageSlice *slice;
      __atomic_fetch_and(&page->trace_pending, ~(1u << word), __ATOMIC_ACQ_REL);
      bits = __atomic_exchange_n(&page->trace_slots[word], 0, __ATOMIC_ACQUIRE);
      if (bits == 0) continue;
      if (remaining < 64) {
        unsigned start = cast_uint(__builtin_ctzll(bits));
        uint64_t mask = ((UINT64_C(1) << remaining) - 1) << start;
        if (bits & ~mask) queue(g, page, word, bits & ~mask);
        bits &= mask;
      }
      if ((words & (words - 1)) == 0) retirenotice(g, page);
      slice = &work->pages[work->page_count++];
      slice->page = page;
      slice->word = word;
      slice->bits = bits;
      work->count += cast_uint(__builtin_popcountll(bits));
    }
    else retirenotice(g, page);
  }
}
