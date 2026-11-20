/* Element versions and layout admission. See lua.h for license. */
#ifndef ltableaccess_h
#define ltableaccess_h

#include "lstate.h"
#include "lgc.h"
#include "ltable.h"

l_sinline LuaTableWriter *luaH_writer (global_State *g, const Value *slot) {
  uintptr_t address = cast(uintptr_t, slot);
  return &g->table_writers[((address >> 3) ^ (address >> 12)) &
                           (LUA_TABLE_WRITERS - 1)];
}

l_sinline int luaH_readvalue (global_State *g, const Value *payload,
                              const lu_byte *tag, TValue *value, uint64_t layout) {
  LuaTableWriter *writer = NULL;
  uint64_t before = 0;
  if (layout & LUA_TABLE_SPLIT_VALUES) {
    writer = luaH_writer(g, payload);
    before = __atomic_load_n(&writer->sequence, __ATOMIC_ACQUIRE);
    /* A busy row need only stop a read of the element it owns. The second
    ** sequence check validates that identity against reuse of the row. */
    if ((before & 1) &&
        __atomic_load_n(&writer->slot, __ATOMIC_ACQUIRE) == cast(uintptr_t, payload))
      return 0;
  }
  __atomic_load(payload, &value->value_, __ATOMIC_ACQUIRE);
  value->tt_ = __atomic_load_n(tag, __ATOMIC_ACQUIRE);
  return writer == NULL ||
         __atomic_load_n(&writer->sequence, __ATOMIC_ACQUIRE) == before;
}

l_sinline int luaH_strongaccess (lua_State *L, Table *table) {
  return !luaC_tracing(L) || luaC_strongtable(G(L), table);
}

LUAI_FUNC LuaTableWriter *luaH_claimwriter (lua_State *, Table *, Value *, uint64_t);
LUAI_FUNC int luaH_beginsplit (Table *, LuaTableWriter *, Value *, uint64_t);
LUAI_FUNC void luaH_releasewriter (lua_State *, Table *, LuaTableWriter *);
/* One grants admission, zero rejects the layout, and minus one means busy. */
LUAI_FUNC int luaH_claimpart (lua_State *, Table *, LuaHashPart *, uint64_t);
LUAI_FUNC void luaH_releasepart (lua_State *, Table *, LuaHashPart *);
LUAI_FUNC void luaH_waitpart (lua_State *, Table *, LuaHashPart *);
LUAI_FUNC int luaH_quiescewriters (lua_State *, Table *, int);
/* Inputs remain rooted; NULL drains all rows, otherwise wait for one row. */
LUAI_FUNC void luaH_waitwriters (lua_State *, Table *, LuaTableWriter *);
LUAI_FUNC int luaH_waitlayout (lua_State *, Table *);
LUAI_FUNC void luaH_elementstore (lua_State *, Table *, Value *, lu_byte *,
                                 const TValue *);
#endif
