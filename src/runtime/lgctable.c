/* Read-only strong-table marking. See lua.h for license. */
#define LUA_CORE
#include "lprefix.h"
#include <string.h>
#include <limits.h>
#include "lgc.h"
#include "ltable.h"

/* One chunk uses at most 512 bytes of pointer snapshots on a carrier stack.
** There is no retry loop, allocation, safepoint or object ownership here.
** Admission retains old buffers; sequence validation protects split values.
** Structural moves shade their old edges before moving them across chunks. */
#define TRACE_SLOTS 32

static int snapshot (Table *table, Table *view, Table **metatable,
                     uint64_t *sequence) {
  *sequence = __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE);
  if (*sequence & 1) return 0;
  view->array = __atomic_load_n(&table->array, __ATOMIC_ACQUIRE);
  view->asize = __atomic_load_n(&table->asize, __ATOMIC_ACQUIRE);
  view->node = __atomic_load_n(&table->node, __ATOMIC_ACQUIRE);
  *metatable = __atomic_load_n(&table->metatable, __ATOMIC_ACQUIRE);
  return __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE) == *sequence;
}

static int strongmode (lua_State *L, Table *table, Table *metatable,
                       uint64_t sequence) {
  global_State *g = G(L);
  if (__atomic_load_n(&table->weak_epoch, __ATOMIC_ACQUIRE) == g->trace_epoch)
    return __atomic_load_n(&table->gcweak, __ATOMIC_ACQUIRE) == 0 ? 1 : -1;
  if (metatable != NULL) {
    TValue mode;
    if (!luaH_readshortstr(g, NULL, metatable, g->tmname[TM_MODE], &mode)) {
      return __atomic_load_n(&metatable->access.owner, __ATOMIC_ACQUIRE) ==
             luaX_token(L) ? -1 : 0;
    }
    if (ttisstring(&mode)) {
      const char *text = getstr(tsvalue(&mode));
      if (strchr(text, 'k') != NULL || strchr(text, 'v') != NULL) return -1;
    }
  }
  if (__atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE) != sequence) return 0;
  /* Only the claimed marker freezes this epoch. Publishing a strong mode
  ** cannot expose a weak cutoff; weak mode changes retain mutation ownership. */
  __atomic_store_n(&table->gcweak, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&table->weak_epoch, g->trace_epoch, __ATOMIC_RELEASE);
  return 1;
}

l_sinline int scanpart (lua_State *L, LuaTraceWork *work, Table *table,
                     const Table *view, unsigned size, int hash) {
  unsigned first;
  for (first = 0; first < size;) {
    GCObject *edges[TRACE_SLOTS * 2];
    unsigned slots = size - first, count;
    uint64_t sequence = __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE);
    uintptr_t sharing;
    if (sequence & 1) return 0;
    sharing = __atomic_load_n(&table->sharing, __ATOMIC_ACQUIRE);
    if (slots > TRACE_SLOTS) slots = TRACE_SLOTS;
    count = luaH_tracechunk(sharing & LUA_OBJECT_SHARED ? G(L) : NULL,
                            view, first, slots, hash, edges);
    /* Never inspect the target header before the tag/payload pair is valid. */
    if (count == UINT_MAX ||
        __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE) != sequence ||
        (!(sharing & LUA_OBJECT_SHARED) &&
         (__atomic_load_n(&table->sharing, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED)))
      return 0;
    luaC_traceedges(work, edges, count);
    first += slots;
    if (__atomic_load_n(&G(L)->world.stopped, __ATOMIC_SEQ_CST)) return 0;
  }
  return 1;
}

int luaC_scanstrong (lua_State *L, LuaTraceWork *work, Table *table) {
  Table view, *metatable;
  uint64_t sequence;
  int mode;
  if (!snapshot(table, &view, &metatable, &sequence)) {
    /* A nested collection can borrow its own suspended native operation.
    ** Its partial construction image is stable under that existing owner;
    ** waiting for this execution's odd sequence would wait on ourselves. */
    if (__atomic_load_n(&table->access.owner, __ATOMIC_ACQUIRE) == luaX_token(L))
      return -1;
    return 0;
  }
  mode = strongmode(L, table, metatable, sequence);
  if (mode != 1) return mode;
  if (metatable != NULL) {
    GCObject *edge = obj2gco(metatable);
    luaC_traceedges(work, &edge, 1);
  }
  if (!scanpart(L, work, table, &view, view.asize, 0) ||
      (!isdummy(&view) &&
       !scanpart(L, work, table, &view, sizenode(&view), 1))) return 0;
  work->marked += cast(l_mem, luaH_size(table, &view));
  luaC_finishstrong(work, obj2gco(table));
  return 1;
}

int luaC_tablesize (Table *table, l_mem *bytes) {
  Table view, *metatable;
  uint64_t sequence;
  if (!snapshot(table, &view, &metatable, &sequence)) return 0;
  *bytes = cast(l_mem, luaH_size(table, &view));
  return 1;
}
