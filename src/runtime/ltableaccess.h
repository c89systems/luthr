/* Element versions and layout admission. See lua.h for license. */
#ifndef ltableaccess_h
#define ltableaccess_h

#include "lstate.h"
#include "lgc.h"

l_sinline LuaTableWriter *luaH_writer (global_State *g, const Value *slot) {
  uintptr_t address = cast(uintptr_t, slot);
  return &g->table_writers[((address >> 3) ^ (address >> 12)) &
                           (LUA_TABLE_WRITERS - 1)];
}

l_sinline int luaH_readvalue (global_State *g, const Value *payload,
                              const lu_byte *tag, TValue *value) {
  LuaTableWriter *writer = luaH_writer(g, payload);
  uint64_t before = __atomic_load_n(&writer->sequence, __ATOMIC_ACQUIRE);
  if (before & 1) return 0;
  __atomic_load(payload, &value->value_, __ATOMIC_ACQUIRE);
  value->tt_ = __atomic_load_n(tag, __ATOMIC_ACQUIRE);
  return __atomic_load_n(&writer->sequence, __ATOMIC_ACQUIRE) == before;
}

l_sinline int luaH_strongaccess (lua_State *L, Table *table) {
  if (!luaC_tracing(L)) return 1;
  if (__atomic_load_n(&table->weak_epoch, __ATOMIC_ACQUIRE) == G(L)->trace_epoch)
    return __atomic_load_n(&table->gcweak, __ATOMIC_ACQUIRE) == 0;
  return __atomic_load_n(&table->metatable, __ATOMIC_ACQUIRE) == NULL;
}

LUAI_FUNC LuaTableWriter *luaH_claimwriter (lua_State *, Table *, Value *, uint64_t);
LUAI_FUNC void luaH_releasewriter (lua_State *, Table *, LuaTableWriter *);
LUAI_FUNC int luaH_quiescewriters (lua_State *, Table *, int);
LUAI_FUNC void luaH_elementstore (lua_State *, Table *, Value *, lu_byte *,
                                 const TValue *);
#endif
