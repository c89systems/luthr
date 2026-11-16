#define _GNU_SOURCE
#include <lauxlib.h>
#include <luthr/tasks.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_barrier_t entered;
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static int counter, synchronized;

static void require(int condition) {
  if (!condition)
    abort();
}

static int touch(lua_State *L) {
  int status = pthread_barrier_wait(&entered);
  require(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
  if (synchronized)
    require(pthread_mutex_lock(&counter_mutex) == 0);
  /* Deliberately unsafe in the negative fixture. Both native activations
   * have entered before either writes; fiber annotations must not hide it. */
  counter++;
  if (synchronized)
    require(pthread_mutex_unlock(&counter_mutex) == 0);
  lua_pushinteger(L, 42);
  return 1;
}

int main(int argc, char **argv) {
  lua_State *L;
  luthr_lua_scheduler *scheduler;
  luthr_lua_scheduler_config config = {0};
  luthr_lua_task *tasks[2];
  const unsigned char source[] = "return touch()";
  require(argc == 2 &&
          (strcmp(argv[1], "safe") == 0 || strcmp(argv[1], "racy") == 0));
  synchronized = strcmp(argv[1], "safe") == 0;
  require(pthread_barrier_init(&entered, NULL, 2) == 0);
  L = luaL_newstate();
  require(L != NULL);
  lua_pushcfunction(L, touch);
  lua_setglobal(L, "touch");
  config.parallelism = 2;
  config.native_limit = 2;
  config.carrier_limit = 4;
  require(luthr_lua_scheduler_new(L, &config, &scheduler) ==
          LUTHR_LUA_RUNTIME_OK);
  for (unsigned i = 0; i < 2; ++i)
    require(luthr_lua_task_submit_buffer(scheduler, source, sizeof(source) - 1,
                                         "=race-detector", NULL,
                                         &tasks[i]) == LUTHR_LUA_RUNTIME_OK);
  for (unsigned i = 0; i < 2; ++i) {
    luthr_lua_task_result result;
    require(luthr_lua_task_wait(tasks[i], 5000, &result) ==
            LUTHR_LUA_RUNTIME_OK);
    require(result.status == LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_task_collect(tasks[i], L, &result) ==
            LUTHR_LUA_RUNTIME_OK);
    require(lua_tointeger(L, -1) == 42);
    lua_pop(L, 1);
    require(luthr_lua_task_free(tasks[i]) == LUTHR_LUA_RUNTIME_OK);
  }
  require(!synchronized || counter == 2);
  require(luthr_lua_scheduler_free(scheduler) == LUTHR_LUA_RUNTIME_OK);
  lua_close(L);
  require(pthread_barrier_destroy(&entered) == 0);
  require(pthread_mutex_destroy(&counter_mutex) == 0);
  return 0;
}
