/* Bounded visible writers, separate from layout ownership. See lua.h. */
#define LUA_CORE
#include "lprefix.h"
#include "ltableaccess.h"

static int active (global_State *g, Table *table) {
  unsigned i;
  for (i = 0; i < LUA_TABLE_WRITERS; i++)
    if (__atomic_load_n(&g->table_writers[i].owner, __ATOMIC_SEQ_CST) == table)
      return 1;
  return 0;
}

void luaH_releasewriter (lua_State *L, Table *table, LuaTableWriter *writer) {
  uint64_t sequence = __atomic_load_n(&writer->sequence, __ATOMIC_RELAXED);
  if (sequence & 1)
    __atomic_store_n(&writer->sequence, sequence + 1, __ATOMIC_RELEASE);
  __atomic_store_n(&writer->owner, NULL, __ATOMIC_SEQ_CST);
  /* Same observer handshake as object ownership: a registering waiter either
  ** sees this departure, or this release sees its registration and wakes it. */
  if (__atomic_load_n(&table->access.waiting, __ATOMIC_SEQ_CST)) {
    LuaWorld *world = &G(L)->world;
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    luaQ_wake(&world->changed, &world->waiters);
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
  }
}

LuaTableWriter *luaH_claimwriter (lua_State *L, Table *table, Value *slot,
                                  uint64_t layout) {
  LuaTableWriter *writer = luaH_writer(G(L), slot);
  Table *empty = NULL;
  uint64_t sequence;
  if (!__atomic_compare_exchange_n(&writer->owner, &empty, table, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) return NULL;
  /* Publish AFTER the row claim. Layout owners clear this hint BEFORE they
  ** scan, so a late claimant cannot be lost behind a completed scan. The row
  ** CAS and owner check are the store/load admission handshake, not a count
  ** modified by every writer on the same table. */
  if (!__atomic_load_n(&table->writers_seen, __ATOMIC_SEQ_CST))
    __atomic_store_n(&table->writers_seen, 1, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(&table->access.owner, __ATOMIC_SEQ_CST) != NULL ||
      __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE) != layout ||
      !luaH_strongaccess(L, table)) {
    luaH_releasewriter(L, table, writer);
    return NULL;
  }
  sequence = __atomic_load_n(&writer->sequence, __ATOMIC_RELAXED);
  lua_assert(!(sequence & 1));
  __atomic_store_n(&writer->sequence, sequence + 1, __ATOMIC_RELEASE);
  return writer;
}

/* The caller has claimed layout ownership. A scanner defers busy tables;
** mutators park with that ownership rooted, as ordinary object waits do. */
int luaH_quiescewriters (lua_State *L, Table *table, int wait) {
  LuaWorld *world = &G(L)->world;
  if (!(__atomic_load_n(&table->sharing, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED))
    return 1;
  if (!__atomic_load_n(&table->writers_seen, __ATOMIC_SEQ_CST)) return 1;
  __atomic_store_n(&table->writers_seen, 0, __ATOMIC_SEQ_CST);
  if (!active(G(L), table)) return 1;
  if (!wait) {
    __atomic_store_n(&table->writers_seen, 1, __ATOMIC_SEQ_CST);
    return 0;
  }
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  __atomic_fetch_add(&table->access.waiting, 1, __ATOMIC_SEQ_CST);
  if (world->stopped || active(G(L), table)) {
    lua_assert(world->heap_depth == 0 || !luaX_isowner(L, &world->heap_owner));
    L->native_depth++;
    L->object_waiting++;
    luaW_deactivate(L);
    luaQ_wake(&world->changed, &world->waiters);
    do {
      luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
    } while (world->stopped || active(G(L), table));
    luaW_resume(L);
    luaC_checksnapshot(L);
    L->object_waiting--;
    L->native_depth--;
  }
  __atomic_fetch_sub(&table->access.waiting, 1, __ATOMIC_SEQ_CST);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  return 1;
}

/* No allocation, wait, callback or safepoint between row claim and release. */
void luaH_elementstore (lua_State *L, Table *table, Value *payload, lu_byte *tag,
                         const TValue *value) {
  TValue old;
  old.value_ = *payload;
  old.tt_ = *tag;
  luaC_prewrite(L, &old, value);
  __atomic_store(payload, &value->value_, __ATOMIC_RELEASE);
  __atomic_store_n(tag, value->tt_, __ATOMIC_RELEASE);
  luaC_barrierback(L, obj2gco(table), value);
}
