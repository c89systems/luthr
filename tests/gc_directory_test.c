#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lstate.h>
#include <lstring.h>
#include <ltable.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define require(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                  \
      abort();                                                                 \
    }                                                                          \
  } while (0)

static lua_State *observed, *first_publisher, *second_publisher;
static unsigned published, unrelated;

void __real_luaM_flushdebt(lua_State *, lua_State *);
void __wrap_luaM_flushdebt(lua_State *actor, lua_State *owner) {
  if (actor == observed && luaW_iscollector(actor)) {
    if (owner == first_publisher)
      published |= 1;
    else if (owner == second_publisher)
      published |= 2;
    else
      unrelated++;
  }
  __real_luaM_flushdebt(actor, owner);
}

static void publish_batches(lua_State *L, int generational) {
  Table *table;
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_createtable(L, 1024, 0);
  for (int i = 1; i <= 1024; i++) {
    lua_newthread(L);
    lua_rawseti(L, 1, i);
  }
  first_publisher = lua_newthread(L);
  second_publisher = lua_newthread(L);
  lua_createtable(second_publisher, 32, 0);
  lua_pushinteger(second_publisher, 43);
  lua_rawseti(second_publisher, 1, 1);
  table = (Table *)lua_topointer(second_publisher, 1);
  lua_gc(L, LUA_GCCOLLECT);

  lua_newtable(first_publisher);
  lua_pushinteger(first_publisher, 42);
  lua_rawseti(first_publisher, 1, 1);
  lua_lock(second_publisher);
  require(first_publisher->new_objects != NULL);
  luaH_resize(second_publisher, table, 64, 0);
  require(second_publisher->new_objects == NULL &&
          second_publisher->storage_retired != NULL);
  observed = second_publisher;
  luaW_stop(second_publisher);
  require(published == 3 && unrelated == 0);
  require(first_publisher->new_objects == NULL &&
          second_publisher->storage_retired == NULL);
  observed = NULL;
  luaW_start(second_publisher);
  lua_unlock(second_publisher);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_rawgeti(first_publisher, 1, 1) == LUA_TNUMBER &&
          lua_tointeger(first_publisher, -1) == 42);
  require(lua_rawgeti(second_publisher, 1, 1) == LUA_TNUMBER &&
          lua_tointeger(second_publisher, -1) == 43);
  lua_close(L);
}

static void invalidate_string_cache(lua_State *L, int generational) {
  lua_State *owner;
  char source[4097];
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  owner = lua_newthread(L);
  require(owner->strcache_epoch == 0);
  memset(source, 'a', sizeof(source) - 1);
  source[sizeof(source) - 1] = '\0';
  for (int i = 0; i < 3; i++) {
    uint64_t epoch;
    const char *value = lua_pushstring(owner, source);
    require(strcmp(value, source) == 0);
    size_t len;
    require(lua_tolstring(owner, -1, &len) == value &&
            len == sizeof(source) - 1);
    epoch = owner->strcache_epoch;
    require(epoch == G(L)->strcache_epoch);
    lua_pop(owner, 1);
    lua_gc(L, LUA_GCCOLLECT);
    require(owner->strcache_epoch == epoch && G(L)->strcache_epoch != epoch);
    source[0]++;
  }
  lua_lock(L);
  luaW_stop(L);
  /* Reuse of epoch one must also invalidate owners that last used it. */
  owner->strcache_epoch = 1;
  G(L)->strcache_epoch = UINT64_MAX;
  luaS_clearcache(G(L));
  require(G(L)->strcache_epoch == 1 && owner->strcache_epoch == 0);
  luaW_start(L);
  lua_unlock(L);
  require(strcmp(lua_pushstring(owner, source), source) == 0);
  lua_close(L);
}

enum { ACTORS = LUA_BARRIER_SHARDS * 2, ROWS = 32, OBJECTS = ACTORS * ROWS };
struct producer {
  lua_State *state;
  GCObject **objects;
  Table *child;
  pthread_barrier_t *start;
  unsigned *done;
};

