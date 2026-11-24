/* Iteration over versioned table storage. See lua.h for license. */
#define LUA_CORE
#include "lprefix.h"
#include "ltableaccess.h"

static int emitnext (lua_State *L, TValue *key, TValue *result,
                     const TValue *value) {
  setobj(L, result, value);
  luaC_tracevalue(L, key);
  luaC_tracevalue(L, result);
  return 1;
}

/* The managed reader's storage epoch retains the shape. Sample the layout
** before reading its tags and payload, and validate before exposing either. */
static int nextrecordview (lua_State *L, Table *table, TValue *key,
                           TValue *result) {
  uint64_t version = __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE);
  Table *keys;
  uintptr_t tags;
  unsigned index = 0;
  TValue value;
  if ((version & 1) || !luaH_strongaccess(L, table)) return -1;
  keys = luaH_recordkeys(__atomic_load_n(&table->node, __ATOMIC_ACQUIRE));
  if (keys == NULL) return -1;
  tags = __atomic_load_n(&table->arraydata, __ATOMIC_ACQUIRE);
  if (!(tags & LUA_RECORD_TAGS)) return -1;
  if (!ttisnil(key)) {
    if (!ttisshrstring(key)) return -1;
    index = luaH_recordindex(keys, tsvalue(key));
    if (index == keys->asize || luaH_recordtag(tags, index) == LUA_VABSTKEY)
      return -1;
    index++;
  }
  index = luaH_nextrecordslot(keys, tags, index);
  if (index == keys->asize)
    return luaH_samelayout(table, version) ? 0 : -1;
  __atomic_load(luaH_recordvalues(table) + index, &val_(&value), __ATOMIC_ACQUIRE);
  if (!luaH_samelayout(table, version)) return -1;
  settt_(&value, luaH_recordtag(tags, index));
  arr2obj(keys, index, key);
  return emitnext(L, key, result, &value);
}

static int cursorvalid (const LuaHashCursor *cursor) {
  return cursor == NULL || cursor->part == NULL ||
         __atomic_load_n(&cursor->part->version, __ATOMIC_ACQUIRE) ==
           cursor->version;
}

/* Each step borrows storage only within the read scope. A sparse table
** can exhaust the scan budget and use the ordinary owned traversal. */
static int nextarrayview (lua_State *L, Table *t, TValue *key, TValue *result,
                         Value *array,
                         unsigned index, unsigned size, uint64_t layout,
                         unsigned *budget, const LuaHashCursor *cursor) {
  unsigned count = size - index < *budget ? size - index : *budget;
  unsigned end = index + count;
  uint64_t rows = cursor == NULL ? layout : 0;
  for (; index < end; index++) {
    const lu_byte *tag = arraytag(array, index);
    TValue value;
    if (tagisempty(__atomic_load_n(tag, __ATOMIC_ACQUIRE))) continue;
    if (!luaH_readvalue(G(L), arrayval(array, index), tag, &value, rows) ||
        isempty(&value) || !cursorvalid(cursor) ||
        !luaH_samelayout(t, layout))
      return -1;
    setivalue(key, cursor == NULL ? cast(lua_Integer, index) + 1 :
                         luaH_indexfrompart(cursor->partition, index + 1));
    return emitnext(L, key, result, &value);
  }
  *budget -= count;
  return index == size && cursorvalid(cursor) ? 0 : -1;
}

static int nexthashview (lua_State *L, Table *t, TValue *key, TValue *result,
                         const LuaHashCursor *cursor, uint64_t layout,
                         unsigned *budget) {
  unsigned size = luaH_nodesize(cursor->nodes);
  unsigned index = cursor->index;
  unsigned capacity = luaH_arraycapacity(cursor->array);
  unsigned count, end;
  uint64_t rows = cursor->part == NULL ? layout : 0;
  if (index < capacity) {
    int more = nextarrayview(L, t, key, result, cursor->array, index,
                             capacity, layout, budget, cursor);
    if (more != 0) return more;
    index = capacity;
  }
  index -= capacity;
  count = size - index < *budget ? size - index : *budget;
  end = index + count;
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
        !luaH_samelayout(t, layout))
      return -1;
    settt_(&nextkey, keytag);
    setobj(L, key, &nextkey);
    return emitnext(L, key, result, &value);
  }
  *budget -= count;
  return index == size && cursorvalid(cursor) ? 0 : -1;
}

static int __attribute__((noinline)) nexttableview (lua_State *L, Table *t,
                                                    TValue *key, TValue *result) {
  uint64_t layout = __atomic_load_n(&t->sequence, __ATOMIC_ACQUIRE);
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
  index = ttisinteger(key) ? luaH_indexin(ivalue(key), size) : 0;
  cursor.nodes = image;
  if (index != 0 || ttisnil(key)) {
    if (index < size) {
      int more = nextarrayview(L, t, key, result, array, index, size, layout,
                               &budget, NULL);
      if (more != 0) return more;
    }
    if (dir != NULL && !luaH_cursorpart(&cursor, dir)) return -1;
  }
  else if (!luaH_hashresume(t, dir, key, layout, &cursor)) return -1;
  for (;;) {
    int more = nexthashview(L, t, key, result, &cursor, layout, &budget);
    if (more != 0) return more;
    /* A deletion can only remove entries from an already scanned partition.
    ** Insertion during traversal has the ordinary 'next' restriction. */
    if (dir == NULL || ++cursor.partition == LUA_HASH_PARTS)
      return luaH_samelayout(t, layout) ? 0 : -1;
    cursor.index = 0;
    if (!luaH_cursorpart(&cursor, dir)) return -1;
  }
}

/* A failed snapshot or the end of iteration leaves both slots unchanged. */
int luaH_trynext (lua_State *L, Table *table, TValue *key, TValue *result) {
  Node *image = __atomic_load_n(&table->node, __ATOMIC_ACQUIRE);
  return luaH_recordkeys(image) != NULL ? nextrecordview(L, table, key, result)
                                       : nexttableview(L, table, key, result);
}

int luaH_next (lua_State *L, Table *table, StkId key) {
  int more = luaH_trynext(L, table, s2v(key), s2v(key + 1));
  if (more >= 0) return more;
  return luaH_nextlocked(L, table, key);
}
