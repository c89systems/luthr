#define _GNU_SOURCE
#include <lauxlib.h>
#include <lstate.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture;

struct actor {
  struct fixture *fixture;
  lua_State *state;
  int cpu;
  int affinity_error;
  int polled;
  int nargs;
  int status;
  int results;
  int done;
};

struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_barrier_t managed;
  int ready;
  int start;
  int abort_start;
  int forbid_allocations;
  int native_entered;
  int release_native;
  struct actor actors[2];
};

static const char closed_source[] =
    "local x=7; return "
    "function() local t={0}; coroutine.yield(); "
    "for i=1,5000 do x=(i%2==0) and 7 or 'word'; t[1]=t[1]+1 end; "
    "return t[1] end, "
    "function() local t={0}; coroutine.yield(); local ok=true; "
    "for i=1,5000 do local v=x; if v~=7 and v~='word' then ok=false end; "
    "t[1]=t[1]+1 end; return ok,t[1] end";

static const char open_source[] =
    "return function() local x=0; local t={0}; "
    "local function change() coroutine.yield(); x=42; return true end; "
    "coroutine.yield(change); while x~=42 do t[1]=t[1]+1 end; "
    "return t[1] end";

static const char api_source[] =
    "return function() local x=0; local t={0}; "
    "local function read() return x end; coroutine.yield(read); "
    "while x~=42 do t[1]=t[1]+1 end; return t[1] end";

static const char cycle_source[] =
    "return function() local x=7; local function read() return x end; "
    "read(); local other=coroutine.yield(read); local sum=0; "
    "for i=1,5000 do sum=sum+other() end; return sum end";

static const char blocked_source[] =
    "return function() coroutine.yield(); fixture_wait(); return true end, "
    "function() local t={0}; coroutine.yield(); fixture_entered(); "
    "for i=1,5000 do t[1]=t[1]+1 end; return t[1] end";

static void check(int status) {
  if (status != 0) {
    fprintf(stderr, "parallel fixture synchronization failed: %d\n", status);
    abort();
  }
}

static void *allocate(void *context, void *pointer, size_t old_size,
                      size_t size) {
  struct fixture *fixture = context;
  (void)old_size;
  if (fixture->forbid_allocations) {
    fputs(
        "parallel prototype attempted allocation/free before GC coordination\n",
        stderr);
    abort();
  }
  if (size == 0) {
    free(pointer);
    return NULL;
  }
  return realloc(pointer, size);
}

static void managed_poll(lua_State *state, void *context) {
  struct actor *actor = context;
  (void)state;
  if (!actor->polled) {
    int status;
    actor->polled = 1;
    /* This runs inside VM dispatch with stack ownership retained. It is not a
     * Lua/C callback, which would release managed ownership before entry. */
    status = pthread_barrier_wait(&actor->fixture->managed);
    if (status != 0 && status != PTHREAD_BARRIER_SERIAL_THREAD)
      check(status);
  }
}

static struct fixture *native_context(lua_State *state) {
  return lua_touserdata(state, lua_upvalueindex(1));
}

