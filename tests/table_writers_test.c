#include <laccess.h>
#include <lapi.h>
#include <lauxlib.h>
#include <limits.h>
#include <ltable.h>
#include <ltableaccess.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define require(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                  \
      abort();                                                                 \
    }                                                                          \
  } while (0)

static struct {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *first, *second, *layout;
  Table *table;
  int entered, release, second_done, layout_waiting, layout_done;
  int hash, record, graph, collision, second_parent, length;
  lua_Integer second_key;
  unsigned first_slot;
  char hash_second[32];
} probe = {.mutex = PTHREAD_MUTEX_INITIALIZER,
           .changed = PTHREAD_COND_INITIALIZER};

static void hold(void) {
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.entered = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  while (!probe.release)
    require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
}

void __real_luaA_begin(lua_State *, GCObject *, LuaObjectScope *);
void __wrap_luaA_begin(lua_State *L, GCObject *object, LuaObjectScope *scope) {
  __real_luaA_begin(L, object, scope);
  if (L == probe.first && object == (GCObject *)probe.table && !probe.entered)
    hold(); /* Old implementation owns the whole table. */
}

void __real_luaH_elementstore(lua_State *, Table *, Value *, lu_byte *,
                              const TValue *) __attribute__((weak));
void __wrap_luaH_elementstore(lua_State *L, Table *table, Value *payload,
                              lu_byte *tag, const TValue *value) {
  if (L == probe.first && table == probe.table && !probe.entered)
    hold(); /* The element is claimed, before its split value is stored. */
  __real_luaH_elementstore(L, table, payload, tag, value);
}

void __real_luaH_recordstore(lua_State *, Table *, unsigned, lu_byte,
                             const TValue *);
void __wrap_luaH_recordstore(lua_State *L, Table *table, unsigned index,
                             lu_byte oldtag, const TValue *value) {
  if (L == probe.first && table == probe.table && !probe.entered)
    hold();
  __real_luaH_recordstore(L, table, index, oldtag, value);
}

void __real_luaQ_wait(lua_State *, pthread_mutex_t *, pthread_cond_t *,
                      LuaWaiter **);
void __wrap_luaQ_wait(lua_State *L, pthread_mutex_t *mutex,
                      pthread_cond_t *changed, LuaWaiter **waiters) {
  if (L == probe.layout) {
    require(pthread_mutex_lock(&probe.mutex) == 0);
    probe.layout_waiting = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    require(pthread_mutex_unlock(&probe.mutex) == 0);
  }
  __real_luaQ_wait(L, mutex, changed, waiters);
}

static void *first(void *unused) {
  (void)unused;
  if (probe.graph)
    lua_createtable(probe.first, 1, 0);
  lua_pushinteger(probe.first, 11);
  if (probe.graph)
    lua_rawseti(probe.first, -2, 1);
  if (probe.hash)
    lua_setfield(probe.first, 1, "field-1");
  else
    lua_rawseti(probe.first, 1, 1);
  return NULL;
}
static void *second(void *unused) {
  unsigned i;
  (void)unused;
  for (i = 0; i < 128; i++) {
    lua_Integer number = i & 1 ? LUA_MININTEGER : LUA_MAXINTEGER;
    lua_pushinteger(probe.second, number);
    if (probe.hash)
      lua_setfield(probe.second, 1, probe.hash_second);
    else
      lua_rawseti(probe.second, 1, probe.second_key);
    require((probe.hash ? lua_getfield(probe.second, 1, probe.hash_second)
                        : lua_rawgeti(probe.second, 1, probe.second_key)) ==
            LUA_TNUMBER);
    require(lua_tointeger(probe.second, -1) == number);
    lua_pop(probe.second, 1);
  }
  if (probe.graph)
    lua_createtable(probe.second, 1, 0);
  lua_pushinteger(probe.second, 22);
  if (probe.graph)
    lua_rawseti(probe.second, -2, 1);
  if (probe.hash)
    lua_setfield(probe.second, 1, probe.hash_second);
  else
    lua_rawseti(probe.second, 1, probe.second_key);
  require((probe.hash ? lua_getfield(probe.second, 1, probe.hash_second)
                      : lua_rawgeti(probe.second, 1, probe.second_key)) ==
          (probe.graph ? LUA_TTABLE : LUA_TNUMBER));
  if (probe.graph)
    require(lua_rawgeti(probe.second, -1, 1) == LUA_TNUMBER);
  require(lua_tointeger(probe.second, -1) == 22);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.second_done = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  return NULL;
}
static void *resize(void *unused) {
  (void)unused;
  lua_lock(probe.layout);
  luaH_resizearray(probe.layout, probe.table, 4096);
  lua_unlock(probe.layout);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.layout_done = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  return NULL;
}

