#include <laccess.h>
#include <lapi.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lmempage.h>
#include <ltable.h>
#include <ltableaccess.h>
#include <pthread.h>
#include <sched.h>
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
  lua_State *first, *blocked, *independent, *marker;
  Table *table;
  lua_Integer keys[3];
  int entered, waiting, release, completed, graph, vm, collecting, collected;
  int reader, relocated, missing;
} probe = {.mutex = PTHREAD_MUTEX_INITIALIZER,
           .changed = PTHREAD_COND_INITIALIZER};

static int missing_value(lua_State *L) {
  if (probe.graph) {
    lua_createtable(L, 1, 0);
    lua_pushinteger(L, 11);
    lua_rawseti(L, -2, 1);
  } else
    lua_pushinteger(L, 11);
  return 1;
}

/* Stack-movement assertions must not depend on realloc's choice of address. */
static void *allocate(void *unused, void *memory, size_t old_size,
                      size_t size) {
  void *replacement;
  (void)unused;
  if (size == 0) {
    free(memory);
    return NULL;
  }
  replacement = malloc(size);
  if (replacement == NULL)
    return NULL;
  if (memory != NULL) {
    memcpy(replacement, memory, old_size < size ? old_size : size);
    free(memory);
  }
  return replacement;
}

void __real_luaH_elementstore(lua_State *, Table *, Value *, lu_byte *,
                              const TValue *);
void __wrap_luaH_elementstore(lua_State *L, Table *table, Value *payload,
                              lu_byte *tag, const TValue *value) {
  if (L == probe.first && table == probe.table) {
    require(pthread_mutex_lock(&probe.mutex) == 0);
    probe.entered = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    while (!probe.release)
      require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
    require(pthread_mutex_unlock(&probe.mutex) == 0);
  }
  __real_luaH_elementstore(L, table, payload, tag, value);
}

void __real_luaQ_wait(lua_State *, pthread_mutex_t *, pthread_cond_t *,
                      LuaWaiter **);
void __wrap_luaQ_wait(lua_State *L, pthread_mutex_t *mutex,
                      pthread_cond_t *cond, LuaWaiter **waiters) {
  if (L == probe.blocked) {
    require(pthread_mutex_lock(&probe.mutex) == 0);
    probe.waiting = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    require(pthread_mutex_unlock(&probe.mutex) == 0);
  }
  __real_luaQ_wait(L, mutex, cond, waiters);
  if (L == probe.blocked) {
    int delay;
    require(pthread_mutex_lock(&probe.mutex) == 0);
    delay = probe.collecting && !probe.collected;
    require(pthread_mutex_unlock(&probe.mutex) == 0);
    if (delay) {
      LuaExecution *execution = luaQ_current();
      int leased = execution != NULL && execution->memory_cache != NULL;
      if (leased)
        luaMC_leave(execution);
      /* Model a late scheduler wake: this execution remains inactive while
       * every GC phase completes, with luaQ_wait's cache lease protocol. */
      require(pthread_mutex_unlock(mutex) == 0);
      require(pthread_mutex_lock(&probe.mutex) == 0);
      while (!probe.collected)
        require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
      require(pthread_mutex_unlock(&probe.mutex) == 0);
      require(pthread_mutex_lock(mutex) == 0);
      if (leased)
        luaMC_enter(execution);
    }
  }
}

static void *collect(void *unused) {
  (void)unused;
  lua_gc(probe.marker, LUA_GCCOLLECT);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.collected = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  return NULL;
}

void __real_luaH_waitwriters(lua_State *, Table *, LuaTableWriter *);
void __wrap_luaH_waitwriters(lua_State *L, Table *table, LuaTableWriter *row) {
  __real_luaH_waitwriters(L, table, row);
  if (L == probe.blocked && probe.reader && !probe.relocated) {
    uintptr_t before = (uintptr_t)L->stack.p;
    probe.relocated = 1;
    require(luaD_reallocstack(L, stacksize(L) + 128, 1));
    require((uintptr_t)L->stack.p != before);
  }
}

