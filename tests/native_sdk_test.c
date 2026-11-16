#include <lauxlib.h>
#include <lua.h>
#include <luthr/tasks.h>
#include <stdio.h>

static int context_value;

static int native_binding(lua_State *L) {
  if (luthr_lua_task_context(L) != &context_value)
    return luaL_error(L, "task context was not inherited by native binding");
  lua_pushinteger(L, luaL_checkinteger(L, 1) + 1);
  return 1;
}

int main(void) {
  lua_State *L = luaL_newstate();
  luthr_lua_scheduler *scheduler;
  luthr_lua_task *task;
  luthr_lua_task_result result;
  luthr_lua_scheduler_config config = {0};
  luthr_lua_task_options options = {0};
  const unsigned char source[] = "return native_binding(41)";
  if (L == NULL)
    return 1;
  config.parallelism = 1;
  config.native_limit = 1;
  config.carrier_limit = 2;
  options.context = &context_value;
  lua_pushcfunction(L, native_binding);
  lua_setglobal(L, "native_binding");
  if (luthr_lua_scheduler_new(L, &config, &scheduler) != LUTHR_LUA_RUNTIME_OK)
    return 2;
  if (luthr_lua_task_submit_buffer(scheduler, source, sizeof(source) - 1,
                                   "=native-sdk", &options,
                                   &task) != LUTHR_LUA_RUNTIME_OK)
    return 3;
  if (luthr_lua_task_wait(task, 5000, &result) != LUTHR_LUA_RUNTIME_OK ||
      result.status != LUTHR_LUA_RUNTIME_OK || result.result_count != 1)
    return 4;
  if (luthr_lua_task_collect(task, L, &result) != LUTHR_LUA_RUNTIME_OK ||
      lua_gettop(L) != 1 || lua_tointeger(L, -1) != 42)
    return 5;
  if (luthr_lua_task_free(task) != LUTHR_LUA_RUNTIME_OK ||
      luthr_lua_scheduler_free(scheduler) != LUTHR_LUA_RUNTIME_OK)
    return 6;
  lua_close(L);
  puts("native Lua headers and task SDK: passed");
  return 0;
}