static int length_method(lua_State *L) {
  lua_pushinteger(L, 88);
  return 1;
}

static void check_length(lua_State *L, int table, lua_Integer expected) {
  require(luaL_loadstring(L, "local t=...; return #t") == LUA_OK);
  lua_pushvalue(L, table);
  require(lua_pcall(L, 1, 1, 0) == LUA_OK);
  require(lua_tointeger(L, -1) == expected);
  lua_pop(L, 1);
}

static void check_length_changes(lua_State *L) {
  int i, table;
  lua_createtable(L, 4, 0);
  table = lua_gettop(L);
  for (i = 1; i < 4; i++) {
    lua_pushinteger(L, i);
    lua_rawseti(L, table, i);
  }
  lua_newtable(L);
  lua_rawseti(L, table, 4);
  require(lua_rawlen(L, table) == 4);
  check_length(L, table, 4);
  lua_createtable(L, 0, 2);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  lua_pushcfunction(L, length_method);
  lua_setfield(L, -2, "__len");
  lua_setmetatable(L, table);
  check_length(L, table, 88);
  require(lua_rawlen(L, table) == 4);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_rawlen(L, table) == 3);
  check_length(L, table, 88);
  lua_pop(L, 1);
}

static void check_writer_groups(lua_State *L) {
  Table *table;
  Value *low, *high;
  LuaTableWriter *a, *b;
  unsigned i, low_group, high_group;
  uint64_t layout;
  int scan;
  lua_createtable(L, 1024, 0);
  table = (Table *)lua_topointer(L, -1);
  lua_pushvalue(L, -1);
  lua_xmove(L, probe.second, 1);
  lua_pop(probe.second, 1);
  lua_lock(L);
  low = getArrVal(table, 0);
  low_group = luaH_writerindex(low) / LUA_TABLE_WRITERS_PER_GROUP;
  for (i = 1; i < table->asize; i++) {
    high = getArrVal(table, i);
    high_group = luaH_writerindex(high) / LUA_TABLE_WRITERS_PER_GROUP;
    if (low_group != high_group)
      break;
  }
  require(i < table->asize);
  if (low_group > high_group) {
    Value *swap = low;
    low = high;
    high = swap;
  }
  layout = table->sequence;
  a = luaH_claimwriter(L, table, low, layout);
  b = luaH_claimwriter(L, table, high, layout);
  require(a != NULL && b != NULL && a != b);
  require(luaH_samelayout(table, layout));
  require(luaH_beginsplit(table, b, high, layout));
  require(!luaH_samelayout(table, layout));
  require(!luaA_tryscan(L, (GCObject *)table));
  luaH_releasewriter(L, table, a);
  /* The first group is idle. The scan still needs the writer in the next
   * group, even after reopening the layout following the earlier attempt. */
  require(!luaA_tryscan(L, (GCObject *)table));
  luaH_releasewriter(L, table, b);
  scan = luaA_tryscan(L, (GCObject *)table);
  require(scan && (table->sequence & LUA_TABLE_WRITER_FLAGS) == 0);
  luaA_endscan(L, (GCObject *)table, scan);
  lua_unlock(L);
  lua_pop(L, 1);
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  pthread_t a, b, c;
  struct timespec deadline;
  unsigned i;
  require(argc == 3 && L != NULL);
  probe.record = strstr(argv[2], "record") != NULL;
  probe.hash = probe.record || strstr(argv[2], "hash") != NULL;
  probe.graph = strstr(argv[2], "graph") != NULL;
  probe.collision = strcmp(argv[2], "collision") == 0;
  probe.length = strcmp(argv[2], "length") == 0;
  probe.second_key = 2;
  probe.second_parent = 1;
  lua_gc(L, strcmp(argv[1], "generational") == 0 ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  if (probe.record) {
    require(luaL_dostring(L, "return {['field-1']=0, ['field-2']=0}") ==
            LUA_OK);
    require(luaH_recordkeys(((Table *)lua_topointer(L, 1))->node) != NULL);
    snprintf(probe.hash_second, sizeof(probe.hash_second), "field-2");
  } else
    lua_createtable(L, probe.hash ? 0 : 1024, probe.hash ? 64 : 0);
  probe.table = (Table *)lua_topointer(L, 1);
  for (i = 1; !probe.record && i <= (probe.hash ? 1U : 1024U); i++) {
    char key[32];
    lua_pushinteger(L, 0);
    if (probe.hash) {
      snprintf(key, sizeof(key), "field-%u", i);
      lua_setfield(L, 1, key);
    } else
      lua_rawseti(L, 1, i);
  }
  if (probe.length)
    require(lua_rawlen(L, 1) == 1024);
  if (probe.hash && !probe.record) {
    const TValue *a;
    unsigned bucket, mask = sizenode(probe.table) - 1;
    lua_pushliteral(L, "field-1");
    bucket = tsvalue(s2v(L->top.p - 1))->hash & mask;
    a = luaH_shortslot(probe.table->node + bucket, tsvalue(s2v(L->top.p - 1)));
    probe.first_slot = (unsigned)((const Node *)a - probe.table->node);
    lua_pop(L, 1);
    for (i = 2; i <= 10000; i++) {
      const TValue *b;
      lua_pushfstring(L, "field-%d", (int)i);
      if ((tsvalue(s2v(L->top.p - 1))->hash & mask) != bucket) {
        lua_pop(L, 1);
        continue;
      }
      /* Force B behind A in the same hash chain. A's busy value must not
       * prevent a lookup from reaching a different, valid B value. */
      lua_pushinteger(L, 0);
      lua_rawset(L, 1);
      lua_pushfstring(L, "field-%d", (int)i);
      b = luaH_shortslot(probe.table->node + bucket,
                         tsvalue(s2v(L->top.p - 1)));
      lua_pop(L, 1);
      if (luaH_writer(G(L), &a->value_) != luaH_writer(G(L), &b->value_))
        break;
    }
    require(i <= 10000);
    snprintf(probe.hash_second, sizeof(probe.hash_second), "field-%u", i);
  }
  if (probe.collision) {
    Table *other;
    lua_createtable(L, 4096, 0);
    probe.second_parent = 2;
    other = (Table *)lua_topointer(L, 2);
    for (i = 1; i <= 4096; i++) {
      lua_pushinteger(L, 0);
      lua_rawseti(L, 2, i);
    }
    for (i = 0; i < 4096; i++)
      if (luaH_writer(G(L), getArrVal(probe.table, 0)) ==
          luaH_writer(G(L), getArrVal(other, i)))
        break;
    require(i < 4096);
    probe.second_key = (lua_Integer)i + 1;
  }
  probe.first = lua_newthread(L);
  probe.second = lua_newthread(L);
  probe.layout = lua_newthread(L);
  for (i = 0; i < 3; i++) {
    lua_State *state = i == 0   ? probe.first
                       : i == 1 ? probe.second
                                : probe.layout;
    lua_pushvalue(L, i == 1 ? probe.second_parent : 1);
    lua_xmove(L, state, 1);
  }
  require(!(probe.table->sequence & LUA_TABLE_WRITER_FLAGS));
  require(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += 10;
  require(pthread_create(&a, NULL, first, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.entered)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_create(&b, NULL, second, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.second_done)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(!probe.release);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  if (probe.length) {
    /* The writer is paused in this table. A cached length depends on the
     * layout and the final tag and can finish without taking ownership. */
    require(lua_rawlen(L, 1) == 1024);
    check_length(L, 1, 1024);
  }
  if (!probe.graph) {
    /* A paused same-type store leaves the previous atomic payload readable. */
    require((probe.hash ? lua_getfield(L, 1, "field-1")
                        : lua_rawgeti(L, 1, 1)) == LUA_TNUMBER);
    require(lua_tointeger(L, -1) == 0);
    lua_pop(L, 1);
  }
  /* A collector must defer rather than park, and must preserve the activity
   * hint so the next layout owner still waits for this outstanding writer. */
  lua_lock(L);
  {
    GCObject *edges[2];
    unsigned found;
    /* The layout version is still even. The scanner validates a slot only
     * if a writer can change both its tag and payload. */
    require(!(probe.table->sequence & 1));
    require(probe.table->sequence & LUA_TABLE_WRITER_GROUP_MASK);
    require(!!(probe.table->sequence & LUA_TABLE_SPLIT_VALUES) == probe.graph);
    if (probe.record) {
      TValue value;
      int read =
          luaH_readrecord(G(L), probe.table, 0, &value, probe.table->sequence);
      require(read == !probe.graph);
      if (read)
        require(ttisinteger(&value) && ivalue(&value) == 0);
      require(
          luaH_readrecord(G(L), probe.table, 1, &value, probe.table->sequence));
      require(probe.graph ? ttistable(&value) : ttisinteger(&value));
    } else {
      found = luaH_tracechunk(probe.graph ? G(L) : NULL, probe.table,
                              probe.first_slot, 1, probe.hash, edges);
      if (probe.graph)
        require(found == UINT_MAX);
      else if (probe.hash) {
        require(found == 1);
        require(edges[0] == gckey(gnode(probe.table, probe.first_slot)));
      } else
        require(found == 0);
    }
  }
  require(!luaA_tryscan(L, (GCObject *)probe.table));
  require(probe.table->sequence & LUA_TABLE_WRITER_GROUP_MASK);
  lua_unlock(L);
  require(pthread_create(&c, NULL, resize, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.layout_waiting && !probe.layout_done)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(!probe.layout_done && (probe.table->sequence & 1));
  probe.release = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
  require(pthread_join(c, NULL) == 0 && probe.layout_done);
  require(!(probe.table->sequence & LUA_TABLE_WRITER_FLAGS));
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCCOLLECT);
  require((probe.hash ? lua_getfield(L, 1, "field-1") : lua_rawgeti(L, 1, 1)) ==
          (probe.graph ? LUA_TTABLE : LUA_TNUMBER));
  if (probe.graph)
    require(lua_rawgeti(L, -1, 1) == LUA_TNUMBER);
  require(lua_tointeger(L, -1) == 11);
  lua_pop(L, probe.graph ? 2 : 1);
  require((probe.hash
               ? lua_getfield(L, probe.second_parent, probe.hash_second)
               : lua_rawgeti(L, probe.second_parent, probe.second_key)) ==
          (probe.graph ? LUA_TTABLE : LUA_TNUMBER));
  if (probe.graph)
    require(lua_rawgeti(L, -1, 1) == LUA_TNUMBER);
  require(lua_tointeger(L, -1) == 22);
  if (probe.length)
    check_length_changes(L);
  check_writer_groups(L);
  lua_close(L);
  require(pthread_cond_destroy(&probe.changed) == 0);
  require(pthread_mutex_destroy(&probe.mutex) == 0);
  return 0;
}
