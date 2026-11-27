#include "lauxlib.h"
#include "ljit.h"
#include "lscheduler.h"
#include "lualib.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define require(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      abort();                                                                 \
    }                                                                          \
  } while (0)

static unsigned executable_requests;
static int deny_execute;

int __real_mprotect(void *, size_t, int);
int __wrap_mprotect(void *memory, size_t bytes, int protection) {
  if (protection & PROT_EXEC) {
    require(!(protection & PROT_WRITE));
    __atomic_fetch_add(&executable_requests, 1, __ATOMIC_RELAXED);
    if (deny_execute) {
      errno = EPERM;
      return -1;
    }
  }
  return __real_mprotect(memory, bytes, protection);
}

static const char program[] =
    "return function(row, n, step)\n"
    "  local sum = 0\n"
    "  for i = -n, n, step do\n"
    "    row.balance = row.balance + i % 9\n"
    "    row.timestamp = (i * 10007) // 31\n"
    "    row.last = {value=i, doubled=i*2, tripled=i*3}\n"
    "    sum = sum + row.balance + row.timestamp + row.last.value\n"
    "    sum = sum + row.last.doubled + row.last.tripled\n"
    "  end\n"
    "  return sum\n"
    "end";

static const Proto *load_loop(lua_State *L) {
  const LClosure *closure;
  require(luaL_dostring(L, program) == LUA_OK);
  closure = lua_topointer(L, -1);
  require(closure != NULL && closure->p->sizeloops == 1);
  return closure->p;
}

static void row(lua_State *L, lua_Integer balance) {
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, balance);
  lua_setfield(L, -2, "balance");
}

static lua_Integer expected(lua_Integer n) {
  lua_Integer balance = 0, sum = 0;
  for (lua_Integer i = -n; i <= n; i++) {
    lua_Integer mod = i % 9, stamp = i * 10007;
    balance += mod < 0 ? mod + 9 : mod;
    sum += balance + stamp / 31 - (stamp % 31 < 0) + 6 * i;
  }
  return sum;
}

static void run(lua_State *L, lua_Integer n) {
  lua_pushvalue(L, 1);
  row(L, 0);
  lua_pushinteger(L, n);
  lua_pushinteger(L, 1);
  require(lua_pcall(L, 3, 1, 0) == LUA_OK);
  require(lua_isinteger(L, -1) && lua_tointeger(L, -1) == expected(n));
  lua_pop(L, 1);
}

static void check_serial(lua_State *L) {
  const Proto *proto = load_loop(L);
  run(L, 700);
  if (deny_execute) {
    require(proto->loops[0].entry == LUA_LOOP_UNAVAILABLE);
    require(G(L)->native_loop_bytes == 0);
  } else {
    require(proto->loops[0].entry > LUA_LOOP_UNAVAILABLE);
    require(G(L)->native_loop_bytes == proto->loops[0].allocsize);
    require(luaL_dostring(
                L, "return function(walk) "
                   "  local calls = 0 "
                   "  local r = {balance=setmetatable({}, {__add=function(_,v) "
                   "    calls=calls+1; return v end})} "
                   "  local ticks = 0 "
                   "  debug.sethook(function() ticks=ticks+1 end, '', 27) "
                   "  local result = walk({balance=0}, 300, 0.5) "
                   "  debug.sethook() "
                   "  assert(result == result and ticks > 10) "
                   "  return walk(r, 700, 1), calls "
                   "end") == LUA_OK);
    lua_pushvalue(L, 1);
    require(lua_pcall(L, 1, 2, 0) == LUA_OK);
    require(lua_tointeger(L, -1) == 1);
    require(lua_tointeger(L, -2) == expected(700));
    lua_pop(L, 2);
  }
  require(executable_requests == 1);
  lua_settop(L, 0);
  lua_gc(L, LUA_GCCOLLECT);
  require(G(L)->native_loop_bytes == 0);
}