static void *write_value(void *argument) {
  unsigned id = (unsigned)(uintptr_t)argument;
  lua_State *L = id == 0   ? probe.first
                 : id == 1 ? probe.blocked
                           : probe.independent;
  lua_Integer value = (lua_Integer)(id + 1) * 11;
  if (id == 1 && probe.reader && !probe.vm) {
    require(lua_rawgeti(L, 1, probe.keys[0]) ==
            (probe.graph ? LUA_TTABLE : LUA_TNUMBER));
    if (probe.graph)
      require(lua_rawgeti(L, -1, 1) == LUA_TNUMBER);
    require(lua_tointeger(L, -1) == 11);
    lua_pop(L, probe.graph ? 2 : 1);
    require(probe.relocated);
  }
  if (id == 2) {
    /* B aliases A's writer row but is still unchanged. Reading it must not
     * revoke the whole table merely because A is modifying another slot. */
    require(lua_rawgeti(L, 1, probe.keys[1]) == LUA_TNUMBER);
    require(lua_tointeger(L, -1) == 0);
    lua_pop(L, 1);
  }
  if (probe.vm) {
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, probe.keys[id]);
  }
  if (id == 0 && probe.missing)
    lua_pushnil(L);
  else if (probe.graph) {
    lua_createtable(L, 1, 0);
    lua_pushinteger(L, value);
    lua_rawseti(L, -2, 1);
  } else
    lua_pushinteger(L, value);
  if (probe.vm) {
    int reading = id == 1 && probe.reader;
    if (reading)
      lua_pushinteger(L, probe.keys[0]);
    require(lua_pcall(L, reading ? 4 : 3, 1, 0) == LUA_OK);
    require(lua_tointeger(L, -1) == (reading ? 110 : 99));
    if (reading)
      require(probe.relocated);
    lua_pop(L, 1);
  } else
    lua_rawseti(L, 1, probe.keys[id]);
  if (id == 0 && probe.missing) {
    require(lua_rawgeti(L, 1, probe.keys[id]) == LUA_TNIL);
    lua_pop(L, 1);
    return NULL;
  }
  require(lua_rawgeti(L, 1, probe.keys[id]) ==
          (probe.graph ? LUA_TTABLE : LUA_TNUMBER));
  if (probe.graph)
    require(lua_rawgeti(L, -1, 1) == LUA_TNUMBER);
  require(lua_tointeger(L, -1) == value);
  if (id == 2) {
    require(pthread_mutex_lock(&probe.mutex) == 0);
    probe.completed = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    require(pthread_mutex_unlock(&probe.mutex) == 0);
  }
  return NULL;
}

