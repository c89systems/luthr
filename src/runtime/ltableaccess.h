/* Element versions and layout admission. See lua.h for license. */
#ifndef ltableaccess_h
#define ltableaccess_h

#include "lstate.h"
#include "lgc.h"
#include "laccess.h"
#include "ltable.h"

LUAI_DDEC(const TValue luaH_absentkey;)

/* Shared and possibly weak lookups retain their checked snapshots. */
LUAI_FUNC int luaH_trygetkey (lua_State *, Table *, const TValue *, TValue *,
                             lu_byte *);
LUAI_FUNC int luaH_trygetsharedint (lua_State *, Table *, lua_Integer,
                                    TValue *, lu_byte *);
LUAI_FUNC int luaH_trygetsharedshortstr (lua_State *, Table *, TString *,
                                         TValue *, lu_byte *);
LUAI_FUNC lu_byte luaH_getinthash (Table *, lua_Integer, TValue *);

l_sinline unsigned luaH_indexin (lua_Integer key, unsigned limit) {
  return (l_castS2U(key) - 1u < limit) ? cast_uint(key) : 0;
}

l_sinline Node *luaH_shortposition (Node *nodes, TString *key) {
  return nodes + lmod(key->hash, twoto(nodes->u.lsize));
}

l_sinline const TValue *luaH_shortslot (Node *node, TString *key) {
  lua_assert(strisshr(key));
  for (;;) {
    int next;
    if (keyisshrstr(node) && keystrval(node) == key) return gval(node);
    next = gnext(node);
    if (next == 0) return &luaH_absentkey;
    node += next;
  }
}

l_sinline lu_byte luaH_hashvalue (const TValue *value, TValue *result) {
  if (!ttisnil(value)) setobj(cast(lua_State *, NULL), result, value);
  return ttypetag(value);
}

l_sinline lu_byte luaH_arrayvalue (Table *table, unsigned index, TValue *result) {
  lu_byte tag = *getArrTag(table, index);
  if (!tagisempty(tag)) farr2val(table, index, tag, result);
  return tag;
}

/* A frozen strong table's write barriers retain its edges. Until the marker
** freezes the mode, an owner can still attach a weak metatable this cycle. */
l_sinline lu_byte luaH_privatevalue (lua_State *L, Table *table, TValue *result,
                                      lu_byte tag) {
  if (!tagisempty(tag)) {
    if (iscollectable(result) && luaC_tracing(L) &&
        !luaC_weakvalid(G(L), luaC_tablestate(table)))
      luaC_traceobject(L, gcvalue(result));
    return tag;
  }
  if (__atomic_load_n(&table->metatable, __ATOMIC_ACQUIRE) != NULL)
    return LUA_VABSTKEY;
  setnilvalue(result);
  return LUA_VNIL;
}

l_sinline int luaH_trygetint (lua_State *L, Table *table, lua_Integer key,
                              TValue *result, lu_byte *tag) {
  unsigned index;
  lu_byte found;
  if (!luaA_localtable(L, table))
    return luaH_trygetsharedint(L, table, key, result, tag);
  index = luaH_indexin(key, table->asize);
  found = index != 0 ? luaH_arrayvalue(table, index - 1, result)
                     : luaH_getinthash(table, key, result);
  *tag = luaH_privatevalue(L, table, result, found);
  return 1;
}

l_sinline int luaH_trygetshortstr (lua_State *L, Table *table, TString *key,
                                   TValue *result, lu_byte *tag) {
  Node *nodes;
  Table *keys;
  lu_byte found;
  if (!luaA_localtable(L, table))
    return luaH_trygetsharedshortstr(L, table, key, result, tag);
  nodes = table->node;
  lua_assert(luaH_hashdir(nodes) == NULL);
  keys = luaH_recordkeys(nodes);
  if (keys != NULL) {
    unsigned index = luaH_recordindex(keys, key);
    found = index < keys->asize ? luaH_recordvalue(table, index, result)
                               : LUA_VABSTKEY;
  }
  else
    found = luaH_hashvalue(luaH_shortslot(luaH_shortposition(nodes, key), key),
                           result);
  *tag = luaH_privatevalue(L, table, result, found);
  return 1;
}

/* Nonparking lookup. A successful read can report an absent key. */
l_sinline int luaH_tryget (lua_State *L, Table *table, const TValue *key,
                           TValue *result, lu_byte *tag) {
  if (ttisinteger(key)) return luaH_trygetint(L, table, ivalue(key), result, tag);
  if (ttisshrstring(key))
    return luaH_trygetshortstr(L, table, tsvalue(key), result, tag);
  return luaH_trygetkey(L, table, key, result, tag);
}

l_sinline unsigned luaH_writerindex (const Value *slot) {
  uintptr_t address = cast(uintptr_t, slot);
  return cast_uint((address >> 3) ^ (address >> 12)) & (LUA_TABLE_WRITERS - 1);
}

l_sinline LuaTableWriter *luaH_writer (global_State *g, const Value *slot) {
  return &g->table_writers[luaH_writerindex(slot)];
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
LUAI_FUNC void luaH_waitpart (lua_State *, LuaHashPart *);
LUAI_FUNC int luaH_quiescewriters (lua_State *, Table *, int);
/* Inputs remain rooted; NULL drains all rows, otherwise wait for one row. */
LUAI_FUNC void luaH_waitwriters (lua_State *, Table *, LuaTableWriter *);
LUAI_FUNC int luaH_waitlayout (lua_State *, Table *);
LUAI_FUNC void luaH_elementstore (lua_State *, Table *, Value *, lu_byte *,
                                 const TValue *);
#endif
