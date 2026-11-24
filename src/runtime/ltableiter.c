/* Iteration over versioned compact record storage. See lua.h for license. */
#define LUA_CORE
#include "lprefix.h"
#include "ltableaccess.h"

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
  setobj2s(L, key + 1, &value);
  luaC_tracevalue(L, s2v(key));
  luaC_tracevalue(L, s2v(key + 1));
  return 1;
}

int luaH_next (lua_State *L, Table *table, StkId key) {
  if (luaH_recordkeys(__atomic_load_n(&table->node, __ATOMIC_ACQUIRE)) != NULL) {
    int more = nextrecordview(L, table, key);
    if (more >= 0) return more;
  }
  return luaH_nextlocked(L, table, key);
}