static void check_parallel(lua_State *L) {
  enum { WIDTH = 4, WORK = 24000 };
  LuaSchedulerConfig config = {
      .slots = WIDTH,
      .carriers = 2 * WIDTH,
      .native_limit = WIDTH,
      .callback_limit = WIDTH,
  };
  LuaScheduler scheduler = {0};
  LuaScheduledTask tasks[WIDTH] = {0};
  const Proto *proto = load_loop(L);
  require(luaSC_init(L, &scheduler, &config) == 0);
  for (unsigned i = 0; i < WIDTH; i++) {
    lua_State *child = lua_newthread(L);
    lua_pushvalue(L, 1);
    lua_xmove(L, child, 1);
    row(child, 0);
    lua_pushinteger(child, WORK);
    lua_pushinteger(child, 1);
    require(luaSC_submit(&scheduler, &tasks[i], child, 3) == 0);
    lua_pop(L, 1);
  }
  lua_gc(L, LUA_GCCOLLECT);
  for (unsigned i = 0; i < WIDTH; i++) {
    int status, results;
    luaSC_wait(&tasks[i]);
    require(luaSC_collect(L, &tasks[i], &status, &results) == LUA_OK);
    require(status == LUA_OK && results == 1);
    require(lua_tointeger(L, -1) == expected(WORK));
    lua_pop(L, 1);
  }
  require(executable_requests == 1);
  require(proto->loops[0].entry > LUA_LOOP_UNAVAILABLE);
  require(luaSC_destroy(L, &scheduler) == 0);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT);
  require(G(L)->native_loop_bytes == 0);
}

static void check_cancel(lua_State *L) {
  LuaSchedulerConfig config = {
      .slots = 1,
      .carriers = 2,
      .native_limit = 1,
      .callback_limit = 1,
  };
  LuaScheduler scheduler = {0};
  LuaScheduledTask task = {0};
  const Proto *proto = load_loop(L);
  lua_State *child;
  int status, results, started;
  run(L, 700);
  require(proto->loops[0].entry > LUA_LOOP_UNAVAILABLE);
  require(luaSC_init(L, &scheduler, &config) == 0);
  row(L, 0);
  child = lua_newthread(L);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_xmove(L, child, 2);
  lua_pushinteger(child, 100000000);
  lua_pushinteger(child, 1);
  require(luaSC_submit(&scheduler, &task, child, 3) == 0);
  lua_pop(L, 1);
  do {
    started = lua_getfield(L, 2, "timestamp") == LUA_TNUMBER;
    lua_pop(L, 1);
    if (!started)
      sched_yield();
  } while (!started);
  require(luaTK_requeststop(&task.root) == LUA_OK);
  luaSC_wait(&task);
  require(luaSC_collect(L, &task, &status, &results) == LUA_OK);
  require(status == LUA_ERRRUN && results == 1 && task.root.stop_delivered);
  require(luaSC_destroy(L, &scheduler) == 0);
  lua_settop(L, 0);
  lua_gc(L, LUA_GCCOLLECT);
  require(G(L)->native_loop_bytes == 0);
}

static unsigned field_calls, field_ticks;

static void count_field_instructions(lua_State *L, lua_Debug *debug) {
  (void)L;
  (void)debug;
  field_ticks++;
}

static int resume_field(lua_State *L, int status, lua_KContext value) {
  require(status == LUA_YIELD);
  lua_pushinteger(L, (lua_Integer)value);
  return 1;
}

static int userdata_field(lua_State *L) {
  const int *value = lua_touserdata(L, 1);
  require(value != NULL && strcmp(lua_tostring(L, 2), "amount") == 0);
  field_calls++;
  if (field_calls == 200) {
    lua_Debug caller;
    require(lua_getstack(L, 1, &caller));
    require(lua_getinfo(L, "l", &caller) && caller.currentline == 4);
    require(lua_checkstack(L, 2048));
  }
  if (field_calls == 317) {
    lua_pushinteger(L, field_calls);
    return lua_yieldk(L, 1, (lua_KContext)*value, resume_field);
  }
  if (field_calls == 419)
    lua_sethook(L, count_field_instructions, LUA_MASKCOUNT, 1);
  lua_pushinteger(L, *value);
  return 1;
}

static int replacement_field(lua_State *L) {
  lua_pushinteger(L, 3);
  lua_pushinteger(L, 99);
  return 2;
}

static int replace_userdata_field(lua_State *L) {
  require(lua_getmetatable(L, 1));
  lua_pushcfunction(L, replacement_field);
  lua_setfield(L, -2, "__index");
  lua_pushinteger(L, 2);
  return 1;
}

