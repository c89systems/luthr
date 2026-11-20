/* Bounded visible writers, separate from layout ownership. See lua.h. */
#define LUA_CORE
#include "lprefix.h"
#include "laccess.h"
#include "ltableaccess.h"
#include "ldo.h"

static int partbusy (const LuaHashPart *part) {
  return (__atomic_load_n(&part->gate, __ATOMIC_SEQ_CST) &
          LUA_HASH_WRITING) != 0;
}

static int active (global_State *g, Table *table, LuaTableWriter *writer) {
  LuaHashDirectory *directory;
  unsigned i;
  if (writer != NULL)
    return __atomic_load_n(&writer->owner, __ATOMIC_SEQ_CST) == table;
  directory = luaH_hashdir(__atomic_load_n(&table->node, __ATOMIC_ACQUIRE));
  if (directory != NULL)
    for (i = 1; i < LUA_HASH_PARTS; ++i)
      if (partbusy(&directory->parts[i])) return 1;
  if (!(__atomic_load_n(&table->sequence, __ATOMIC_RELAXED) &
        LUA_TABLE_ELEMENT_WRITES)) return 0;
  for (i = 0; i < LUA_TABLE_WRITERS; i++)
    if (__atomic_load_n(&g->table_writers[i].owner, __ATOMIC_SEQ_CST) == table)
      return 1;
  return 0;
}

void luaH_releasepart (lua_State *L, Table *table, LuaHashPart *part) {
  unsigned previous = __atomic_fetch_and(&part->gate,
    ~cast_uint(LUA_HASH_WRITING), __ATOMIC_SEQ_CST);
  int watched = luaA_lockwatched(&table->access);
  lua_assert(previous & LUA_HASH_WRITING);
  if ((previous & LUA_HASH_WAITING) || watched) {
    LuaWorld *world = &G(L)->world;
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    if (previous & LUA_HASH_WAITING)
      luaQ_wake(&part->changed, &part->waiters);
    if (watched) luaQ_wake(&world->changed, &world->waiters);
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
  }
}

