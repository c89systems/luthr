/* Iteration over versioned table storage. See lua.h for license. */
#define LUA_CORE
#include "lprefix.h"
#include "ltableaccess.h"

static int emitnext (lua_State *L, StkId key, const TValue *value) {
  setobj2s(L, key + 1, value);
  luaC_tracevalue(L, s2v(key));
  luaC_tracevalue(L, s2v(key + 1));
  return 1;
}

/* The managed reader's storage epoch retains the shape. Sample the layout
** before reading its tags and payload, and validate before exposing either. */
static int nextrecordview (lua_State *L, Table *table, StkId key) {
  uint64_t version = __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE);
  const TValue *previous = s2v(key);
  Table *keys;
  uintptr_t tags;
  unsigned index = 0;
  TValue value;
  if ((version & 1) || !luaH_strongaccess(L, table)) return -1;
  keys = luaH_recordkeys(__atomic_load_n(&table->node, __ATOMIC_ACQUIRE));
  if (keys == NULL) return -1;
  tags = __atomic_load_n(&table->arraydata, __ATOMIC_ACQUIRE);
  if (!(tags & LUA_RECORD_TAGS)) return -1;
  if (!ttisnil(previous)) {
    if (!ttisshrstring(previous)) return -1;
    index = luaH_recordindex(keys, tsvalue(previous));
    if (index == keys->asize || luaH_recordtag(tags, index) == LUA_VABSTKEY)
      return -1;
    index++;
  }
  index = luaH_nextrecordslot(keys, tags, index);
  if (index == keys->asize)
    return __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE) == version ? 0 : -1;
  __atomic_load(luaH_recordvalues(table) + index, &val_(&value), __ATOMIC_ACQUIRE);
  if (__atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE) != version) return -1;
  settt_(&value, luaH_recordtag(tags, index));
  arr2obj(keys, index, s2v(key));
  return emitnext(L, key, &value);
}

/* Each step borrows storage only within the read scope. A sparse table
** can exhaust the scan budget and use the ordinary owned traversal. */
static int nextarrayview (lua_State *L, Table *t, StkId key, Value *array,
                         unsigned index, unsigned size, uint64_t layout,
                         unsigned *budget) {
  unsigned count = size - index < *budget ? size - index : *budget;
  unsigned end = index + count;
  for (; index < end; index++) {
    const lu_byte *tag = arraytag(array, index);
    TValue value;
    if (tagisempty(__atomic_load_n(tag, __ATOMIC_ACQUIRE))) continue;
    if (!luaH_readvalue(G(L), array - index - 1, tag, &value, layout) ||
        isempty(&value) ||
        __atomic_load_n(&t->sequence, __ATOMIC_ACQUIRE) != layout)
      return -1;
    setivalue(s2v(key), cast(lua_Integer, index) + 1);
    return emitnext(L, key, &value);
  }
  *budget -= count;
  return index == size ? 0 : -1;
}

static int cursorvalid (const LuaHashCursor *cursor) {
  return cursor->part == NULL ||
         __atomic_load_n(&cursor->part->version, __ATOMIC_ACQUIRE) ==
           cursor->version;
}

static int nexthashview (lua_State *L, Table *t, StkId key,
                         const LuaHashCursor *cursor, uint64_t layout,
                         unsigned *budget) {
  unsigned size = luaH_nodesize(cursor->nodes);
  unsigned index = cursor->index;
  unsigned count = size - index < *budget ? size - index : *budget;
  unsigned end = index + count;
  uint64_t rows = cursor->part == NULL ? layout : 0;
  for (; index < end; index++) {
    const Node *node = cursor->nodes + index;
    TValue value, nextkey;
    lu_byte keytag;
    if (tagisempty(__atomic_load_n(&node->u.tt_, __ATOMIC_ACQUIRE))) continue;
    if (!luaH_readvalue(G(L), &node->u.value_, &node->u.tt_, &value, rows) ||
        isempty(&value)) return -1;
    keytag = __atomic_load_n(&node->u.key_tt, __ATOMIC_ACQUIRE);
    __atomic_load(&node->u.key_val, &val_(&nextkey), __ATOMIC_ACQUIRE);
    if (keytag == LUA_TDEADKEY || keytag == LUA_VNIL || !cursorvalid(cursor) ||
        __atomic_load_n(&t->sequence, __ATOMIC_ACQUIRE) != layout)
      return -1;
    settt_(&nextkey, keytag);
    setobj2s(L, key, &nextkey);
    return emitnext(L, key, &value);
  }
  *budget -= count;
  return index == size && cursorvalid(cursor) ? 0 : -1;
}

static int __attribute__((noinline)) nexttableview (lua_State *L, Table *t,
                                                    StkId key) {
  uint64_t layout = __atomic_load_n(&t->sequence, __ATOMIC_ACQUIRE);
  const TValue *previous = s2v(key);
  Node *image;
  LuaHashDirectory *dir;
  LuaHashCursor cursor = {0};
  Value *array;
  unsigned size, index;
  unsigned budget = LUA_NEXT_PROBE_LIMIT;
  if ((layout & 1) || !luaH_strongaccess(L, t)) return -1;
  image = __atomic_load_n(&t->node, __ATOMIC_ACQUIRE);
  if (luaH_recordkeys(image) != NULL) return -1;
  dir = luaH_hashdir(image);
  array = luaH_readarray(t);
  size = array != NULL ? arrayheader(array)->size : 0;
  index = ttisinteger(previous) ? luaH_indexin(ivalue(previous), size) : 0;
  cursor.nodes = image;
  if (index != 0 || ttisnil(previous)) {
    if (index < size) {
      int more = nextarrayview(L, t, key, array, index, size, layout, &budget);
      if (more != 0) return more;
    }
    if (dir != NULL && !luaH_cursorpart(&cursor, dir)) return -1;
  }
  else if (!luaH_hashresume(t, dir, previous, layout, &cursor)) return -1;
  for (;;) {
    int more = nexthashview(L, t, key, &cursor, layout, &budget);
    if (more != 0) return more;
    /* A deletion can only remove entries from an already scanned partition.
    ** Insertion during traversal has the ordinary 'next' restriction. */
    if (dir == NULL || ++cursor.partition == LUA_HASH_PARTS)
      return __atomic_load_n(&t->sequence, __ATOMIC_ACQUIRE) == layout ? 0 : -1;
    cursor.index = 0;
    if (!luaH_cursorpart(&cursor, dir)) return -1;
  }
}

int luaH_next (lua_State *L, Table *table, StkId key) {
  Node *image = __atomic_load_n(&table->node, __ATOMIC_ACQUIRE);
  int more = luaH_recordkeys(image) != NULL ? nextrecordview(L, table, key)
                                           : nexttableview(L, table, key);
  if (more >= 0) return more;
  return luaH_nextlocked(L, table, key);
}