int main(int argc, char **argv) {
  lua_State *L = lua_newstate(allocate, NULL, 0);
  lua_Integer seen[LUA_TABLE_WRITERS] = {0};
  Value *first = NULL;
  pthread_t a, b, c, marker;
  struct timespec deadline;
  unsigned i;
  int hash;
  require(argc == 4 && L != NULL);
  hash = strstr(argv[2], "hash") != NULL;
  probe.graph = strstr(argv[2], "graph") != NULL;
  probe.vm = strncmp(argv[3], "vm", 2) == 0;
  probe.missing = strcmp(argv[3], "vm_missing") == 0;
  probe.reader = strstr(argv[3], "read") != NULL || probe.missing;
  lua_gc(L, strcmp(argv[1], "generational") == 0 ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_createtable(L, hash ? 0 : 4096, hash ? 4096 : 0);
  probe.table = (Table *)lua_topointer(L, 1);
  if (probe.missing) {
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, missing_value);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, 1);
  }
  for (i = 1; i <= 4096; i++) {
    lua_pushinteger(L, 0);
    lua_rawseti(L, 1, hash ? -(lua_Integer)i : (lua_Integer)i);
  }
  /* Choose an actual collision among occupied storage, independent of hash
   * placement and resize decisions. There are more entries than writer rows. */
  for (i = 0; i < (hash ? sizenode(probe.table) : probe.table->asize); i++) {
    Value *slot =
        hash ? &gnode(probe.table, i)->u.value_ : getArrVal(probe.table, i);
    lua_Integer key;
    size_t row;
    if (hash && !keyisinteger(gnode(probe.table, i)))
      continue;
    key = hash ? keyival(gnode(probe.table, i)) : (lua_Integer)i + 1;
    row = (size_t)(luaH_writer(G(L), slot) - G(L)->table_writers);
    if (seen[row]) {
      probe.keys[0] = seen[row];
      probe.keys[1] = key;
      first = slot;
      break;
    }
    seen[row] = key;
  }
  require(first != NULL);
  for (i = 0; i < (hash ? sizenode(probe.table) : probe.table->asize); i++) {
    Value *slot =
        hash ? &gnode(probe.table, i)->u.value_ : getArrVal(probe.table, i);
    if (hash && !keyisinteger(gnode(probe.table, i)))
      continue;
    if (luaH_writer(G(L), slot) != luaH_writer(G(L), first)) {
      probe.keys[2] =
          hash ? keyival(gnode(probe.table, i)) : (lua_Integer)i + 1;
      break;
    }
  }
  require(probe.keys[1] && probe.keys[2]);
  if (probe.reader && !probe.graph) {
    /* Exercise a read that must wait for a split publication. */
    lua_pushboolean(L, 0);
    lua_rawseti(L, 1, probe.keys[0]);
  }
  probe.first = lua_newthread(L);
  probe.blocked = lua_newthread(L);
  probe.independent = lua_newthread(L);
  probe.marker = lua_newthread(L);
  for (i = 0; i < 3; i++) {
    lua_State *state = i == 0   ? probe.first
                       : i == 1 ? probe.blocked
                                : probe.independent;
    lua_pushvalue(L, 1);
    lua_xmove(L, state, 1);
    if (probe.vm) {
      const char *chunk = "local t,k,value=...; local keep={99}; "
                          "t[k]=value; return keep[1]";
      if (i == 1 && probe.reader)
        chunk = probe.graph
                    ? "local t,k,value,r=...; local keep={99}; "
                      "local got=t[r][1]; t[k]=value; return keep[1]+got"
                    : "local t,k,value,r=...; local keep={99}; "
                      "local got=t[r]; t[k]=value; return keep[1]+got";
      require(luaL_loadstring(state, chunk) == LUA_OK);
    }
  }
  require(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += 10;
  require(pthread_create(&a, NULL, write_value, (void *)(uintptr_t)0) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.entered)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_create(&b, NULL, write_value, (void *)(uintptr_t)1) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.waiting)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_create(&c, NULL, write_value, (void *)(uintptr_t)2) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.completed)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(!probe.release);
  if (probe.vm) {
    probe.collecting = 1;
    require(pthread_mutex_unlock(&probe.mutex) == 0);
    require(pthread_create(&marker, NULL, collect, NULL) == 0);
    while (!luaW_stopped(&G(L)->world)) {
      struct timespec now;
      require(clock_gettime(CLOCK_REALTIME, &now) == 0);
      require(now.tv_sec < deadline.tv_sec);
      sched_yield();
    }
    require(pthread_mutex_lock(&probe.mutex) == 0);
  }
  probe.release = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0 &&
          pthread_join(c, NULL) == 0);
  if (probe.vm)
    require(pthread_join(marker, NULL) == 0 && probe.collected);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCCOLLECT);
  for (i = 0; i < 3; i++) {
    if (i == 0 && probe.missing) {
      require(lua_rawgeti(L, 1, probe.keys[i]) == LUA_TNIL);
      lua_pop(L, 1);
      continue;
    }
    require(lua_rawgeti(L, 1, probe.keys[i]) ==
            (probe.graph ? LUA_TTABLE : LUA_TNUMBER));
    if (probe.graph)
      require(lua_rawgeti(L, -1, 1) == LUA_TNUMBER);
    require(lua_tointeger(L, -1) == (lua_Integer)(i + 1) * 11);
    lua_pop(L, probe.graph ? 2 : 1);
  }
  lua_close(L);
  require(pthread_cond_destroy(&probe.changed) == 0);
  require(pthread_mutex_destroy(&probe.mutex) == 0);
  return 0;
}
