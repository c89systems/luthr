#include <lauxlib.h>
#include <ldo.h>
#include <lpipe.h>
#include <ltask.h>
#include <lualib.h>
#include <luthrum/lua_runtime.h>
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
struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *main;
  luthrum_lua_runtime *runtime;
  LuaTaskRoot tasks[3];
  LuaExecution executions[3];
  const char *mode;
  size_t live;
  unsigned failures, closed, finalized, attempts;
  int deny, waiting[2], before, before_release, notify_entered, notify_release;
  int arm_notify, retiring, status[3], results[3];
  int returned, finish;
  int native_caught, native_release, cleanup_blocked;
};
struct actor {
  struct fixture *test;
  int id;
};
static struct fixture *active;
static __thread int native_marker;
static void lock(struct fixture *test) {
  require(pthread_mutex_lock(&test->mutex) == 0);
}
static void unlock(struct fixture *test) {
  require(pthread_mutex_unlock(&test->mutex) == 0);
}
static void notify(struct fixture *test) {
  require(pthread_cond_broadcast(&test->changed) == 0);
}
static void waitfor(struct fixture *test) {
  require(pthread_cond_wait(&test->changed, &test->mutex) == 0);
}
int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int __wrap_pthread_cond_wait(pthread_cond_t *condition,
                             pthread_mutex_t *mutex) {
  LuaExecution *execution = luaQ_current();
  int id = execution == &active->executions[0]   ? 0
           : execution == &active->executions[1] ? 1
                                                 : -1;
  if (id >= 0 && condition != &active->changed) {
    LuaWorld *world = &G(active->main)->world;
    int same = mutex == &world->mutex;
    int registered;
    if (!same)
      require(pthread_mutex_lock(&world->mutex) == 0);
    registered = active->tasks[id].stop_wait != NULL;
    if (!same)
      require(pthread_mutex_unlock(&world->mutex) == 0);
    lock(active);
    if (registered)
      active->waiting[id] = 1;
    else if (same && id == 0)
      active->retiring = 1;
    else if (id == 0 && strstr(active->mode, "cleanup"))
      active->cleanup_blocked = 1;
    notify(active);
    unlock(active);
  }
  return __real_pthread_cond_wait(condition, mutex);
}
int __real_luaQ_waitstop(lua_State *, pthread_mutex_t *, pthread_cond_t *,
                         LuaWaiter **);