static void *publish(void *argument) {
  struct producer *producer = argument;
  lua_State *L = producer->state;
  unsigned i;
  int status = pthread_barrier_wait(producer->start);
  require(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
  lua_lock(L);
  for (i = 0; i < ROWS; i++) {
    GCObject *object = producer->objects[i];
    if (producer->child != NULL) {
      TValue value;
      sethvalue(L, &value, producer->child);
      luaH_setint(L, gco2t(object), 2, &value);
    } else {
      require(iswhite(G(L), object));
      luaC_traceobject(L, object);
    }
  }
  lua_unlock(L);
  __atomic_fetch_add(producer->done, 1, __ATOMIC_RELEASE);
  return NULL;
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  struct producer producers[ACTORS];
  pthread_t workers[ACTORS];
  pthread_barrier_t start;
  GCObject **objects = calloc(OBJECTS, sizeof(*objects));
  unsigned i, done = 0;
  Table *child = NULL;
  int back;
  require(argc == 2 && L != NULL && objects != NULL);
  if (strncmp(argv[1], "publish_", 8) == 0) {
    free(objects);
    publish_batches(L, strcmp(argv[1], "publish_generational") == 0);
    return 0;
  }
  if (strncmp(argv[1], "string_cache_", 13) == 0) {
    free(objects);
    invalidate_string_cache(L,
                            strcmp(argv[1], "string_cache_generational") == 0);
    return 0;
  }
  back = strcmp(argv[1], "remembered") == 0;
  lua_gc(L, back ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_createtable(L, OBJECTS, 0);
  for (i = 0; i < OBJECTS; i++) {
    lua_createtable(L, 2, 0);
    objects[i] = (GCObject *)lua_topointer(L, -1);
    lua_pushinteger(L, i + 1);
    lua_rawseti(L, -2, 1);
    lua_rawseti(L, 1, i + 1);
  }
  if (back) {
    lua_gc(L, LUA_GCCOLLECT);
    lua_newtable(L);
    lua_pushinteger(L, 42);
    lua_rawseti(L, -2, 1);
    child = (Table *)lua_topointer(L, -1);
  }
  require(lua_checkstack(L, ACTORS + 1));
  require(pthread_barrier_init(&start, NULL, ACTORS) == 0);
  for (i = 0; i < ACTORS; i++) {
    producers[i].state = lua_newthread(L);
    producers[i].objects = objects + i * ROWS;
    producers[i].child = child;
    producers[i].start = &start;
    producers[i].done = &done;
  }
  lua_lock(L);
  if (!back) {
    luaW_stop(L);
    luaC_runtilstate(L, GCSpropagate, 1);
    require(luaC_tracing(L));
    luaW_start(L);
    for (i = 0; i < OBJECTS; i++)
      require(iswhite(G(L), objects[i]));
  }
  for (i = 0; i < ACTORS; i++)
    require(pthread_create(&workers[i], NULL, publish, &producers[i]) == 0);
  /* More producers than shards guarantee collisions. Consume concurrently,
   * including empty-directory passes and producer publication/clear races. */
  while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) != ACTORS) {
    require(pthread_mutex_lock(&G(L)->world.mutex) == 0);
    luaC_drainbarriers(L);
    require(pthread_mutex_unlock(&G(L)->world.mutex) == 0);
    sched_yield();
  }
  for (i = 0; i < ACTORS; i++)
    require(pthread_join(workers[i], NULL) == 0);
  require(pthread_mutex_lock(&G(L)->world.mutex) == 0);
  luaC_drainbarriers(L);
  require(G(L)->barrier_ready == 0);
  for (i = 0; i < LUA_BARRIER_SHARDS; i++)
    require(G(L)->barriers[i].forward == NULL &&
            G(L)->barriers[i].back == NULL);
  for (i = 0; i < OBJECTS; i++)
    require(isgray(objects[i]));
  require(pthread_mutex_unlock(&G(L)->world.mutex) == 0);
  lua_unlock(L);
  /* Retire every producer root before finishing collection. Published work
   * belongs to the VM, and its target graph must survive producer retirement.
   */
  lua_settop(L, 1);
  lua_gc(L, LUA_GCCOLLECT);
  for (i = 0; i < OBJECTS; i++) {
    require(lua_rawgeti(L, 1, i + 1) == LUA_TTABLE);
    lua_rawgeti(L, -1, 1);
    require(lua_tointeger(L, -1) == (lua_Integer)i + 1);
    lua_pop(L, 1);
    if (back) {
      require(lua_rawgeti(L, -1, 2) == LUA_TTABLE);
      lua_rawgeti(L, -1, 1);
      require(lua_tointeger(L, -1) == 42);
      lua_pop(L, 2);
    }
    lua_pop(L, 1);
  }
  lua_close(L);
  free(objects);
  require(pthread_barrier_destroy(&start) == 0);
  return 0;
}