int luaH_claimpart (lua_State *L, Table *table, LuaHashPart *part,
                    uint64_t layout) {
  unsigned spins;
  /* A partition writer never allocates or reaches a safepoint. A short
  ** retry can finish the write before it needs to publish roots and park. */
  for (spins = 0;; ++spins) {
    unsigned gate = __atomic_load_n(&part->gate, __ATOMIC_RELAXED);
    if (!(gate & LUA_HASH_WRITING) &&
        __atomic_compare_exchange_n(&part->gate, &gate,
          gate | LUA_HASH_WRITING, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
      break;
    if (spins == 32 || luaW_stopped(&G(L)->world)) return -1;
    __builtin_ia32_pause();
  }
  /* Either the layout owner sees this claim or the claimant observes the
  ** closed layout before touching storage. Unlike a row, the partition
  ** exists for the rest of the table's lifetime and needs no advertisement. */
  if (luaA_lockowner(&table->access) != NULL ||
      !luaH_strongaccess(L, table) ||
      ((__atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE) ^ layout) &
       ~cast(uint64_t, LUA_TABLE_WRITER_FLAGS)) != 0) {
    luaH_releasepart(L, table, part);
    return 0;
  }
  return 1;
}

void luaH_releasewriter (lua_State *L, Table *table, LuaTableWriter *writer) {
  uint64_t sequence = __atomic_load_n(&writer->sequence, __ATOMIC_RELAXED);
  if (sequence & 1)
    __atomic_store_n(&writer->sequence, sequence + 1, __ATOMIC_RELEASE);
  __atomic_store_n(&writer->owner, NULL, __ATOMIC_SEQ_CST);
  /* Same observer handshake as object ownership: a registering waiter either
  ** sees this departure, or this release sees its registration and wakes it. */
  if (__atomic_load_n(&writer->waiting, __ATOMIC_SEQ_CST) ||
      luaA_lockwatched(&table->access)) {
    LuaWorld *world = &G(L)->world;
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    luaQ_wake(&world->changed, &world->waiters);
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
  }
}

static int advertise (Table *table, uint64_t layout, uint64_t flag) {
  uint64_t current = __atomic_load_n(&table->sequence, __ATOMIC_ACQUIRE);
  lua_assert(!(layout & 1));
  /* Flags only accumulate while this even layout is current. A structural
  ** owner closes the layout before it can drain writers and clear them. */
  while (((current ^ layout) & ~cast(uint64_t, LUA_TABLE_WRITER_FLAGS)) == 0) {
    if ((current & flag) ||
        __atomic_compare_exchange_n(&table->sequence, &current, current | flag,
                                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return 1;
  }
  return 0;
}

LuaTableWriter *luaH_claimwriter (lua_State *L, Table *table, Value *slot,
                                  uint64_t layout) {
  LuaTableWriter *writer = luaH_writer(G(L), slot);
  Table *empty = NULL;
  if (!__atomic_compare_exchange_n(&writer->owner, &empty, table, 0,
                                    __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) return NULL;
  /* A layout owner either sees the claimed row, or closes the even version
  ** before the claimant advertises itself. An odd version rejects new writers. */
  if (luaA_lockowner(&table->access) != NULL ||
      !luaH_strongaccess(L, table) ||
      !advertise(table, layout, LUA_TABLE_ELEMENT_WRITES)) {
    luaH_releasewriter(L, table, writer);
    return NULL;
  }
  return writer;
}

int luaH_beginsplit (Table *table, LuaTableWriter *writer, Value *slot,
                     uint64_t layout) {
  uint64_t sequence;
  /* Announce split publication before either component changes. A reader
  ** using the previous layout must detect this change on validation. */
  if (!advertise(table, layout, LUA_TABLE_SPLIT_VALUES)) return 0;
  sequence = __atomic_load_n(&writer->sequence, __ATOMIC_RELAXED);
  lua_assert(!(sequence & 1));
  __atomic_store_n(&writer->slot, cast(uintptr_t, slot), __ATOMIC_RELEASE);
  __atomic_store_n(&writer->sequence, sequence + 1, __ATOMIC_RELEASE);
  return 1;
}

/* The caller has claimed layout ownership. A scanner defers busy tables;
** mutators park with that ownership rooted, as ordinary object waits do. */
int luaH_quiescewriters (lua_State *L, Table *table, int wait) {
  uint64_t layout = __atomic_load_n(&table->sequence, __ATOMIC_RELAXED);
  lua_assert(layout & 1);
  if (active(G(L), table, NULL)) {
    if (!wait) return 0;
    luaH_waitwriters(L, table, NULL);
  }
  if (layout & LUA_TABLE_ELEMENT_WRITES)
    __atomic_store_n(&table->sequence, layout & ~cast(uint64_t, LUA_TABLE_WRITER_FLAGS),
                     __ATOMIC_RELEASE);
  return 1;
}

static int writersbusy (lua_State *L, Table *table, LuaTableWriter *writer) {
  return active(G(L), table, writer);
}

static int layoutbusy (lua_State *L, Table *table, LuaTableWriter *writer) {
  void *owner = luaA_lockowner(&table->access);
  cast_void(writer);
  return owner != NULL && owner != luaX_token(L);
}

/* Publish the whole Lua frame before parking, as an object scope does. */
static void waittable (lua_State *L, Table *table, LuaTableWriter *writer,
                       LuaHashPart *part,
                       int (*busy)(lua_State *, Table *, LuaTableWriter *)) {
  LuaWorld *world = &G(L)->world;
  ptrdiff_t saved_top = -1;
  if (isLua(L->ci) && L->top.p < L->ci->top.p) {
    saved_top = savestack(L, L->top.p);
    L->top.p = L->ci->top.p;
  }
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  if (writer != NULL)
    __atomic_fetch_add(&writer->waiting, 1, __ATOMIC_SEQ_CST);
  else if (part == NULL) luaA_watchlock(&table->access);
  if (luaW_stopped(world) || (part != NULL ? partbusy(part) : busy(L, table, writer))) {
    lua_assert(world->heap_depth == 0 || !luaX_isowner(L, &world->heap_owner));
    L->native_depth++;
    L->object_waiting++;
    luaW_deactivate(L);
    if (part == NULL ||
        (luaW_control(world) & (LUA_WORLD_STOPPED | LUA_WORLD_STORAGE_WAIT)))
      luaQ_wake(&world->changed, &world->waiters);
    do {
      if (part != NULL && !luaW_stopped(world)) {
        /* The release tests this bit in the same operation that opens the
        ** partition. A registering waiter either observes that departure
        ** or is visible to the release while holding the predicate mutex. */
        if (__atomic_fetch_or(&part->gate, LUA_HASH_WAITING,
                              __ATOMIC_SEQ_CST) & LUA_HASH_WRITING)
          luaQ_wait(L, &world->mutex, &part->changed, &part->waiters);
        if (part->waiters == NULL)
          __atomic_fetch_and(&part->gate, ~cast_uint(LUA_HASH_WAITING),
                             __ATOMIC_SEQ_CST);
      }
      else {
        luaQ_wait(L, &world->mutex, &world->changed, &world->waiters);
        if (writer == NULL && part == NULL) luaA_watchlock(&table->access);
      }
    } while (luaW_stopped(world) || (part != NULL ? partbusy(part) : busy(L, table, writer)));
    luaW_resume(L);
    luaC_checksnapshot(L);
    L->object_waiting--;
    L->native_depth--;
  }
  if (writer != NULL)
    __atomic_fetch_sub(&writer->waiting, 1, __ATOMIC_SEQ_CST);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  if (saved_top >= 0) L->top.p = restorestack(L, saved_top);
}

void luaH_waitwriters (lua_State *L, Table *table, LuaTableWriter *writer) {
  waittable(L, table, writer, NULL, writersbusy);
}

void luaH_waitpart (lua_State *L, Table *table, LuaHashPart *part) {
  if (partbusy(part) || luaW_stopped(&G(L)->world))
    waittable(L, table, NULL, part, NULL);
}

/* Waiting for a layout change is not a request to own the following layout.
** Reentrant native borrows and the quiescent collector keep their scoped path. */
int luaH_waitlayout (lua_State *L, Table *table) {
  if (luaW_iscollector(L) || !layoutbusy(L, table, NULL)) return 0;
  waittable(L, table, NULL, NULL, layoutbusy);
  return 1;
}

/* No allocation, wait, callback or safepoint between row claim and release. */
void luaH_elementstore (lua_State *L, Table *table, Value *payload, lu_byte *tag,
                         const TValue *value) {
  TValue old;
  old.value_ = *payload;
  old.tt_ = *tag;
  lua_assert(old.tt_ == value->tt_ ||
    (__atomic_load_n(&luaH_writer(G(L), payload)->sequence, __ATOMIC_RELAXED) & 1));
  luaC_prewrite(L, &old, value);
  __atomic_store(payload, &value->value_, __ATOMIC_RELEASE);
  if (old.tt_ != value->tt_) __atomic_store_n(tag, value->tt_, __ATOMIC_RELEASE);
  luaC_barrierback(L, obj2gco(table), value);
}
