/* Bounded visible writers, separate from layout ownership. See lua.h. */
#define LUA_CORE
#include "lprefix.h"
#include "ltableaccess.h"
#include "ldo.h"

static int active (global_State *g, Table *table, LuaTableWriter *writer) {
  unsigned i;
  if (writer != NULL)
    return __atomic_load_n(&writer->owner, __ATOMIC_SEQ_CST) == table;
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
  if (__atomic_load_n(&writer->waiting, __ATOMIC_SEQ_CST) ||
      __atomic_load_n(&table->access.waiting, __ATOMIC_SEQ_CST)) {
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
  __atomic_store_n(&writer->slot, cast(uintptr_t, slot), __ATOMIC_RELEASE);
  __atomic_store_n(&writer->sequence, sequence + 1, __ATOMIC_RELEASE);
  return writer;
}

/* The caller has claimed layout ownership. A scanner defers busy tables;
** mutators park with that ownership rooted, as ordinary object waits do. */
int luaH_quiescewriters (lua_State *L, Table *table, int wait) {
  if (!(__atomic_load_n(&table->sharing, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED))
    return 1;
  if (!__atomic_load_n(&table->writers_seen, __ATOMIC_SEQ_CST)) return 1;
  __atomic_store_n(&table->writers_seen, 0, __ATOMIC_SEQ_CST);
  if (!active(G(L), table, NULL)) return 1;
  if (!wait) {
    __atomic_store_n(&table->writers_seen, 1, __ATOMIC_SEQ_CST);
    return 0;
  }
  luaH_waitwriters(L, table, NULL);
  return 1;
}

static int writersbusy (lua_State *L, Table *table, LuaTableWriter *writer) {
  return active(G(L), table, writer);
}

static int layoutbusy (lua_State *L, Table *table, LuaTableWriter *writer) {
  void *owner = __atomic_load_n(&table->access.owner, __ATOMIC_SEQ_CST);
  cast_void(writer);
  return owner != NULL && owner != luaX_token(L);
}

/* Publish the whole Lua frame before parking, as an object scope does. */
static void waittable (lua_State *L, Table *table, LuaTableWriter *writer,
                       int (*busy)(lua_State *, Table *, LuaTableWriter *)) {
  LuaWorld *world = &G(L)->world;
  unsigned *waiting = writer != NULL ? &writer->waiting : &table->access.waiting;
  ptrdiff_t saved_top = -1;
  if (isLua(L->ci) && L->top.p < L->ci->top.p) {
    saved_top = savestack(L, L->top.p);
    L->top.p = L->ci->top.p;
  }
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  __atomic_fetch_add(waiting, 1, __ATOMIC_SEQ_CST);
  if (world->stopped || busy(L, table, writer)) {
    lua_assert(world->heap_depth == 0 || !luaX_isowner(L, &world->heap_owner));
    L->native_depth++;
    L->object_waiting++;
    luaW_deactivate(L);
    luaQ_wake(&world->changed, &world->waiters);
    do {
      luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
    } while (world->stopped || busy(L, table, writer));
    luaW_resume(L);
    luaC_checksnapshot(L);
    L->object_waiting--;
    L->native_depth--;
  }
  __atomic_fetch_sub(waiting, 1, __ATOMIC_SEQ_CST);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  if (saved_top >= 0) L->top.p = restorestack(L, saved_top);
}

void luaH_waitwriters (lua_State *L, Table *table, LuaTableWriter *writer) {
  waittable(L, table, writer, writersbusy);
}

/* Waiting for a layout change is not a request to own the following layout.
** Reentrant native borrows and the quiescent collector keep their scoped path. */
int luaH_waitlayout (lua_State *L, Table *table) {
  if (luaW_iscollector(L) || !layoutbusy(L, table, NULL)) return 0;
  waittable(L, table, NULL, layoutbusy);
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
