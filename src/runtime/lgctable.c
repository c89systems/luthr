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
  view->arraydata = __atomic_load_n(&table->arraydata, __ATOMIC_ACQUIRE);
  view->asize = 0;
  if (view->arraydata != 0 && !(view->arraydata & LUA_RECORD_TAGS))
    view->asize = arrayheader(luaH_array(view))->size;
  view->node = __atomic_load_n(&table->node, __ATOMIC_ACQUIRE);
  *metatable = __atomic_load_n(&table->metatable, __ATOMIC_ACQUIRE);
  return luaH_samelayout(table, *sequence);
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
  if (!luaH_samelayout(table, sequence)) return 0;
  /* Only the claimed marker freezes this epoch. Publishing a strong mode
  ** cannot expose a weak cutoff; weak mode changes retain mutation ownership. */
  luaC_settablestate(table, luaC_weakstamp(g));
  return 1;
}

l_sinline int scanpart (lua_State *L, LuaTraceWork *work, Table *table,
                     const Table *view, unsigned first, unsigned end,
                     int hash, const LuaHashPart *part, int *young) {
  while (first < end) {
    GCObject *edges[TRACE_SLOTS * 2];
    unsigned slots = end - first, count;
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
        !luaH_samelayout(table, sequence))
      return 0;
    if (young != NULL && !*young) {
      unsigned i;
      for (i = 0; i < count; i++) {
        if (!isold(edges[i])) {
          *young = 1;
          break;
        }
      }
    }
    luaC_traceedges(work, edges, count);
    first += slots;
    if (luaW_stopped(&G(L)->world)) return 0;
  }
  return 1;
}

/* A scalar card has no edges for any trace. A young-clean stamp also skips
** old edges for its major epoch. A reference store dirties either proof;
** the existing barriers shade edges if the store races with the skip. */
static int scanarray (lua_State *L, LuaTraceWork *work, Table *table,
                      const Table *view, int *young_edges) {
  global_State *g = G(L);
  uint64_t epoch = 0;
  uint64_t *cards;
  unsigned first;
  if (view->asize < LUA_ARRAY_CARD_MIN)
    return scanpart(L, work, table, view, 0, view->asize, 0, NULL, young_edges);
  if (isold(obj2gco(table)) && (luaC_markstate(g) & LUA_MARK_YOUNG)) {
    epoch = __atomic_load_n(&g->major_completed, __ATOMIC_ACQUIRE) + 1;
    if (epoch >= LUA_ARRAY_CARD_SCANNING) epoch = 0;
  }
  cards = luaH_arraycards(view);
  for (first = 0; first < view->asize; first += LUA_ARRAY_CARD_SLOTS) {
    uint64_t *card = &cards[first / LUA_ARRAY_CARD_SLOTS];
    uint64_t stamp = __atomic_load_n(card, __ATOMIC_ACQUIRE);
    unsigned end = first + LUA_ARRAY_CARD_SLOTS;
    int young = 0, clearing, scanned;
    if (stamp == LUA_ARRAY_CARD_SCALAR || (epoch != 0 && stamp == epoch)) {
      if (luaW_stopped(&g->world)) return 0;
      continue;
    }
    if (end > view->asize) end = view->asize;
    if (epoch == 0) {
      if (!scanpart(L, work, table, view, first, end, 0, NULL, young_edges))
        return 0;
      continue;
    }
    /* Acquire the dirty publication before reading the slots. Publish the
    ** proof only if no writer dirtied the card during this scan. */
    clearing = stamp != LUA_ARRAY_CARD_SCANNING &&
      __atomic_compare_exchange_n(card, &stamp, LUA_ARRAY_CARD_SCANNING, 0,
                                  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    scanned = scanpart(L, work, table, view, first, end, 0, NULL,
                       clearing ? &young : NULL);
    if (clearing) {
      uint64_t expected = LUA_ARRAY_CARD_SCANNING;
      __atomic_compare_exchange_n(card, &expected, scanned && !young ? epoch : 0,
                                  0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
    if (!scanned) return 0;
  }
  return 1;
}

int luaC_scanstrong (lua_State *L, LuaTraceWork *work, Table *table) {
  Table view, *metatable;
  Table *keys;
  LuaHashDirectory *directory;
  unsigned i, parts;
  lu_mem bytes;
  uint64_t sequence;
  int mode, young = 0;
  int *young_edges = NULL;
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
  keys = luaH_recordkeys(view.node);
  if (keys != NULL) {
    GCObject *edges[LUA_RECORD_MAX_FIELDS + 2];
    Value *values = luaH_recordvalues(table);
    unsigned count = 1;
    edges[0] = obj2gco(keys);
    if (metatable != NULL) edges[count++] = obj2gco(metatable);
    for (i = 0; i < keys->asize; i++) {
      lu_byte tag = luaH_recordtag(view.arraydata, i);
      if (tag & BIT_ISCOLLECTABLE)
        __atomic_load(&values[i].gc, &edges[count++], __ATOMIC_ACQUIRE);
    }
    if (!luaH_samelayout(table, sequence))
      return 0;
    luaC_traceedges(work, edges, count);
    work->marked += sizeof(*table) + table->inline_bytes;
    luaC_finishstrong(work, obj2gco(table), 0);
    return 1;
  }
  directory = luaH_hashdir(view.node);
  if (metatable != NULL) {
    GCObject *edge = obj2gco(metatable);
    luaC_traceedges(work, &edge, 1);
  }
  else if (directory == NULL && getage(obj2gco(table)) == G_SURVIVAL &&
           (luaC_markstate(G(L)) & LUA_MARK_YOUNG))
    young_edges = &young;
  if (!scanarray(L, work, table, &view, young_edges)) return 0;
  parts = directory != NULL ? LUA_HASH_PARTS : 1;
  bytes = luaH_size(table, &view);
  for (i = directory != NULL ? 1u : 0u; i < parts; ++i) {
    const LuaHashPart *part = directory != NULL ? luaH_part(directory, i) : NULL;
    if (i != 0) {
      Value *array = __atomic_load_n(&part->array, __ATOMIC_ACQUIRE);
      view.arraydata = cast(uintptr_t, array);
      view.node = __atomic_load_n(&part->nodes, __ATOMIC_ACQUIRE);
      if (!scanpart(L, work, table, &view, 0, luaH_arraycapacity(array),
                    0, part, young_edges)) return 0;
    }
    if (!scanpart(L, work, table, &view, 0, luaH_nodesize(view.node), 1,
                  part, young_edges)) return 0;
  }
  work->marked += cast(l_mem, bytes);
  /* A structural move can carry an existing young edge behind the scan.
  ** The same image must survive all chunks. A young write vetoes the proof
  ** in the age byte even if the write preserves the layout and tag. */
  luaC_finishstrong(work, obj2gco(table), young_edges != NULL && !young &&
                    luaH_samelayout(table, sequence));
  return 1;
}

int luaC_tablesize (Table *table, l_mem *bytes) {
  Table view, *metatable;
  uint64_t sequence;
  if (!snapshot(table, &view, &metatable, &sequence)) return 0;
  *bytes = cast(l_mem, luaH_size(table, &view));
  return 1;
}