static int replace_userdata_metatable(lua_State *L) {
  lua_createtable(L, 0, 32);
  lua_pushcfunction(L, replacement_field);
  lua_setfield(L, -2, "__index");
  lua_pushboolean(L, 1);
  lua_setfield(L, -2, "unrelated");
  require(lua_setmetatable(L, 1));
  lua_pushinteger(L, 2);
  return 1;
}

static int userdata_reference(lua_State *L) {
  require(lua_getiuservalue(L, 1, 1) == LUA_TTABLE);
  field_calls++;
  return 1;
}

static void check_native_fields(lua_State *L) {
  const LClosure *closure;
  lua_State *child;
  int *value, results;
  require(luaL_dostring(L, "return function(row, n)\n"
                           "  local sum = 0\n"
                           "  for i = 1, n do\n"
                           "    sum = sum + row.amount\n"
                           "  end\n"
                           "  return sum\n"
                           "end") == LUA_OK);
  closure = lua_topointer(L, 1);
  require(closure != NULL && closure->p->sizeloops == 1);
  lua_pushvalue(L, 1);
  lua_newtable(L);
  lua_pushinteger(L, 2);
  lua_setfield(L, -2, "amount");
  lua_pushinteger(L, 700);
  require(lua_pcall(L, 2, 1, 0) == LUA_OK);
  require(lua_tointeger(L, -1) == 1400);
  lua_pop(L, 1);
  require(closure->p->loops[0].entry > LUA_LOOP_UNAVAILABLE);

  child = lua_newthread(L);
  lua_pushvalue(L, 1);
  lua_xmove(L, child, 1);
  value = lua_newuserdatauv(child, sizeof(*value), 0);
  *value = 3;
  lua_newtable(child);
  lua_pushcfunction(child, userdata_field);
  lua_setfield(child, -2, "__index");
  require(lua_setmetatable(child, -2));
  lua_pushinteger(child, 700);
  require(lua_resume(child, L, 2, &results) == LUA_YIELD);
  require(results == 1 && lua_tointeger(child, -1) == 317);
  lua_pop(child, 1);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_resume(child, L, 0, &results) == LUA_OK);
  require(results == 1 && lua_tointeger(child, -1) == 2100);
  require(field_calls == 700 && field_ticks > 200);

  lua_pushvalue(L, 1);
  lua_newuserdatauv(L, 1, 0);
  lua_newtable(L);
  lua_pushcfunction(L, replace_userdata_field);
  lua_setfield(L, -2, "__index");
  lua_newtable(L);
  lua_pushliteral(L, "kv");
  lua_setfield(L, -2, "__mode");
  require(lua_setmetatable(L, -2));
  require(lua_setmetatable(L, -2));
  lua_gc(L, LUA_GCCOLLECT);
  lua_pushinteger(L, 100);
  require(lua_pcall(L, 2, 1, 0) == LUA_OK);
  require(lua_tointeger(L, -1) == 299);

  lua_pushvalue(L, 1);
  lua_newuserdatauv(L, 1, 0);
  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, replace_userdata_metatable);
  lua_setfield(L, -2, "__index");
  require(lua_setmetatable(L, -2));
  lua_pushinteger(L, 100);
  require(lua_pcall(L, 2, 1, 0) == LUA_OK);
  require(lua_tointeger(L, -1) == 299);

  /* A table read after a native result must resume at the table read,
     retaining the returned value and the already executed C call. */
  field_calls = 0;
  require(luaL_dostring(L, "return function(row) "
                           "local sum = 0; for i = 1, 700 do "
                           "sum = sum + row.reference.amount end; return sum "
                           "end") == LUA_OK);
  lua_newuserdatauv(L, 1, 1);
  lua_createtable(L, 0, 1);
  lua_pushinteger(L, 3);
  lua_setfield(L, -2, "amount");
  require(lua_setiuservalue(L, -2, 1));
  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, userdata_reference);
  lua_setfield(L, -2, "__index");
  require(lua_setmetatable(L, -2));
  require(lua_pcall(L, 1, 1, 0) == LUA_OK);
  require(lua_tointeger(L, -1) == 2100 && field_calls == 700);
}

static int call_many(lua_State *L) {
  lua_Integer n = luaL_checkinteger(L, 3);
  for (lua_Integer i = 0; i < n; i++) {
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_call(L, 1, 0);
  }
  return 0;
}

