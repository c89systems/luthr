#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lstate.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *main;
  lua_State *worker;
  pthread_t collector;
  pthread_t worker_carrier;
  int collector_ready;
  int collect_start;
  int collect_done;
  int active_phase;
  int entered;
  int allow_poll;
  int native_release;
  int native_case;
  int contended_case;
  int worker_collections;
  unsigned ordinary_polls;
  int application_progress;
  int progress_requested;
  int progress_observed;
  int finalized[3];
  int finalizer_polls;
  int stop_ref;
  int worker_done;
  int worker_status;
  int worker_results;
  int failed;
};

struct resource {
  struct fixture *fixture;
  int id;
  unsigned magic;
};

static const char managed_source[] =
    "local stop=false; return function(token) local t={token}; "
    "coroutine.yield(); local n=0; while not stop do "
    "for i=1,2000 do n=n+1 end end; return t[1],n end, "
    "function(value) stop=value end";

static const char native_source[] =
    "return function(token) coroutine.yield(); fixture_wait(token); "
    "return token,1 end, function() end";

static void check(int status) {
  if (status != 0) {
    fprintf(stderr, "GC fixture synchronization failed: %d\n", status);
    abort();
  }
}

static void *allocate(void *context, void *pointer, size_t old_size,
                      size_t size) {
  struct fixture *fixture = context;
  (void)old_size;
  check(pthread_mutex_lock(&fixture->mutex));
  if (fixture->active_phase &&
      __atomic_load_n(&G(fixture->main)->allocator.owner, __ATOMIC_ACQUIRE) !=
          luaQ_ownertoken()) {
    fputs("GC rendezvous: allocator callback without admission\n", stderr);
    abort();
  }
  check(pthread_mutex_unlock(&fixture->mutex));
  if (size == 0) {
    free(pointer);
    return NULL;
  }
  return realloc(pointer, size);
}

static void worker_poll(lua_State *state, void *context) {
  struct fixture *fixture = context;
  (void)state;
  check(pthread_mutex_lock(&fixture->mutex));
  if (!luaC_ingc(state) && ++fixture->ordinary_polls > 1) {
    fixture->application_progress = 1;
    check(pthread_cond_broadcast(&fixture->changed));
  }
  if (!fixture->entered) {
    fixture->entered = 1;
    check(pthread_cond_broadcast(&fixture->changed));
    while (!fixture->allow_poll)
      check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  }
  if (fixture->progress_requested) {
    fixture->progress_observed = 1;
    check(pthread_cond_broadcast(&fixture->changed));
  }
  check(pthread_mutex_unlock(&fixture->mutex));
  if (fixture->contended_case && fixture->worker_collections == 0) {
    fixture->worker_collections++;
    /* Another collector has already requested this pause. Join that request
     * as a pinned inactive mutator, then perform a real competing collection.
     */
    luaC_fullgc(state, 0);
  }
}

static void finalizer_poll(lua_State *state, void *context) {
  struct fixture *fixture = context;
  if (state->world_depth != 0 || luaW_scope(state).heap != 0) {
    fputs("Lua finalizer retained collection or heap ownership\n", stderr);
    abort();
  }
  fixture->finalizer_polls++;
  /* Either collecting execution may claim a finalizer. If the worker runs
   * it, the main execution must still be able to execute the stop setter.
   * Both polling sites witness actual Lua progress outside GC ownership. */
  check(pthread_mutex_lock(&fixture->mutex));
  if (fixture->progress_requested) {
    fixture->progress_observed = 1;
    check(pthread_cond_broadcast(&fixture->changed));
  }
  check(pthread_mutex_unlock(&fixture->mutex));
}

