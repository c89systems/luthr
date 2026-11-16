#include <luthr/lua.h>
#include <luthr/lua_runtime.h>
#include <luthr/tasks.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  luthr_lua_state *state;
  luthr_lua_integer value;
  luthr_lua_runtime *runtime;
  luthr_lua_scheduler *scheduler;
  luthr_lua_task *task;
  luthr_lua_task_result result;
  luthr_lua_scheduler_config config;
  const unsigned char source[] = "assert(6*7==42)";
  state = luthr_lua_l_newstate();
  if (state == NULL)
    return 1;
  luthr_lua_pushinteger(state,
                        luthr_lua_integer_make(0x12345678U, 0x9abcdef0U));
  value = luthr_lua_tointeger(state, -1);
  if (luthr_lua_integer_high(value) != 0x12345678U ||
      luthr_lua_integer_low(value) != 0x9abcdef0U)
    return 2;
  luthr_lua_close(state);
  if (argc == 2) {
    state = luthr_lua_l_newstate();
    if (state == NULL)
      return 5;
    luthr_lua_l_openlibs(state);
    luthr_lua_pushstring(state, argv[1]);
    luthr_lua_setglobal(state, "module_dir");
    if (luthr_lua_l_dostring(
            state, "package.cpath=module_dir..'/?.so'; "
                   "local m=require('native_fixture'); "
                   "assert(m.callback(function(n) return n+1 end)==43)") !=
        LUTHR_LUA_OK) {
      fprintf(stderr, "%s\n", luthr_lua_tostring(state, -1));
      return 6;
    }
    luthr_lua_close(state);
  }
  if (luthr_lua_runtime_new(&runtime) != LUTHR_LUA_RUNTIME_OK)
    return 3;
  if (luthr_lua_runtime_run_buffer(
          runtime, source, sizeof(source) - 1, "installed-consumer", 0, NULL,
          LUTHR_LUA_RUNTIME_OPEN_LIBS) != LUTHR_LUA_RUNTIME_OK) {
    fprintf(stderr, "%s\n", luthr_lua_runtime_error(runtime));
    return 4;
  }
  luthr_lua_runtime_free(runtime);
  memset(&config, 0, sizeof(config));
  config.parallelism = 1;
  config.native_limit = 1;
  config.carrier_limit = 2;
  state = luthr_lua_l_newstate();
  if (state == NULL)
    return 7;
  luthr_lua_l_openlibs(state);
  if (luthr_lua_scheduler_new(state, &config, &scheduler) !=
      LUTHR_LUA_RUNTIME_OK)
    return 7;
  if (luthr_lua_task_submit_buffer(scheduler, source, sizeof(source) - 1,
                                   "=installed-task", NULL,
                                   &task) != LUTHR_LUA_RUNTIME_OK)
    return 8;
  if (luthr_lua_task_wait(task, 5000, &result) != LUTHR_LUA_RUNTIME_OK ||
      result.status != LUTHR_LUA_RUNTIME_OK)
    return 9;
  if (luthr_lua_task_collect(task, NULL, &result) != LUTHR_LUA_RUNTIME_OK ||
      luthr_lua_task_free(task) != LUTHR_LUA_RUNTIME_OK ||
      luthr_lua_scheduler_free(scheduler) != LUTHR_LUA_RUNTIME_OK)
    return 10;
  luthr_lua_close(state);
  puts("installed C89 consumer: passed");
  return 0;
}
