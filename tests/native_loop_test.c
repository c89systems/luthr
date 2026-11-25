#include "lauxlib.h"
#include "ljit.h"
#include "lscheduler.h"
#include "lualib.h"

#include <errno.h>
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
  } else
    check_serial(L);
  lua_close(L);
  return 0;
}
