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
  uint64_t state = luaC_tablestate(table);
  if (luaC_weakvalid(g, state))
    return luaC_weakmode(state) == 0 ? 1 : -1;
  if (metatable != NULL) {
    TValue mode;
    if (!luaH_readshortstr(g, NULL, metatable, g->tmname[TM_MODE], &mode)) {
      return luaA_lockowner(&metatable->access) == luaX_token(L) ? -1 : 0;
    }
    if (ttisstring(&mode)) {
      const char *text = getstr(tsvalue(&mode));
      if (strchr(text, 'k') != NULL || strchr(text, 'v') != NULL) return -1;
    }
  }
  if (__atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE) != sequence) return 0;
  /* Only the claimed marker freezes this epoch. Publishing a strong mode
  ** cannot expose a weak cutoff; weak mode changes retain mutation ownership. */
  luaC_settablestate(table, g->weak_stamp);
  return 1;
}

l_sinline int scanpart (lua_State *L, LuaTraceWork *work, Table *table,
                     const Table *view, unsigned size, int hash,
                     const LuaHashPart *part) {
  unsigned first;
  for (first = 0; first < size;) {
    GCObject *edges[TRACE_SLOTS * 2];
    unsigned slots = size - first, count;
    uint64_t sequence = __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE);
    uint64_t revision = part != NULL ?
      __atomic_load_n(&part->version, __ATOMIC_ACQUIRE) : 0;
    if ((sequence | revision) & 1) return 0;
    if (slots > TRACE_SLOTS) slots = TRACE_SLOTS;
    count = luaH_tracechunk(part == NULL && (sequence & LUA_TABLE_SPLIT_VALUES) ?
                            G(L) : NULL,
                            view, first, slots, hash, edges);
    /* Never inspect the target header before the tag/payload pair is valid. */
    if (count == UINT_MAX ||
        (part != NULL &&
         __atomic_load_n(&part->version, __ATOMIC_ACQUIRE) != revision) ||
        __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE) != sequence)
      return 0;
    luaC_traceedges(work, edges, count);
    first += slots;
    if (luaW_stopped(&G(L)->world)) return 0;
  }
  return 1;
}

int luaC_scanstrong (lua_State *L, LuaTraceWork *work, Table *table) {
  Table view, *metatable;
  LuaHashDirectory *directory;
  unsigned i, parts;
  lu_mem bytes;
  uint64_t sequence;
  int mode;
  if (!snapshot(table, &view, &metatable, &sequence)) {
    /* A nested collection can borrow its own suspended native operation.
    ** Its partial construction image is stable under that existing owner;
    ** waiting for this execution's odd sequence would wait on ourselves. */
    if (luaA_lockowner(&table->access) == luaX_token(L))
      return -1;
    return 0;
  }
  mode = strongmode(L, table, metatable, sequence);
  if (mode != 1) return mode;
  if (metatable != NULL) {
    GCObject *edge = obj2gco(metatable);
    luaC_traceedges(work, &edge, 1);
  }
  if (!scanpart(L, work, table, &view, view.asize, 0, NULL)) return 0;
  directory = luaH_hashdir(view.node);
  parts = luaH_hashparts(&view);
  bytes = luaH_size(table, &view);
  for (i = 0; i < parts; ++i) {
    const LuaHashPart *part = directory != NULL ? &directory->parts[i] : NULL;
    if (part != NULL)
      view.node = __atomic_load_n(&part->nodes, __ATOMIC_ACQUIRE);
    if (!scanpart(L, work, table, &view, luaH_nodesize(view.node), 1,
                  i != 0 ? part : NULL)) return 0;
  }
  work->marked += cast(l_mem, bytes);
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