static int finish_loop_call(lua_State *L, int status, lua_KContext value) {
  require(status == LUA_OK || status == LUA_YIELD);
  lua_pushvalue(L, 1);
  lua_pushinteger(L, (lua_Integer)value);
  lua_call(L, 1, 0);
  return 0;
}

static int loop_call(lua_State *L) {
  lua_Integer value = luaL_checkinteger(L, 2);
  require(lua_toboolean(L, 3));
  if (value == 317 && lua_isyieldable(L)) {
    lua_pushinteger(L, value);
    return lua_yieldk(L, 1, (lua_KContext)value, finish_loop_call);
  }
  return finish_loop_call(L, LUA_OK, (lua_KContext)value);
}

static unsigned width_calls;

static int loop_argument_width(lua_State *L) {
  int width = (int)(width_calls++ % 3);
  require(lua_gettop(L) == (width == 0 ? 9 : width));
  if (width == 0) {
    require(lua_tointeger(L, 8) == 8 && lua_tointeger(L, 9) == 9);
    lua_settop(L, 1);
  } else if (width == 2)
    require(lua_tointeger(L, 2) == 2);
  require(lua_type(L, 8) == LUA_TNONE);
  return 0;
}

static void check_native_calls(lua_State *L) {
  static const lua_Integer expected_sum = 700 * 701 / 2;
  const LClosure *closure;
  lua_State *child;
  int results;
  require(luaL_dostring(L, "return function(call) "
                           "local sum = 0; "
                           "local function add(n) sum = sum + n end; "
                           "for i=1,700 do call(add, i, true) end; "
                           "return sum end") == LUA_OK);
  closure = lua_topointer(L, 1);
  require(closure != NULL && closure->p->sizeloops == 1);
  lua_pushvalue(L, 1);
  lua_pushcfunction(L, loop_call);
  require(lua_pcall(L, 1, 1, 0) == LUA_OK);
  require(lua_tointeger(L, -1) == expected_sum);
  lua_pop(L, 1);
  require(closure->p->loops[0].entry > LUA_LOOP_UNAVAILABLE);

  lua_pushvalue(L, 1);
  require(luaL_dostring(
              L, "return function(add,i,ok) assert(ok); add(i) end") == LUA_OK);
  require(lua_pcall(L, 1, 1, 0) == LUA_OK);
  require(lua_tointeger(L, -1) == expected_sum);
  lua_pop(L, 1);

  child = lua_newthread(L);
  lua_pushvalue(L, 1);
  lua_xmove(L, child, 1);
  lua_pushcfunction(child, loop_call);
  require(lua_resume(child, L, 1, &results) == LUA_YIELD);
  require(results == 1 && lua_tointeger(child, -1) == 317);
  lua_pop(child, 1);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_resume(child, L, 0, &results) == LUA_OK);
  require(results == 1 && lua_tointeger(child, -1) == expected_sum);

  require(luaL_dostring(L, "return function(check) "
                           "for i=1,700 do "
                           "  check(i,2,3,4,5,6,7,8,9); check(i); check(i,2) "
                           "end end") == LUA_OK);
  closure = lua_topointer(L, -1);
  require(closure != NULL && closure->p->sizeloops == 1);
  lua_pushvalue(L, -1);
  lua_pushcfunction(L, loop_argument_width);
  require(lua_pcall(L, 1, 0, 0) == LUA_OK);
  require(width_calls == 2100 &&
          closure->p->loops[0].entry > LUA_LOOP_UNAVAILABLE);
}

static void check_native_callbacks(lua_State *L) {
  const LClosure *closure;
  lua_pushcfunction(L, call_many);
  lua_setglobal(L, "call_many");
  require(luaL_dostring(
              L,
              "local total,calls,sample=0,0,nil; "
              "function record(chunk) "
              "  total=total+#chunk; calls=calls+1; "
              "  if sample==nil then sample=chunk end "
              "end; "
              "call_many(record,'abc',400); "
              "assert(total==1200 and calls==400 and sample=='abc'); "
              "sample=nil; collectgarbage('collect'); "
              "call_many(record,'xyz',10); "
              "assert(total==1230 and calls==410 and sample=='xyz'); "
              "local next=setmetatable({}, {__len=function() return 7 end}); "
              "sample=nil; call_many(record,next,10); "
              "assert(total==1300 and calls==420 and rawequal(sample,next))") ==
          LUA_OK);
  require(lua_getglobal(L, "record") == LUA_TFUNCTION);
  closure = lua_topointer(L, -1);
  require(closure != NULL && closure->p->nativecall != NULL);
  require(closure->p->nativecall->entry > LUA_LOOP_UNAVAILABLE);
  require(luaL_dostring(L,
                        "collectgarbage('stop'); "
                        "local value, calls = 0, 0; "
                        "function overwrite(nextvalue) "
                        "  calls=calls+1; value=nextvalue "
                        "end; "
                        "call_many(overwrite, 31, 400); "
                        "local weak=setmetatable({}, {__mode='v'}); "
                        "do local old={}; value=old; weak[1]=old end; "
                        "call_many(overwrite, 41, 1); "
                        "collectgarbage('collect'); "
                        "assert(value==41 and calls==401 and weak[1]==nil)") ==
          LUA_OK);
}

