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

enum { APPEND_BLOCK_AT = 160, APPEND_RACE_ROUNDS = 64 };
static unsigned append_calls, append_stage;
static pthread_barrier_t append_race;

static void wait_for_append_stage(unsigned stage) {
  while (__atomic_load_n(&append_stage, __ATOMIC_ACQUIRE) != stage)
    sched_yield();
}

static void meet_append_race(void) {
  int status = pthread_barrier_wait(&append_race);
  require(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
}

static int blocking_field_result(lua_State *L) {
  ++append_calls;
  if (append_calls == APPEND_BLOCK_AT) {
    __atomic_store_n(&append_stage, 1, __ATOMIC_RELEASE);
    wait_for_append_stage(2);
    lua_pushinteger(L, 7);
    __atomic_store_n(&append_stage, 3, __ATOMIC_RELEASE);
    wait_for_append_stage(4);
  } else {
    if (append_calls > APPEND_BLOCK_AT &&
        append_calls <= APPEND_BLOCK_AT + APPEND_RACE_ROUNDS) {
      lua_Integer first, second;
      meet_append_race();
      if (append_calls & 1)
        lua_pushinteger(L, 7);
      else
        lua_pushnumber(L, 7.0);
      meet_append_race();
      require(lua_gettop(L) == 4);
      first = lua_tointeger(L, 3);
      second = lua_tointeger(L, 4);
      require((first == 7 && second == 9) || (first == 9 && second == 7));
    }
    lua_pushinteger(L, 3);
  }
  return 1;
}

static int repeat_native_appends(lua_State *L) {
  lua_Integer sum = 0;
  for (unsigned i = 0; i < 700; i++) {
    blocking_field_result(L);
    sum += lua_tointeger(L, -1);
    lua_settop(L, 2);
  }
  lua_pushinteger(L, sum);
  return 1;
}

static void check_native_append(lua_State *L, int standalone) {
  LuaSchedulerConfig config = {
      .slots = 1, .carriers = 2, .native_limit = 1, .callback_limit = 1};
  LuaScheduler scheduler = {0};
  LuaScheduledTask task = {0};
  lua_State *child;
  const LClosure *closure;
  int status, results;
  const char *program =
      standalone ? "return function(probe) local sum=probe(1,2); return sum end"
                 : "return function(row) "
                   "  local sum = 0; for i = 1, 700 do "
                   "    sum = sum + row.amount "
                   "  end; return sum "
                   "end";
  require(luaL_dostring(L, program) == LUA_OK);
  closure = lua_topointer(L, 1);
  child = lua_newthread(L);
  lua_pushvalue(L, 1);
  lua_xmove(L, child, 1);
  if (standalone)
    lua_pushcfunction(child, repeat_native_appends);
  else {
    lua_newuserdatauv(child, 1, 0);
    lua_newtable(child);
    lua_pushcfunction(child, blocking_field_result);
    lua_setfield(child, -2, "__index");
    require(lua_setmetatable(child, -2));
  }
  require(pthread_barrier_init(&append_race, NULL, 2) == 0);
  require(luaSC_init(L, &scheduler, &config) == 0);
  require(luaSC_submit(&scheduler, &task, child, 1) == 0);

  wait_for_append_stage(1);
  if (!standalone)
    require(closure->p->loops[0].entry > LUA_LOOP_UNAVAILABLE);
  require(lua_gettop(child) == 2 && lua_checkstack(child, 8192));
  lua_pushliteral(child, "foreign root");
  lua_gc(L, LUA_GCCOLLECT);
  require(strcmp(lua_tostring(child, -1), "foreign root") == 0);
  __atomic_store_n(&append_stage, 2, __ATOMIC_RELEASE);

  wait_for_append_stage(3);
  require(lua_gettop(child) == 4 && lua_tointeger(child, -1) == 7);
  lua_pushinteger(child, 53);
  lua_replace(child, -2);
  lua_gc(L, LUA_GCCOLLECT);
  __atomic_store_n(&append_stage, 4, __ATOMIC_RELEASE);
  for (unsigned round = 0; round < APPEND_RACE_ROUNDS; round++) {
    meet_append_race();
    lua_pushinteger(child, 9);
    if (round % 8 == 0)
      lua_gc(L, LUA_GCCOLLECT);
    meet_append_race();
  }

  luaSC_wait(&task);
  require(luaSC_collect(L, &task, &status, &results) == LUA_OK);
  require(status == LUA_OK && results == 1 && append_calls == 700);
  require(lua_tointeger(L, -1) == 699 * 3 + 53);
  require(luaSC_destroy(L, &scheduler) == 0);
  require(pthread_barrier_destroy(&append_race) == 0);
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

static unsigned result_calls;

static int resume_loop_result(lua_State *L, int status,
                              lua_KContext iteration) {
  require(status == LUA_YIELD);
  lua_pushinteger(L, (lua_Integer)iteration + 1);
  return 1;
}

static int produce_loop_results(lua_State *L) {
  int count = (int)luaL_checkinteger(L, 1);
  lua_Integer iteration = luaL_checkinteger(L, 2);
  result_calls++;
  if (iteration == 257)
    require(lua_checkstack(L, 256));
  if (iteration == 401) {
    if (count == -1) {
      lua_pushnil(L);
      lua_pushliteral(L, "native result rejected");
      return 2;
    }
    if (count == -2) {
      lua_pushinteger(L, iteration);
      return lua_yieldk(L, 1, (lua_KContext)iteration, resume_loop_result);
    }
  }
  if (count < 0)
    count = 1;
  require(lua_checkstack(L, count + 1));
  for (int i = 1; i <= count; i++) {
    if (i == count && count > LUA_MINSTACK) {
      lua_createtable(L, 0, 1);
      lua_pushinteger(L, iteration + i);
      lua_setfield(L, -2, "value");
      break;
    }
    lua_pushinteger(L, iteration + i);
  }
  return count;
}

static int consume_loop_results(lua_State *L) {
  int count = lua_gettop(L) - 1;
  lua_Integer iteration = luaL_checkinteger(L, 1);
  if (iteration == 353 && count > LUA_MINSTACK)
    lua_gc(L, LUA_GCCOLLECT);
  for (int i = 1; i <= count; i++) {
    if (i == count && count > LUA_MINSTACK) {
      require(lua_getfield(L, i + 1, "value") == LUA_TNUMBER);
      require(lua_tointeger(L, -1) == iteration + i);
      lua_pop(L, 1);
    } else {
      require(lua_tointeger(L, i + 1) == iteration + i);
    }
  }
  lua_pushinteger(L, count);
  lua_pushinteger(L, iteration + count);
  return 2;
}

static void check_native_results(lua_State *L) {
  static const int widths[] = {0, 1, 3, LUA_MINSTACK + 5};
  static const lua_Integer series = 600 * 601 / 2;
  const LClosure *loop;
  lua_State *child;
  int results;
  require(luaL_dostring(L, "return function(produce,consume,branch,count) "
                           "local total = 0 "
                           "for i=1,600 do "
                           "  if branch then "
                           "    local a,b = consume(i,produce(count,i)) "
                           "    total = total+a+(b or 0) "
                           "  else "
                           "    local first,second = produce(count,i) "
                           "    total = total+(second or i) "
                           "  end "
                           "end "
                           "return total end") == LUA_OK);
  loop = lua_topointer(L, 1);
  require(loop != NULL && loop->p->sizeloops == 1);
  for (unsigned w = 0; w < sizeof(widths) / sizeof(*widths); w++) {
    int width = widths[w];
    for (int consumer = 0; consumer < 4; consumer++) {
      lua_Integer expected = series + 1200 * width;
      result_calls = 0;
      lua_pushvalue(L, 1);
      lua_pushcfunction(L, produce_loop_results);
      if (consumer == 1) {
        require(luaL_dostring(L, "return function(i,...) "
                                 "local n = select('#',...) "
                                 "for j=1,n do "
                                 "  local v=select(j,...) "
                                 "  if type(v)=='table' then v=v.value end "
                                 "  assert(v == i+j) "
                                 "end "
                                 "return n,i+n end") == LUA_OK);
      } else if (consumer == 2) {
        lua_getglobal(L, "assert");
        expected = width == 0 ? series : 2 * series + 600;
      } else if (consumer == 3) {
        lua_pushnil(L);
        expected = series + (width >= 2 ? 1200 : 0);
      } else {
        lua_pushcfunction(L, consume_loop_results);
      }
      lua_pushboolean(L, consumer != 3);
      lua_pushinteger(L, width);
      require(lua_pcall(L, 4, 1, 0) == LUA_OK);
      require(result_calls == 600 && lua_tointeger(L, -1) == expected);
      lua_pop(L, 1);
    }
  }
  require(loop->p->loops[0].entry > LUA_LOOP_UNAVAILABLE);

  child = lua_newthread(L);
  lua_pushvalue(L, 1);
  lua_xmove(L, child, 1);
  lua_pushcfunction(child, produce_loop_results);
  lua_pushcfunction(child, consume_loop_results);
  lua_pushboolean(child, 1);
  lua_pushinteger(child, -2);
  result_calls = 0;
  require(lua_resume(child, L, 4, &results) == LUA_YIELD);
  require(results == 1 && lua_tointeger(child, -1) == 401);
  lua_pop(child, 1);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_resume(child, L, 0, &results) == LUA_OK);
  require(results == 1 && lua_tointeger(child, -1) == series + 1200);
  require(result_calls == 600);
  lua_pop(L, 1);

  require(luaL_dostring(L, "return function(produce) "
                           "local sum = 0 "
                           "for i=1,600 do sum=sum+assert(produce(-1,i)) end "
                           "return sum end") == LUA_OK);
  loop = lua_topointer(L, -1);
  require(loop != NULL && loop->p->sizeloops == 1);
  lua_pushvalue(L, -1);
  lua_pushcfunction(L, produce_loop_results);
  result_calls = 0;
  require(lua_pcall(L, 1, 1, 0) == LUA_ERRRUN);
  require(result_calls == 401 && lua_isstring(L, -1));
  require(strstr(lua_tostring(L, -1), "native result rejected") != NULL);
  require(loop->p->loops[0].entry > LUA_LOOP_UNAVAILABLE);
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

static void check_lua_calls(lua_State *L) {
  const LClosure *loop, *callee;
  require(
      luaL_dostring(
          L,
          "local count, sum = 0, 0; "
          "local function add(value) count=count+1; sum=sum+value end; "
          "local function repeat_add(value,n) "
          "  local step=add; for i=1,n do step(value) end end; "
          "repeat_add(3,700); assert(count==700 and sum==2100); "
          "local handled=0; "
          "local alternate=setmetatable({}, {__add=function(a,b) "
          "  handled=handled+1; return a+5 end}); "
          "repeat_add(alternate,10); "
          "assert(count==710 and sum==2150 and handled==10); "
          "local failure={}; "
          "local bad=setmetatable({}, {__add=function() error(failure) end}); "
          "local ok,result=pcall(repeat_add,bad,1); "
          "assert(not ok and result==failure and count==711 and sum==2150); "
          "collectgarbage('collect'); "
          "local hooks=0; debug.sethook(function() "
          "  if debug.getinfo(2,'f').func==add then hooks=hooks+1 end "
          "end,'c'); repeat_add(4,10); debug.sethook(); "
          "assert(count==721 and sum==2190 and hooks==10); "
          "return repeat_add,add") == LUA_OK);
  loop = lua_topointer(L, 1);
  callee = lua_topointer(L, 2);
  require(loop != NULL && loop->p->sizeloops == 1);
  require(loop->p->loops[0].entry > LUA_LOOP_UNAVAILABLE);
  require(callee != NULL && callee->p->nativecall != NULL);
  require(callee->p->nativecall->entry > LUA_LOOP_UNAVAILABLE);
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
  } else if (strcmp(argv[1], "native_append_incremental") == 0 ||
             strcmp(argv[1], "native_append_generational") == 0) {
    lua_gc(L, strcmp(argv[1], "native_append_incremental") == 0 ? LUA_GCINC
                                                                : LUA_GCGEN);
    check_native_append(L, 0);
  } else if (strcmp(argv[1], "native_owner_incremental") == 0 ||
             strcmp(argv[1], "native_owner_generational") == 0) {
    lua_gc(L, strcmp(argv[1], "native_owner_incremental") == 0 ? LUA_GCINC
                                                               : LUA_GCGEN);
    check_native_append(L, 1);
  } else if (strcmp(argv[1], "native_callbacks") == 0) {
    check_native_callbacks(L);
  } else if (strcmp(argv[1], "native_capture_race") == 0) {
    check_native_capture_race(L);
  } else if (strcmp(argv[1], "native_calls") == 0) {
    check_native_calls(L);
  } else if (strcmp(argv[1], "lua_calls_incremental") == 0 ||
             strcmp(argv[1], "lua_calls_generational") == 0) {
    lua_gc(L, strcmp(argv[1], "lua_calls_incremental") == 0 ? LUA_GCINC
                                                            : LUA_GCGEN);
    check_lua_calls(L);
  } else if (strcmp(argv[1], "native_results_incremental") == 0 ||
             strcmp(argv[1], "native_results_generational") == 0) {
    lua_gc(L, strcmp(argv[1], "native_results_incremental") == 0 ? LUA_GCINC
                                                                 : LUA_GCGEN);
    check_native_results(L);
  } else
    check_serial(L);
  lua_close(L);
  return 0;
}