int __wrap_luaQ_waitstop(lua_State *state, pthread_mutex_t *mutex,
                         pthread_cond_t *changed, LuaWaiter **waiters) {
  if (strstr(active->mode, "prestop") &&
      luaQ_current() == &active->executions[0]) {
    lock(active);
    active->before = 1;
    notify(active);
    while (!active->before_release)
      waitfor(active);
    unlock(active);
  }
  return __real_luaQ_waitstop(state, mutex, changed, waiters);
}
void __real_luaQ_interrupt(LuaStopWait *);
void __wrap_luaQ_interrupt(LuaStopWait *wait) {
  lock(active);
  if (active->arm_notify) {
    active->arm_notify = 0;
    active->notify_entered = 1;
    notify(active);
    while (!active->notify_release)
      waitfor(active);
  }
  unlock(active);
  __real_luaQ_interrupt(wait);
}
static void *allocate(void *context, void *old, size_t old_size, size_t size) {
  struct fixture *test = context;
  void *memory;
  if (old == NULL)
    old_size = 0;
  if (!size) {
    test->live -= old_size;
    free(old);
    return NULL;
  }
  if (test->deny)
    test->attempts++;
  if (test->deny && size > old_size) {
    test->failures++;
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
static void *host_allocate(void *context, size_t size) {
  return allocate(context, NULL, 0, size);
}
static void host_free(void *context, void *p, size_t size) {
  (void)allocate(context, p, size, 0);
}
static int capture(void *state) {
  active->main = state;
  return 0;
}
static void execute(lua_State *state, const char *source) {
  int status = luaL_loadstring(state, source);
  if (status == LUA_OK)
    status = lua_pcall(state, 0, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "stop wait: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
}
static int cleanup(lua_State *state) {
  (void)state;
  active->closed++;
  return 0;
}
static int finalize(lua_State *state) {
  (void)state;
  active->finalized++;
  return 0;
}
static int await_target(lua_State *state) {
  int status = -99, results = -99;
  require(luaTK_await(state, &active->tasks[2], &status, &results) == LUA_OK);
  require(status == LUA_OK && results == 1);
  return 1;
}
static int pinned(lua_State *state) {
  pthread_t carrier = pthread_self();
  require(luaQ_current()->native_depth != 0);
  native_marker = 321;
  lua_getglobal(state, "q");
  lua_getfield(state, -1, "recv");
  lua_insert(state, -2);
  require(lua_pcall(state, 1, 1, 0) == LUA_ERRRUN &&
          strcmp(lua_tostring(state, -1), "task stopped") == 0);
  lua_pop(state, 1);
  lock(active);
  active->native_caught = 1;
  notify(active);
  while (!active->native_release)
    waitfor(active);
  unlock(active);
  require(pthread_equal(carrier, pthread_self()) && native_marker == 321);
  return 0;
}
static void admit(struct fixture *test, int id, const char *source) {
  lua_State *child = lua_newthread(test->main);
  require(luaL_loadstring(child, source) == LUA_OK);
  require(luaTK_admit(child, &test->tasks[id], &test->executions[id]) ==
          LUA_OK);
  lua_settop(test->main, 0);
}
static void *run_actor(void *context) {
  struct actor *actor = context;
  struct fixture *test = actor->test;
  int id = actor->id;
  NativeStackContext native = *luaE_getnativestackcontext(), *previous;
  native.execution = &test->executions[id];
  previous = luaE_setnativestackcontext(&native);
  if (id == 0 && strstr(test->mode, "driver")) {
    int status = -99, results = -99;
    require(luaTK_await(test->tasks[0].thread, &test->tasks[2], &status,
                        &results) == LUA_TK_STOPPED);
    require(status == -99 && results == -99 &&
            test->tasks[0].phase == LUA_TASK_ADMITTED);
  }
  test->status[id] = luaTK_resume(&test->tasks[id], 0, &test->results[id]);
  require(test->status[id] == (id == 0 ? LUA_ERRRUN : LUA_OK));
  if (id == 0 && strstr(test->mode, "oom")) {
    lock(test);
    test->returned = 1;
    notify(test);
    while (!test->finish)
      waitfor(test);
    unlock(test);
  }
  require(luaTK_finish(&test->tasks[id], &test->status[id],
                       &test->results[id]) == LUA_OK);
  require(test->executions[id].managed_depth == 0 &&
          test->executions[id].native_depth == 0);
  require(test->tasks[id].stop_wait == NULL);
  luaE_setnativestackcontext(previous);
  return NULL;
}
static void *request(void *context) {
  struct fixture *test = context;
  require(luaTK_requeststop(&test->tasks[0]) == LUA_OK);
  return NULL;
}
static void gc(struct fixture *test) {
  lua_gc(test->main, LUA_GCCOLLECT);
  lua_gc(test->main, LUA_GCCOLLECT);
}
static void collect(struct fixture *test, int id, int stopped) {
  int status = -99, results = -99;
  require(luaTK_collect(test->main, &test->tasks[id], &status, &results) ==
          LUA_OK);
  require(status == (stopped ? LUA_ERRRUN : LUA_OK) && results == 1);
  if (stopped)
    require(strcmp(lua_tostring(test->main, -1), "task stopped") == 0);
  else
    require(lua_tointeger(test->main, -1) == 42 ||
            strcmp(lua_tostring(test->main, -1), "peer") == 0);
  lua_pop(test->main, 1);
}
static void *run(void *context) {
  struct fixture *test = context;
  struct actor actors[3] = {{test, 0}, {test, 1}, {test, 2}};
  pthread_t receiver, peer, requester;
  int paired = strstr(test->mode, "peer") != NULL;
  int prestop = strstr(test->mode, "prestop") != NULL;
  int race = strstr(test->mode, "race") != NULL;
  int driver = strstr(test->mode, "driver") != NULL;
  int awaiting = driver || strstr(test->mode, "await") != NULL;
  int send = strstr(test->mode, "send") != NULL;
  int denial = strstr(test->mode, "oom") != NULL;
  int pin = strstr(test->mode, "pinned") != NULL;
  int cleaning = strstr(test->mode, "cleanup") != NULL;
  lua_State *state;
  require(pthread_mutex_init(&test->mutex, NULL) == 0);
  require(pthread_cond_init(&test->changed, NULL) == 0);
  if (strstr(test->mode, "facade")) {
    static const unsigned char source[] = "require('fixture')";
    luthrum_lua_runtime_allocator_config config = {0};
    config.user = test;
    config.alloc_fn = host_allocate;
    config.realloc_fn = allocate;
    config.free_fn = host_free;
    require(luthrum_lua_runtime_new_with_allocator(&test->runtime, &config) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_openlibs(test->runtime) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_register_c_module(
                test->runtime, "fixture", capture) == LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_run_buffer(test->runtime, source,
                                           sizeof(source) - 1, "stop-wait", 0,
                                           NULL, 0) == LUTHRUM_LUA_RUNTIME_OK);
  } else {
    test->main = lua_newstate(allocate, test, 0);
    require(test->main != NULL);
    luaL_openlibs(test->main);
  }
  state = test->main;
  if (strstr(test->mode, "generational"))
    lua_gc(state, LUA_GCGEN);
  luaQ_pushmanaged(state, cleanup, 0);
  lua_setglobal(state, "cleanup");
  luaQ_pushmanaged(state, finalize, 0);
  lua_setglobal(state, "finalize");
  luaQ_pushmanaged(state, await_target, 0);
  lua_setglobal(state, "await_target");
  lua_pushcfunction(state, pinned);
  lua_setglobal(state, "pinned");
  lua_pushboolean(state, cleaning);
  lua_setglobal(state, "cleaning");
  luaP_newpipe(state, 1);
  lua_setglobal(state, "q");
  execute(state, "token=setmetatable({}, {__gc=finalize})");
  if (send)
    execute(state, "q:send('original')");
  if (driver)
    admit(test, 0, "local t=token; token=nil; return 42");
  else {
    const char *body = pin        ? "return pinned()"
                       : awaiting ? "return await_target()"
                       : send     ? "return q:send('pending')"
                                  : "return q:recv()";
    char *source = malloc(strlen(body) + 300);
    require(source != NULL);
    require(
        sprintf(source,
                "local t=token; token=nil; local r <close> =setmetatable({}, "
                "{__close=function() if cleaning then "
                "assert(q:recv()=='cleanup') end; cleanup(t) end}); %s",
                body) > 0);
    admit(test, 0, source);
    free(source);
  }
  admit(test, 1, paired ? "return q:recv()" : "return 42");
  admit(test, 2, "return 42");
  if (paired && strstr(test->mode, "head")) {
    require(pthread_create(&peer, NULL, run_actor, &actors[1]) == 0);
    lock(test);
    while (!test->waiting[1])
      waitfor(test);
    unlock(test);
  }
  require(pthread_create(&receiver, NULL, run_actor, &actors[0]) == 0);
  lock(test);
  while (prestop ? !test->before : !test->waiting[0])
    waitfor(test);
  unlock(test);
  if (paired && !strstr(test->mode, "head")) {
    require(pthread_create(&peer, NULL, run_actor, &actors[1]) == 0);
    lock(test);
    while (!test->waiting[1])
      waitfor(test);
    unlock(test);
  } else if (!paired) {
    run_actor(&actors[1]);
    collect(test, 1, 0);
  }
  gc(test);
  require(test->closed == 0 && test->finalized == 0);
  if (denial)
    test->deny = 1;
  if (race) {
    lock(test);
    test->arm_notify = 1;
    unlock(test);
    require(pthread_create(&requester, NULL, request, test) == 0);
    lock(test);
    while (!test->notify_entered)
      waitfor(test);
    unlock(test);
    if (strstr(test->mode, "repeat"))
      require(luaTK_requeststop(&test->tasks[0]) == LUA_OK);
    if (strstr(test->mode, "finalize"))
      execute(state, "q:close(); debug.getmetatable(q).__gc(q)");
    else if (strstr(test->mode, "close"))
      execute(state, "q:close()");
    else
      execute(state, "q:send('normal')");
    lock(test);
    while (!test->retiring)
      waitfor(test);
    unlock(test);
    gc(test);
    require(test->closed == 0 && test->finalized == 0);
    if (strstr(test->mode, "repeat"))
      require(luaTK_requeststop(&test->tasks[0]) == LUA_OK);
    lock(test);
    test->notify_release = 1;
    notify(test);
    unlock(test);
    require(pthread_join(requester, NULL) == 0);
  } else {
    require(luaTK_requeststop(&test->tasks[0]) == LUA_OK);
    if (prestop) {
      lock(test);
      test->before_release = 1;
      notify(test);
      unlock(test);
    }
  }
  if (denial) {
    lock(test);
    while (!test->returned)
      waitfor(test);
    unlock(test);
    require(test->failures == 0 && test->attempts == 0);
    test->deny = 0;
    lock(test);
    test->finish = 1;
    notify(test);
    unlock(test);
  }
  if (pin || cleaning) {
    int status = -99, results = -99;
    lock(test);
    while (pin ? !test->native_caught : !test->cleanup_blocked)
      waitfor(test);
    unlock(test);
    require(luaTK_collect(state, &test->tasks[0], &status, &results) ==
            LUA_TK_PENDING);
    require(status == -99 && results == -99);
    gc(test);
    require(test->closed == 0 && test->finalized == 0);
    if (pin) {
      require(luaTK_requeststop(&test->tasks[0]) == LUA_OK);
      lock(test);
      test->native_release = 1;
      notify(test);
      unlock(test);
    } else {
      require(luaTK_requeststop(&test->tasks[0]) == LUA_TK_INVALID);
      execute(state, "q:send('cleanup')");
    }
  }
  require(pthread_join(receiver, NULL) == 0);
  require(test->closed == (driver ? 0u : 1u));
  collect(test, 0, 1);
  require(luaTK_requeststop(&test->tasks[0]) == LUA_TK_INVALID);
  if (paired) {
    require(pthread_mutex_lock(&G(state)->world.mutex) == 0);
    require(test->tasks[1].stop_wait != NULL);
    require(pthread_mutex_unlock(&G(state)->world.mutex) == 0);
    execute(state, "q:send('peer')");
    require(pthread_join(peer, NULL) == 0);
    collect(test, 1, 0);
  }
  if (awaiting) {
    require(!__atomic_load_n(&test->tasks[2].stop_requested, __ATOMIC_ACQUIRE));
  }
  run_actor(&actors[2]);
  collect(test, 2, 0);
  if (strstr(test->mode, "finalize"))
    execute(
        state,
        "local s=q:stats(); assert(not s.ready and s.waiting_senders==0 and "
        "s.waiting_receivers==0)");
  else if (race && strstr(test->mode, "close"))
    execute(state, "assert(q:recv()==nil)");
  else if (race)
    execute(state, "assert(q:recv()=='normal')");
  else if (send)
    execute(state, "assert(q:recv()=='original')");
  if (!strstr(test->mode, "finalize") && !(race && strstr(test->mode, "close")))
    execute(
        state,
        "q:send('later'); assert(q:recv()=='later'); local "
        "s=q:stats(); assert(s.waiting_senders==0 and s.waiting_receivers==0)");
  execute(state, "token=nil");
  gc(test);
  require(test->finalized == 1 && G(state)->tasks == NULL);
  if (test->runtime)
    luthrum_lua_runtime_free(test->runtime);
  else
    lua_close(state);
  require(test->live == 0);
  require(pthread_cond_destroy(&test->changed) == 0);
  require(pthread_mutex_destroy(&test->mutex) == 0);
  return NULL;
}
int main(int argc, char **argv) {
  struct fixture *test = calloc(1, sizeof(*test));
  pthread_t worker;
  require(test != NULL && argc == 2);
  active = test;
  test->mode = argv[1];
  require(pthread_create(&worker, NULL, run, test) == 0);
  require(pthread_join(worker, NULL) == 0);
  free(test);
  return 0;
}