static int finalize(lua_State *state) {
  struct resource *resource = lua_touserdata(state, 1);
  struct fixture *fixture = resource->fixture;
  if (resource->magic != 0x1234abcdU || resource->id < 1 || resource->id > 2 ||
      lua_tointeger(state, 2) != 4501500)
    abort();
  resource->magic = 0;
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->finalized[resource->id]++;
  if (fixture->active_phase && resource->id == 2 && !fixture->native_case) {
    fixture->progress_requested = 1;
    check(pthread_cond_broadcast(&fixture->changed));
    while (!fixture->progress_observed)
      check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  }
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static int native_wait(lua_State *state) {
  struct resource *resource = lua_touserdata(state, 1);
  struct fixture *fixture = resource->fixture;
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->entered = 1;
  check(pthread_cond_broadcast(&fixture->changed));
  while (!fixture->native_release)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  check(pthread_mutex_unlock(&fixture->mutex));
  if (resource->magic != 0x1234abcdU)
    abort();
  return 0;
}

static void push_resource(lua_State *state, struct fixture *fixture, int id) {
  struct resource *resource = lua_newuserdatauv(state, sizeof(*resource), 0);
  resource->fixture = fixture;
  resource->id = id;
  resource->magic = 0x1234abcdU;
  luaL_setmetatable(state, "gc.fixture.resource");
}

static int prepare(struct fixture *fixture, int generational) {
  lua_State *state = fixture->main;
  int results;
  luaL_openlibs(state);
  (void)lua_gc(state, LUA_GCSTOP);
  lua_pushcfunction(state, native_wait);
  lua_setglobal(state, "fixture_wait");
  luaL_newmetatable(state, "gc.fixture.resource");
  if (luaL_loadstring(state, "local native=...; return function(value) "
                             "local n=0; for i=1,3000 do n=n+i end; "
                             "native(value,n) end") != LUA_OK)
    return 1;
  lua_pushcfunction(state, finalize);
  if (lua_pcall(state, 1, 1, 0) != LUA_OK)
    return 1;
  lua_setfield(state, -2, "__gc");
  lua_pop(state, 1);
  if (luaL_loadstring(state, fixture->native_case ? native_source
                                                  : managed_source) != LUA_OK ||
      lua_pcall(state, 0, 2, 0) != LUA_OK)
    return 1;
  fixture->stop_ref = luaL_ref(state, LUA_REGISTRYINDEX);
  fixture->worker = lua_newthread(state);
  lua_pushvalue(state, 1);
  lua_xmove(state, fixture->worker, 1);
  push_resource(state, fixture, 1);
  lua_xmove(state, fixture->worker, 1);
  if (!lua_checkstack(fixture->worker, 128) ||
      lua_resume(fixture->worker, state, 1, &results) != LUA_YIELD || results)
    return 1;
  /* Warm the setter frame without changing the worker's stop condition. */
  lua_rawgeti(state, LUA_REGISTRYINDEX, fixture->stop_ref);
  lua_pushboolean(state, 0);
  if (lua_pcall(state, 1, 0, 0) != LUA_OK)
    return 1;
  if (generational)
    (void)lua_gc(state, LUA_GCGEN);
  push_resource(state, fixture, 2);
  lua_pop(state, 1);
  if (!fixture->native_case) {
    fixture->worker->state_poll = worker_poll;
    fixture->worker->state_poll_context = fixture;
    fixture->worker->poll_remaining = 1;
  }
  state->state_poll = finalizer_poll;
  state->state_poll_context = fixture;
  state->poll_remaining = 1;
  return 0;
}

static void *run_worker(void *context) {
  struct fixture *fixture = context;
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->worker_carrier = pthread_self();
  check(pthread_mutex_unlock(&fixture->mutex));
  fixture->worker_status =
      lua_resume(fixture->worker, NULL, 0, &fixture->worker_results);
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->worker_done = 1;
  check(pthread_cond_broadcast(&fixture->changed));
  check(pthread_mutex_unlock(&fixture->mutex));
  return NULL;
}

static void *run_collector(void *context) {
  struct fixture *fixture = context;
  int i;
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->collector = pthread_self();
  fixture->collector_ready = 1;
  check(pthread_cond_broadcast(&fixture->changed));
  while (!fixture->collect_start)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  check(pthread_mutex_unlock(&fixture->mutex));
  for (i = 0; i < 4; ++i)
    if (lua_gc(fixture->main, LUA_GCCOLLECT) != 0)
      fixture->failed = 1;
  if (!fixture->native_case) {
    /* The worker may reach its blocking finalizer after these collections
     * return. Wait for that request, then execute real Lua on the main state
     * to release it; an earlier setter call cannot witness a future request. */
    check(pthread_mutex_lock(&fixture->mutex));
    while (!fixture->application_progress) {
      if (fixture->progress_requested && !fixture->progress_observed) {
        check(pthread_mutex_unlock(&fixture->mutex));
        lua_lock(fixture->main);
        fixture->main->poll_remaining = 1;
        lua_unlock(fixture->main);
        lua_rawgeti(fixture->main, LUA_REGISTRYINDEX, fixture->stop_ref);
        lua_pushboolean(fixture->main, 0);
        if (lua_pcall(fixture->main, 1, 0, 0) != LUA_OK)
          fixture->failed = 1;
        check(pthread_mutex_lock(&fixture->mutex));
      } else
        check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
    }
    check(pthread_mutex_unlock(&fixture->mutex));
  }
  lua_rawgeti(fixture->main, LUA_REGISTRYINDEX, fixture->stop_ref);
  lua_pushboolean(fixture->main, 1);
  if (lua_pcall(fixture->main, 1, 0, 0) != LUA_OK)
    fixture->failed = 1;
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->collect_done = 1;
  check(pthread_cond_broadcast(&fixture->changed));
  check(pthread_mutex_unlock(&fixture->mutex));
  return NULL;
}

int main(int argc, char **argv) {
  struct fixture fixture;
  pthread_t worker, collector;
  LuaWorld *world;
  struct resource *result;
  int generational;
  if (argc != 2)
    return 2;
  memset(&fixture, 0, sizeof(fixture));
  fixture.native_case = strstr(argv[1], "native") != NULL;
  fixture.contended_case = strstr(argv[1], "contended") != NULL;
  generational = strstr(argv[1], "generational") != NULL;
  check(pthread_mutex_init(&fixture.mutex, NULL));
  check(pthread_cond_init(&fixture.changed, NULL));
  fixture.main = lua_newstate(allocate, &fixture, 0);
  if (fixture.main == NULL || prepare(&fixture, generational))
    return 1;
  check(pthread_create(&collector, NULL, run_collector, &fixture));
  check(pthread_mutex_lock(&fixture.mutex));
  while (!fixture.collector_ready)
    check(pthread_cond_wait(&fixture.changed, &fixture.mutex));
  fixture.active_phase = 1;
  check(pthread_mutex_unlock(&fixture.mutex));
  check(pthread_create(&worker, NULL, run_worker, &fixture));
  check(pthread_mutex_lock(&fixture.mutex));
  while (!fixture.entered)
    check(pthread_cond_wait(&fixture.changed, &fixture.mutex));
  check(pthread_mutex_unlock(&fixture.mutex));
  /* Remove the last ordinary coroutine root after managed/native entry. The
   * runtime's temporary handoff/native pins must now keep its stack alive. */
  lua_settop(fixture.main, 0);
  check(pthread_mutex_lock(&fixture.mutex));
  fixture.collect_start = 1;
  check(pthread_cond_broadcast(&fixture.changed));
  check(pthread_mutex_unlock(&fixture.mutex));
  if (!fixture.native_case) {
    world = &G(fixture.main)->world;
    check(pthread_mutex_lock(&world->mutex));
    while (!luaW_stopped(world))
      luaQ_wait(fixture.main, &world->mutex, &world->changed, &world->waiters);
    check(pthread_mutex_unlock(&world->mutex));
    check(pthread_mutex_lock(&fixture.mutex));
    fixture.allow_poll = 1;
    check(pthread_cond_broadcast(&fixture.changed));
    check(pthread_mutex_unlock(&fixture.mutex));
  }
  check(pthread_mutex_lock(&fixture.mutex));
  while (!fixture.collect_done)
    check(pthread_cond_wait(&fixture.changed, &fixture.mutex));
  if (fixture.native_case)
    fixture.failed |= fixture.worker_done;
  fixture.native_release = 1;
  check(pthread_cond_broadcast(&fixture.changed));
  check(pthread_mutex_unlock(&fixture.mutex));
  check(pthread_join(collector, NULL));
  check(pthread_join(worker, NULL));
  fixture.active_phase = 0;
  world = &G(fixture.main)->world;
  fixture.failed |=
      luaW_activecount(world) != 0 || luaW_stopped(world) ||
      fixture.worker->native_depth != 0 || fixture.worker->world_depth != 0 ||
      fixture.main->native_depth != 0 || fixture.main->world_depth != 0 ||
      luaW_heapheld(world) || fixture.worker->heap_waiting != 0 ||
      fixture.main->heap_waiting != 0;
  fixture.failed |= fixture.worker_status != LUA_OK ||
                    fixture.worker_results != 2 || fixture.finalized[1] != 0 ||
                    fixture.finalized[2] != 1 || fixture.finalizer_polls == 0;
  if (!fixture.native_case)
    fixture.failed |= !fixture.progress_observed;
  if (fixture.contended_case)
    fixture.failed |= fixture.worker_collections != 1;
  result = lua_touserdata(fixture.worker, -2);
  fixture.failed |= result == NULL || result->magic != 0x1234abcdU ||
                    lua_tointeger(fixture.worker, -1) <= 0;
  lua_close(fixture.main);
  fixture.failed |= fixture.finalized[1] != 1 || fixture.finalized[2] != 1;
  check(pthread_cond_destroy(&fixture.changed));
  check(pthread_mutex_destroy(&fixture.mutex));
  if (!fixture.failed)
    printf("GC rendezvous %s: retained coroutine, completed collection, "
           "finalizers outside pause\n",
           argv[1]);
  return fixture.failed;
}