static int native_wait(lua_State *state) {
  struct fixture *fixture = native_context(state);
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->native_entered = 1;
  check(pthread_cond_broadcast(&fixture->changed));
  while (!fixture->release_native)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static int native_entered(lua_State *state) {
  struct fixture *fixture = native_context(state);
  check(pthread_mutex_lock(&fixture->mutex));
  while (!fixture->native_entered)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static int native_setcapture(lua_State *state) {
  if (lua_getupvalue(state, 1, 1) == NULL || lua_tointeger(state, -1) != 0)
    abort();
  lua_pop(state, 1);
  lua_pushinteger(state, 42);
  if (lua_setupvalue(state, 1, 1) == NULL ||
      lua_getupvalue(state, 1, 1) == NULL || lua_tointeger(state, -1) != 42)
    abort();
  return 0;
}

static void register_native(lua_State *state, const char *name,
                            lua_CFunction operation, struct fixture *fixture) {
  lua_pushlightuserdata(state, fixture);
  lua_pushcclosure(state, operation, 1);
  lua_setglobal(state, name);
}

static lua_State *prepare_actor(lua_State *main, int function_index,
                                struct actor *actor, int expected_results) {
  lua_State *state = lua_newthread(main);
  int results;
  lua_pushvalue(main, function_index);
  lua_xmove(main, state, 1);
  if (!lua_checkstack(state, 128) ||
      lua_resume(state, main, 0, &results) != LUA_YIELD ||
      results != expected_results)
    return NULL;
  actor->state = state;
  return state;
}

static int prepare(lua_State *main, struct fixture *fixture, const char *name) {
  const char *source;
  int results;
  if (strcmp(name, "closed") == 0)
    source = closed_source;
  else if (strcmp(name, "open") == 0)
    source = open_source;
  else if (strcmp(name, "api") == 0)
    source = api_source;
  else if (strcmp(name, "cycle") == 0)
    source = cycle_source;
  else if (strcmp(name, "blocked") == 0)
    source = blocked_source;
  else
    return 1;
  register_native(main, "fixture_wait", native_wait, fixture);
  register_native(main, "fixture_entered", native_entered, fixture);
  lua_pushcfunction(main, native_setcapture);
  lua_setglobal(main, "fixture_setcapture");
  if (luaL_loadstring(main, source) != LUA_OK ||
      lua_pcall(main, 0, LUA_MULTRET, 0) != LUA_OK)
    return 1;
  results = lua_gettop(main);
  if (strcmp(name, "open") == 0 || strcmp(name, "api") == 0) {
    lua_State *owner = prepare_actor(main, 1, &fixture->actors[0], 1);
    int reader_index;
    if (owner == NULL)
      return 1;
    lua_xmove(owner, main, 1);
    reader_index = lua_gettop(main);
    if (strcmp(name, "api") == 0) {
      if (luaL_loadstring(main,
                          "local capture=...; return function() "
                          "coroutine.yield(); fixture_setcapture(capture); "
                          "return true end") != LUA_OK)
        return 1;
      lua_pushvalue(main, reader_index);
      if (lua_pcall(main, 1, 1, 0) != LUA_OK)
        return 1;
      reader_index = lua_gettop(main);
    }
    if (prepare_actor(main, reader_index, &fixture->actors[1], 0) == NULL)
      return 1;
  } else if (strcmp(name, "cycle") == 0) {
    int reader_refs[2];
    int i;
    for (i = 0; i < 2; ++i) {
      lua_State *owner = prepare_actor(main, 1, &fixture->actors[i], 1);
      if (owner == NULL)
        return 1;
      lua_xmove(owner, main, 1);
      reader_refs[i] = luaL_ref(main, LUA_REGISTRYINDEX);
    }
    for (i = 0; i < 2; ++i) {
      lua_rawgeti(main, LUA_REGISTRYINDEX, reader_refs[1 - i]);
      lua_xmove(main, fixture->actors[i].state, 1);
      fixture->actors[i].nargs = 1;
    }
  } else {
    if (results != 2)
      return 1;
    if (prepare_actor(main, 1, &fixture->actors[0], 0) == NULL ||
        prepare_actor(main, 2, &fixture->actors[1], 0) == NULL)
      return 1;
  }
  return 0;
}

static void *run_actor(void *context) {
  struct actor *actor = context;
  struct fixture *fixture = actor->fixture;
  cpu_set_t selected;
  CPU_ZERO(&selected);
  CPU_SET(actor->cpu, &selected);
  actor->affinity_error =
      pthread_setaffinity_np(pthread_self(), sizeof(selected), &selected);
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->ready++;
  check(pthread_cond_broadcast(&fixture->changed));
  while (!fixture->start && !fixture->abort_start)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  if (fixture->abort_start) {
    check(pthread_mutex_unlock(&fixture->mutex));
    return NULL;
  }
  check(pthread_mutex_unlock(&fixture->mutex));
  actor->status = lua_resume(actor->state, NULL, actor->nargs, &actor->results);
  check(pthread_mutex_lock(&fixture->mutex));
  actor->done = 1;
  check(pthread_cond_broadcast(&fixture->changed));
  check(pthread_mutex_unlock(&fixture->mutex));
  return NULL;
}

static int verify_results(struct fixture *fixture, const char *name) {
  struct actor *first = &fixture->actors[0];
  struct actor *second = &fixture->actors[1];
  int i;
  for (i = 0; i < 2; ++i) {
    struct actor *actor = &fixture->actors[i];
    if (actor->status != LUA_OK || !actor->polled) {
      fprintf(stderr, "%s actor %d: status=%d poll=%d\n", name, i,
              actor->status, actor->polled);
      return 1;
    }
  }
  if (strcmp(name, "closed") == 0)
    return first->results != 1 || lua_tointeger(first->state, -1) != 5000 ||
           second->results != 2 || !lua_toboolean(second->state, -2) ||
           lua_tointeger(second->state, -1) != 5000;
  if (strcmp(name, "open") == 0 || strcmp(name, "api") == 0)
    return first->results != 1 || lua_tointeger(first->state, -1) < 0 ||
           second->results != 1 || !lua_toboolean(second->state, -1);
  if (strcmp(name, "cycle") == 0)
    return first->results != 1 || second->results != 1 ||
           lua_tointeger(first->state, -1) != 35000 ||
           lua_tointeger(second->state, -1) != 35000;
  return first->results != 1 || !lua_toboolean(first->state, -1) ||
         second->results != 1 || lua_tointeger(second->state, -1) != 5000;
}

int main(int argc, char **argv) {
  struct fixture fixture;
  pthread_t threads[2];
  cpu_set_t available;
  lua_State *main_state;
  int cpus[2] = {-1, -1};
  int cpu, i, created = 0, failed = 0;
  if (argc != 2)
    return 2;
  memset(&fixture, 0, sizeof(fixture));
  if (sched_getaffinity(0, sizeof(available), &available) != 0)
    return 1;
  for (cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &available)) {
      if (cpus[0] < 0)
        cpus[0] = cpu;
      else {
        cpus[1] = cpu;
        break;
      }
    }
  }
  if (cpus[0] < 0)
    return 1;
  if (cpus[1] < 0)
    cpus[1] = cpus[0];
  check(pthread_mutex_init(&fixture.mutex, NULL));
  check(pthread_cond_init(&fixture.changed, NULL));
  check(pthread_barrier_init(&fixture.managed, NULL, 2));
  main_state = lua_newstate(allocate, &fixture, 0);
  if (main_state == NULL)
    return 1;
  luaL_openlibs(main_state);
  (void)lua_gc(main_state, LUA_GCSTOP);
  G(main_state)->gcstopem = 1;
  if (prepare(main_state, &fixture, argv[1]) != 0) {
    fputs("parallel fixture preparation failed\n", stderr);
    failed = 1;
    goto cleanup;
  }
  for (i = 0; i < 2; ++i) {
    struct actor *actor = &fixture.actors[i];
    if (G(actor->state) != G(main_state))
      abort();
    actor->fixture = &fixture;
    actor->cpu = cpus[i];
    actor->state->state_poll = managed_poll;
    actor->state->state_poll_context = actor;
    actor->state->poll_remaining = 1;
    if (pthread_create(&threads[i], NULL, run_actor, actor) != 0) {
      failed = 1;
      break;
    }
    created++;
  }
  check(pthread_mutex_lock(&fixture.mutex));
  while (!failed && fixture.ready != 2)
    check(pthread_cond_wait(&fixture.changed, &fixture.mutex));
  for (i = 0; i < created; ++i)
    failed |= fixture.actors[i].affinity_error != 0;
  fixture.forbid_allocations = !failed;
  fixture.abort_start = failed;
  fixture.start = !failed;
  check(pthread_cond_broadcast(&fixture.changed));
  check(pthread_mutex_unlock(&fixture.mutex));
  if (!failed && strcmp(argv[1], "blocked") == 0) {
    check(pthread_mutex_lock(&fixture.mutex));
    while (!fixture.native_entered)
      check(pthread_cond_wait(&fixture.changed, &fixture.mutex));
    check(pthread_mutex_unlock(&fixture.mutex));
    check(pthread_join(threads[1], NULL));
    check(pthread_mutex_lock(&fixture.mutex));
    failed |= fixture.actors[0].done;
    fixture.release_native = 1;
    check(pthread_cond_broadcast(&fixture.changed));
    check(pthread_mutex_unlock(&fixture.mutex));
    check(pthread_join(threads[0], NULL));
  } else {
    for (i = 0; i < created; ++i)
      check(pthread_join(threads[i], NULL));
  }
  fixture.forbid_allocations = 0;
  if (!failed)
    failed = verify_results(&fixture, argv[1]);
  if (!failed)
    printf("parallel VM %s: shared VM, CPU %d/%d, managed overlap, no "
           "allocation\n",
           argv[1], cpus[0], cpus[1]);
cleanup:
  G(main_state)->gcstopem = 0;
  lua_close(main_state);
  check(pthread_barrier_destroy(&fixture.managed));
  check(pthread_cond_destroy(&fixture.changed));
  check(pthread_mutex_destroy(&fixture.mutex));
  return failed;
}
