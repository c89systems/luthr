#define _GNU_SOURCE
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <luthrum/lua.h>
#include <pthread.h>
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
enum { WORKERS = 2, ROUNDS = 128 };
struct fixture {
  lua_State *main, *children[WORKERS];
  pthread_barrier_t phase, head_read;
  const char *mode;
  size_t live;
  unsigned deny, denied, finalized, entered, hooks;
  int facade, registry;
};
struct actor {
  struct fixture *test;
  unsigned id;
};
static struct fixture *active;
static __thread int interleave;
static void meet(pthread_barrier_t *barrier) {
  int status = pthread_barrier_wait(barrier);
  require(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
}
int __real_lua_rawgeti(lua_State *, int, lua_Integer);
int __wrap_lua_rawgeti(lua_State *state, int table, lua_Integer key) {
  int result = __real_lua_rawgeti(state, table, key);
  /* Force a contender between the old public free-list read and its write.
   * The observable requirement is distinct references and intact values. */
  if (interleave && key == 1)
    meet(&active->head_read);
  return result;
}
static int reference(lua_State *state, int table) {
  return active->facade ? luthrum_lua_l_ref((luthrum_lua_state *)state, table)
                        : luaL_ref(state, table);
}
static void unreference(lua_State *state, int table, int ref) {
  if (active->facade)
    luthrum_lua_l_unref((luthrum_lua_state *)state, table, ref);
  else
    luaL_unref(state, table, ref);
}
static void *allocate(void *context, void *old, size_t old_size, size_t size) {
  struct fixture *test = context;
  void *memory;
  if (old == NULL)
    old_size = 0;
  if (size == 0) {
    test->live -= old_size;
    free(old);
    return NULL;
  }
  if (size > old_size && test->deny) {
    test->deny--;
    test->denied++;
    return NULL;
  }
  memory = malloc(size);
  if (memory) {
    if (old) {
      memcpy(memory, old, old_size < size ? old_size : size);
      free(old);
    }
    test->live = test->live - old_size + size;
  }
  return memory;
}
static int marker_gc(lua_State *state) {
  (void)state;
  active->finalized++;
  return 0;
}
static int once(lua_State *state) {
  int ref;
  active->entered++;
  ref = reference(state, 1);
  lua_pushinteger(state, ref);
  return 1;
}
static void push_value(lua_State *state, unsigned value) {
  lua_createtable(state, 0, 1);
  lua_pushinteger(state, value);
  lua_setfield(state, -2, "id");
}
static void verify(lua_State *state, int table, int ref, unsigned expected) {
  require(lua_rawgeti(state, table, ref) == LUA_TTABLE);
  lua_getfield(state, -1, "id");
  require(lua_tointeger(state, -1) == expected);
  lua_pop(state, 2);
}
static void *parallel(void *context) {
  struct actor *actor = context;
  struct fixture *test = actor->test;
  lua_State *state = test->children[actor->id];
  for (unsigned n = 0; n < ROUNDS; ++n) {
    unsigned expected = (n + 1) * WORKERS + actor->id;
    int table = test->registry ? LUA_REGISTRYINDEX : n % 2 ? 1 : -2;
    int ref;
    push_value(state, expected);
    meet(&test->phase);
    interleave = 1;
    ref = reference(state, table);
    interleave = 0;
    require(ref >= 2 && lua_gettop(state) == 1);
    meet(&test->phase);
    if (actor->id == 0 && n % 16 == 0)
      lua_gc(state, LUA_GCCOLLECT);
    meet(&test->phase);
    table = test->registry ? LUA_REGISTRYINDEX : 1;
    verify(state, table, ref, expected);
    meet(&test->phase);
    interleave = 1;
    unreference(state, test->registry ? LUA_REGISTRYINDEX : -1, ref);
    interleave = 0;
    require(lua_gettop(state) == 1);
    meet(&test->phase);
  }
  return NULL;
}
static void *after_failure(void *context) {
  lua_State *state = context;
  int ref;
  push_value(state, 99);
  ref = reference(state, 1);
  verify(state, 1, ref, 99);
  unreference(state, 1, ref);
  return NULL;
}
static void count_hook(lua_State *state, lua_Debug *debug) {
  int top = lua_gettop(state), ref;
  (void)debug;
  lua_pushinteger(state, 42);
  ref = reference(state, LUA_REGISTRYINDEX);
  require(lua_gettop(state) == top);
  lua_rawgeti(state, LUA_REGISTRYINDEX, ref);
  require(lua_tointeger(state, -1) == 42);
  lua_pop(state, 1);
  unreference(state, LUA_REGISTRYINDEX, ref);
  require(lua_gettop(state) == top);
  active->hooks++;
}
static void *run(void *context) {
  struct fixture *test = context;
  lua_State *state = lua_newstate(allocate, test, 0);
  pthread_t threads[WORKERS];
  struct actor actors[WORKERS] = {{test, 0}, {test, 1}};
  require(state != NULL);
  test->main = state;
  luaL_openlibs(state);
  lua_pushnil(state);
  require(reference(state, 9999) == LUA_REFNIL && lua_gettop(state) == 0);
  unreference(state, 9999, LUA_REFNIL);
  unreference(state, 9999, LUA_NOREF);
  lua_createtable(state, 1, 0);
  lua_pushinteger(state, 0);
  lua_rawseti(state, -2, 1);
  lua_setglobal(state, "references");
  for (unsigned n = 0; n < WORKERS; ++n) {
    test->children[n] = lua_newthread(state);
    lua_getglobal(test->children[n], "references");
  }
  if (strstr(test->mode, "oom")) {
    lua_State *child = test->children[0];
    lua_settop(child, 0);
    lua_pushcfunction(child, once);
    lua_getglobal(child, "references");
    lua_pushnil(child);
    require(lua_pcall(child, 2, 1, 0) == LUA_OK);
    require(lua_tointeger(child, -1) == LUA_REFNIL);
    lua_settop(child, 0);
    lua_pushcfunction(child, once);
    lua_getglobal(child, "references");
    lua_newuserdatauv(child, 1, 0);
    lua_newtable(child);
    lua_pushcfunction(child, marker_gc);
    lua_setfield(child, -2, "__gc");
    lua_setmetatable(child, -2);
    test->deny = 2;
    require(lua_pcall(child, 2, 1, 0) == LUA_ERRMEM);
    require(test->denied == 2 && test->entered == 2 && lua_gettop(child) == 1);
    test->deny = 0;
    lua_settop(child, 0);
    lua_gc(state, LUA_GCCOLLECT);
    lua_gc(state, LUA_GCCOLLECT);
    require(test->finalized == 1);
    require(pthread_create(&threads[1], NULL, after_failure,
                           test->children[1]) == 0);
    require(pthread_join(threads[1], NULL) == 0);
  } else if (strstr(test->mode, "hook")) {
    lua_State *child = test->children[0];
    lua_settop(child, 0);
    lua_sethook(child, count_hook, LUA_MASKCOUNT, 1);
    require(luaL_loadstring(
                child, "local t={}; for n=1,100 do t[n]=n end; return 42") ==
            LUA_OK);
    require(lua_pcall(child, 0, 1, 0) == LUA_OK &&
            lua_tointeger(child, -1) == 42);
    lua_sethook(child, NULL, 0, 0);
    require(test->hooks > 100);
  } else {
    require(pthread_barrier_init(&test->phase, NULL, WORKERS) == 0);
    require(pthread_barrier_init(&test->head_read, NULL, WORKERS) == 0);
    for (unsigned n = 0; n < WORKERS; ++n)
      require(pthread_create(&threads[n], NULL, parallel, &actors[n]) == 0);
    for (unsigned n = 0; n < WORKERS; ++n)
      require(pthread_join(threads[n], NULL) == 0);
    require(pthread_barrier_destroy(&test->head_read) == 0);
    require(pthread_barrier_destroy(&test->phase) == 0);
  }
  lua_close(state);
  require(test->live == 0);
  return NULL;
}
int main(int argc, char **argv) {
  struct fixture *test = calloc(1, sizeof(*test));
  pthread_t worker;
  require(test != NULL && argc == 2);
  active = test;
  test->mode = argv[1];
  test->facade = strstr(argv[1], "facade") != NULL;
  test->registry = strstr(argv[1], "registry") != NULL;
  require(pthread_create(&worker, NULL, run, test) == 0);
  require(pthread_join(worker, NULL) == 0);
  free(test);
  return 0;
}
