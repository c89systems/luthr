#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lstate.h>
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
