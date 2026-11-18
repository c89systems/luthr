#include <errno.h>
#include <lauxlib.h>
#include <lua.h>
#include <luthr/lua_runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_mutex;
static int fail_condition;
static int failures;
static int live_mutexes;
static int live_conditions;
static size_t live_bytes;

int __real_pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int __real_pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
int __real_pthread_mutex_destroy(pthread_mutex_t *);
int __real_pthread_cond_destroy(pthread_cond_t *);

int __wrap_pthread_mutex_init(pthread_mutex_t *mutex,
                              const pthread_mutexattr_t *attributes) {
  int status;
  if (fail_mutex && --fail_mutex == 0) {
    ++failures;
    return EAGAIN;
  }
  status = __real_pthread_mutex_init(mutex, attributes);
  if (status == 0)
    ++live_mutexes;
  return status;
}

int __wrap_pthread_cond_init(pthread_cond_t *condition,
                             const pthread_condattr_t *attributes) {
  int status;
  if (fail_condition && --fail_condition == 0) {
    ++failures;
    return EAGAIN;
  }
  status = __real_pthread_cond_init(condition, attributes);
  if (status == 0)
    ++live_conditions;
  return status;
}

int __wrap_pthread_mutex_destroy(pthread_mutex_t *mutex) {
  int status = __real_pthread_mutex_destroy(mutex);
  if (status == 0)
    --live_mutexes;
  return status;
}

int __wrap_pthread_cond_destroy(pthread_cond_t *condition) {
  int status = __real_pthread_cond_destroy(condition);
  if (status == 0)
    --live_conditions;
  return status;
}

static void *allocate(void *context, void *pointer, size_t old_size,
                      size_t size) {
  void *result;
  (void)context;
  if (pointer == NULL)
    old_size = 0;
  if (size == 0) {
    live_bytes -= old_size;
    free(pointer);
    return NULL;
  }
  result = realloc(pointer, size);
  if (result != NULL)
    live_bytes = live_bytes - old_size + size;
  return result;
}

static int create_thread(lua_State *state) {
  (void)lua_newthread(state);
  return 1;
}

static void *facade_alloc(void *context, size_t size) {
  return allocate(context, NULL, 0, size);
}

static void facade_free(void *context, void *pointer, size_t size) {
  (void)allocate(context, pointer, size, 0);
}

static int facade_case(const char *name) {
  luthr_lua_runtime_allocator_config config;
  luthr_lua_runtime *runtime = NULL;
  int failed;
  memset(&config, 0, sizeof(config));
  config.alloc_fn = facade_alloc;
  config.realloc_fn = allocate;
  config.free_fn = facade_free;
  if (strcmp(name, "facade_policy_mutex") == 0)
    fail_mutex = 1;
  else if (strcmp(name, "facade_trace_mutex") == 0)
    fail_mutex = 2;
  else if (strcmp(name, "facade_allocator_mutex") == 0)
    fail_mutex = 3;
  else if (strcmp(name, "facade_state_mutex") == 0)
    fail_mutex = 4;
  else if (strcmp(name, "facade_world_mutex") == 0)
    fail_mutex = 5;
  else
    return 1;
  failed = luthr_lua_runtime_new_with_allocator(&runtime, &config) !=
               LUTHR_LUA_RUNTIME_ERR_ALLOC ||
           runtime != NULL || failures != 1 || live_mutexes != 0 ||
           live_conditions != 0 || live_bytes != 0;
  fail_mutex = 0;
  if (runtime != NULL)
    luthr_lua_runtime_free(runtime);
  runtime = NULL;
  failed |= luthr_lua_runtime_new_with_allocator(&runtime, &config) !=
            LUTHR_LUA_RUNTIME_OK;
  luthr_lua_runtime_free(runtime);
  return failed || live_mutexes != 0 || live_conditions != 0 || live_bytes != 0;
}

static int run_case(const char *name) {
  lua_State *state;
  int status = LUA_OK;
  int failed = 0;
  int world_case = strncmp(name, "world_", 6) == 0;
  int allocator_case = strncmp(name, "allocator_", 10) == 0;
  int main_case =
      strncmp(name, "main_", 5) == 0 || world_case || allocator_case;
  int condition_case = strstr(name, "condition") != NULL;
  if (strncmp(name, "facade_", 7) == 0)
    return facade_case(name);
  if (main_case) {
    int position = allocator_case ? 1 : world_case ? 3 : 2;
    fail_mutex = condition_case ? 0 : position;
    fail_condition = condition_case ? position : 0;
  }
  state = lua_newstate(allocate, NULL, 0);
  if (main_case) {
    failed = state != NULL;
  } else if (state == NULL) {
    return 1;
  } else if (strncmp(name, "thread_", 7) == 0) {
    lua_pushcfunction(state, create_thread);
    fail_mutex = !condition_case;
    fail_condition = condition_case;
    status = lua_pcall(state, 0, 1, 0);
  } else if (strcmp(name, "closed_capture") == 0) {
    fail_mutex = 1;
    status = luaL_loadstring(state, "return 42");
  } else if (strcmp(name, "open_capture") == 0) {
    if (luaL_loadstring(state, "local x=42; return function() return x end") !=
        LUA_OK)
      failed = 1;
    else {
      fail_mutex = 1;
      status = lua_pcall(state, 0, 1, 0);
    }
  } else {
    failed = 1;
  }
  fail_mutex = fail_condition = 0;
  if (state != NULL) {
    if (!main_case)
      failed |= status != LUA_ERRMEM;
    lua_settop(state, 0);
    (void)lua_gc(state, LUA_GCCOLLECT);
    /* A handled synchronization failure must leave the VM usable. */
    failed |= luaL_dostring(state, "local x=42; local f=function() return x "
                                   "end; return f()") != LUA_OK;
    failed |= lua_tointeger(state, -1) != 42;
    lua_close(state);
  }
  if (failures != 1 || live_mutexes != 0 || live_conditions != 0 ||
      live_bytes != 0) {
    fprintf(stderr, "%s: failures=%d live mutexes=%d conditions=%d bytes=%zu\n",
            name, failures, live_mutexes, live_conditions, live_bytes);
    failed = 1;
  }
  return failed;
}

struct run {
  const char *name;
  int failed;
};

static void *run_worker(void *context) {
  struct run *run = context;
  run->failed = run_case(run->name);
  return NULL;
}

int main(int argc, char **argv) {
  struct run run;
  pthread_t worker;
  if (argc != 2)
    return 2;
  run.name = argv[1];
  run.failed = 1;
  if (pthread_create(&worker, NULL, run_worker, &run) != 0)
    return 1;
  if (pthread_join(worker, NULL) != 0)
    return 1;
  return run.failed;
}