static pthread_barrier_t capture_start;

static int start_capture_race(lua_State *L) {
  (void)L;
  int status = pthread_barrier_wait(&capture_start);
  require(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
  return 0;
}

static void check_native_capture_race(lua_State *L) {
  LuaSchedulerConfig config = {
      .slots = 2, .carriers = 4, .native_limit = 2, .callback_limit = 2};
  LuaScheduler scheduler = {0};
  LuaScheduledTask tasks[2] = {0};
  const LClosure *overwrite;
  require(pthread_barrier_init(&capture_start, NULL, 3) == 0);
  lua_pushcfunction(L, start_capture_race);
  lua_setglobal(L, "start_capture_race");
  lua_pushcfunction(L, call_many);
  lua_setglobal(L, "call_many");
  require(luaL_dostring(
              L, "local value,calls=41,0; "
                 "local function overwrite(n) calls=calls+1; value=n end; "
                 "call_many(overwrite,41,400); "
                 "return function(native) "
                 "  start_capture_race(); "
                 "  if native then "
                 "    call_many(overwrite,41,4000); assert(calls==4400) "
                 "  else "
                 "    local marker={}; "
                 "    for i=1,4000 do "
                 "      value=marker; local seen=value; "
                 "      assert(rawequal(seen,marker) or seen==41) "
                 "    end "
                 "  end; return 51 "
                 "end, overwrite") == LUA_OK);
  overwrite = lua_topointer(L, 2);
  require(overwrite != NULL && overwrite->p->nativecall != NULL);
  require(overwrite->p->nativecall->entry > LUA_LOOP_UNAVAILABLE);
  require(luaSC_init(L, &scheduler, &config) == 0);
  for (unsigned i = 0; i < 2; i++) {
    lua_State *child = lua_newthread(L);
    lua_pushvalue(L, 1);
    lua_xmove(L, child, 1);
    lua_pushboolean(child, i == 0);
    require(luaSC_submit(&scheduler, &tasks[i], child, 1) == 0);
    lua_pop(L, 1);
  }
  start_capture_race(L);
  lua_gc(L, LUA_GCCOLLECT);
  for (unsigned i = 0; i < 2; i++) {
    int status, results;
    luaSC_wait(&tasks[i]);
    require(luaSC_collect(L, &tasks[i], &status, &results) == LUA_OK);
    require(status == LUA_OK && results == 1 && lua_tointeger(L, -1) == 51);
    lua_pop(L, 1);
  }
  require(luaSC_destroy(L, &scheduler) == 0);
  require(pthread_barrier_destroy(&capture_start) == 0);
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  require(argc == 2 && L != NULL);
  luaL_openlibs(L);
  deny_execute = strcmp(argv[1], "denied") == 0;
  if (strcmp(argv[1], "incremental") == 0) {
    lua_gc(L, LUA_GCINC);
    check_parallel(L);
  } else if (strcmp(argv[1], "generational") == 0) {
    lua_gc(L, LUA_GCGEN);
    check_parallel(L);
  } else if (strcmp(argv[1], "cancel") == 0) {
    check_cancel(L);
  } else if (strcmp(argv[1], "native_fields") == 0) {
    check_native_fields(L);
  } else if (strcmp(argv[1], "native_callbacks") == 0) {
    check_native_callbacks(L);
  } else if (strcmp(argv[1], "native_capture_race") == 0) {
    check_native_capture_race(L);
  } else if (strcmp(argv[1], "native_calls") == 0) {
    check_native_calls(L);
  } else
    check_serial(L);
  lua_close(L);
  return 0;
}
