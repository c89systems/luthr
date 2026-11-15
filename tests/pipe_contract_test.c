#include <errno.h>
#include <lauxlib.h>
#include <lexec.h>
#include <lpipe.h>
#include <lstate.h>
#include <lualib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
  const char *mode;
  unsigned allocations, fail_at, failures;
  size_t live;
  int deny, native_calls;
};
static struct fixture *active;
static int sync_failure;
#define require(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      abort();                                                                 \
    }                                                                          \
  } while (0)
int __real_pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int __real_pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
int __wrap_pthread_mutex_init(pthread_mutex_t *mutex,
                              const pthread_mutexattr_t *attributes) {
  if (sync_failure == 1) {
    sync_failure = 0;
    return ENOMEM;
  }
  return __real_pthread_mutex_init(mutex, attributes);
}
int __wrap_pthread_cond_init(pthread_cond_t *condition,
                             const pthread_condattr_t *attributes) {
  if (sync_failure == 2) {
    sync_failure = 0;
    return ENOMEM;
  }
  return __real_pthread_cond_init(condition, attributes);
}
static void *allocate(void *context, void *old, size_t old_size, size_t size) {
  struct fixture *test = context;
  void *pointer;
  if (old == NULL)
    old_size = 0;
  if (size == 0) {
    test->live -= old_size;
    free(old);
    return NULL;
  }
  test->allocations++;
  if (size > old_size && test->deny && test->allocations >= test->fail_at) {
    test->failures++;
    return NULL;
  }
  pointer = malloc(size);
  if (pointer != NULL) {
    if (old != NULL) {
      memcpy(pointer, old, old_size < size ? old_size : size);
      free(old);
    }
    test->live = test->live - old_size + size;
  }
  return pointer;
}
static int newpipe(lua_State *state) {
  luaP_newpipe(state, (unsigned)luaL_optinteger(state, 1, 0));
  return 1;
}
static int fail_allocations(lua_State *state) {
  active->deny = lua_toboolean(state, 1);
  active->fail_at = active->allocations + 1;
  return 0;
}
static int same_string(lua_State *state) {
  require(lua_tolstring(state, 1, NULL) == lua_tolstring(state, 2, NULL));
  return 0;
}
static void execute(lua_State *state, const char *source) {
  int status = luaL_loadstring(state, source);
  if (status == LUA_OK)
    status = lua_pcall(state, 0, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "pipe contract: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
}
static void native_enter(LuaExecution *execution, lua_State *state) {
  (void)execution;
  (void)state;
  active->native_calls++;
}
static int foreign_probe(lua_State *state) {
  require(luaQ_current()->native_depth == 1);
  lua_pushinteger(state, 42);
  return 1;
}
static int managed_probe(lua_State *state) {
  require(luaQ_current()->native_depth == 0);
  lua_pushvalue(state, lua_upvalueindex(1));
  return 1;
}
static int continued(lua_State *state, int status, lua_KContext context) {
  require(luaQ_current()->native_depth == 0 && status == LUA_YIELD &&
          context == 123);
  return managed_probe(state);
}
static int managed_yield(lua_State *state) {
  require(luaQ_current()->native_depth == 0);
  return lua_yieldk(state, 0, 123, continued);
}
static int managed_call(lua_State *state) {
  require(luaQ_current()->native_depth == 0);
  lua_getglobal(state, "yield_managed");
  lua_callk(state, 0, 1, 123, continued);
  return 1;
}
static int managed_error(lua_State *state) {
  require(luaQ_current()->native_depth == 0);
  return luaL_error(state, "managed expected");
}
static int recovered(lua_State *state, int status, lua_KContext context) {
  require(luaQ_current()->native_depth == 0 && status == LUA_ERRRUN &&
          context == 456);
  require(strstr(lua_tostring(state, -1), "managed expected") != NULL);
  lua_pushinteger(state, 77);
  return 1;
}
static int managed_protected(lua_State *state) {
  int status;
  require(luaQ_current()->native_depth == 0);
  lua_getglobal(state, "yield_then_error");
  status = lua_pcallk(state, 0, 0, 0, 456, recovered);
  return recovered(state, status, 456);
}
static void managed_contract(lua_State *state) {
  NativeStackContext context = *luaE_getnativestackcontext(), *previous;
  LuaExecution execution = {0};
  lua_State *child;
  int results, i, calls;
  lua_pushinteger(state, 77);
  luaQ_pushmanaged(state, managed_probe, 1);
  require(lua_iscfunction(state, -1) &&
          lua_tocfunction(state, -1) == managed_probe);
  lua_setglobal(state, "managed");
  lua_pushinteger(state, 77);
  luaQ_pushmanaged(state, managed_yield, 1);
  lua_setglobal(state, "yield_managed");
  lua_pushinteger(state, 77);
  luaQ_pushmanaged(state, managed_call, 1);
  lua_setglobal(state, "call_managed");
  luaQ_pushmanaged(state, managed_error, 0);
  lua_setglobal(state, "error_managed");
  luaQ_pushmanaged(state, managed_protected, 0);
  lua_setglobal(state, "protected_managed");
  execute(state,
          "function yield_then_error() yield_managed(); error_managed() end");
  lua_pushcfunction(state, foreign_probe);
  lua_setglobal(state, "foreign");
  lua_pushinteger(state, 1);
  lua_pushcclosure(state, foreign_probe, 1);
  lua_setglobal(state, "foreign_capture");
  require(
      luaL_loadstring(
          state,
          "local n=managed(); local m=foreign(); local k=foreign_capture(); "
          "if n~=77 or m~=42 or k~=42 then error('result') end; "
          "return managed()") == LUA_OK);
  execution.native_enter = native_enter;
  context.execution = &execution;
  previous = luaE_setnativestackcontext(&context);
  require(lua_pcall(state, 0, 0, 0) == LUA_OK);
  require(active->native_calls == 2);
  for (i = 0; i < 3; ++i) {
    child = lua_newthread(state);
    require(luaL_loadstring(child, i == 0   ? "return yield_managed()"
                                   : i == 1 ? "return call_managed()"
                                            : "return protected_managed()") ==
            LUA_OK);
    calls = active->native_calls; /* Chunk readers remain foreign callbacks. */
    require(lua_resume(child, NULL, 0, &results) == LUA_YIELD);
    require(execution.native_depth == 0 && execution.managed_depth == 0 &&
            execution.protected == NULL);
    require(lua_resume(child, NULL, 0, &results) == LUA_OK);
    require(results == 1 && lua_tointeger(child, -1) == 77);
    require(active->native_calls == calls);
    lua_pop(state, 1);
  }
  require(luaL_loadstring(state, "error_managed()") == LUA_OK);
  calls = active->native_calls;
  require(lua_pcall(state, 0, 0, 0) == LUA_ERRRUN);
  require(strstr(lua_tostring(state, -1), "managed expected") != NULL);
  lua_pop(state, 1);
  require(execution.native_depth == 0 && execution.managed_depth == 0 &&
          execution.protected == NULL && active->native_calls == calls);
  require(luaE_setnativestackcontext(previous) == &context);
}
static void *run(void *context) {
  struct fixture *test = context;
  lua_State *state = lua_newstate(allocate, test, 0);
  require(state != NULL);
  luaL_openlibs(state);
  luaQ_pushmanaged(state, newpipe, 0);
  lua_setglobal(state, "pipe");
  luaQ_pushmanaged(state, fail_allocations, 0);
  lua_setglobal(state, "deny");
  luaQ_pushmanaged(state, same_string, 0);
  lua_setglobal(state, "same");
  if (strcmp(test->mode, "managed") == 0)
    managed_contract(state);
  else if (strcmp(test->mode, "contract") == 0)
    execute(
        state,
        "for _,n in ipairs({0,1,64}) do local p=pipe(n); "
        "local s=string.rep('x',1024); assert(p:send(s)==1024); p:close(); "
        "same(p:recv(),s); assert(p:recv()==nil); "
        "assert(p:close()); local t=p:stats(); "
        "assert(t.capacity==(n==0 and 1 or n) and t.closed and t.ready); "
        "assert(t.sent_bytes==1024 and t.received_bytes==1024 and "
        "t.buffered_bytes==0 and t.peak_bytes==1024); "
        "assert(not pcall(p.send,p,'x')) end; "
        "local p=pipe(2); assert(p:send('')==0); p:send('a'); p:close(); "
        "assert(p:recv()=='' and p:recv()=='a' and p:recv()==nil); "
        "assert(not pcall(pipe,65)); assert(not pcall(pipe,-1)); "
        "assert(not pcall(p.send,p,string.rep('x',1025))); "
        "assert(not pcall(p.recv,{})); assert(not pcall(p.recv,io.stdout)); "
        "assert(getmetatable(p)==false); local mt=debug.getmetatable(p); "
        "mt.__gc(p); mt.__gc(p); assert(p:recv()==nil); "
        "assert(not p:stats().ready and not pcall(p.send,p,'a'))");
  else if (strcmp(test->mode, "transfer_oom") == 0)
    execute(
        state,
        "local p=pipe(); local s=string.rep('a',1024); "
        "p:send(s); assert(p:recv()==s); "
        "deny(true); p:send(s); local received=p:recv(); p:close(); "
        "local eof=p:recv(); deny(false); same(received,s); assert(eof==nil)");
  else if (strcmp(test->mode, "corrupt") == 0)
    execute(state,
            "local p=pipe(); p:send('abc'); debug.setuservalue(p,{}); "
            "local ok,err=pcall(p.recv,p); "
            "assert(not ok and string.find(err,'storage was modified')); "
            "assert(p:recv()==nil and p:stats().buffered_bytes==0); "
            "assert(not pcall(p.send,p,'x'))");
  else if (strncmp(test->mode, "roots_", 6) == 0) {
    unsigned round;
    luaP_newpipe(state, LUA_PIPE_SLOTS_MAX);
    lua_setglobal(state, "p");
    (void)lua_gc(state, LUA_GCCOLLECT);
    if (strstr(test->mode, "generational") != NULL)
      (void)lua_gc(state, LUA_GCGEN);
    for (round = 0; round < 3; ++round) {
      execute(state,
              "for i=1,64 do p:send(string.rep(string.char(i),1024)) end");
      (void)lua_gc(state, LUA_GCSTEP, 10000);
      (void)lua_gc(state, LUA_GCCOLLECT);
      execute(state, "for i=1,64 do "
                     "assert(p:recv()==string.rep(string.char(i),1024)) end");
    }
    execute(state, "p=nil");
    (void)lua_gc(state, LUA_GCCOLLECT);
    (void)lua_gc(state, LUA_GCCOLLECT);
    /* The private immutable metatable remains rooted even without any pipe. */
    execute(state, "local p=pipe(); p:send('after gc'); p:close(); "
                   "assert(p:recv()=='after gc' and p:recv()==nil)");
  } else if (strcmp(test->mode, "create_oom") == 0) {
    unsigned offset, succeeded = 0, failed = 0;
    /* Every allocation site in first-time construction, including metatable
       registration. Persistent failure also denies the emergency retry. */
    for (offset = 1; offset < 90; ++offset) {
      lua_State *probe = lua_newstate(allocate, test, 0);
      int status;
      require(probe != NULL);
      lua_pushcfunction(probe, newpipe);
      test->fail_at = test->allocations + offset;
      test->deny = 1;
      status = lua_pcall(probe, 0, 1, 0);
      test->deny = 0;
      require(status == LUA_OK || status == LUA_ERRMEM);
      succeeded += status == LUA_OK;
      failed += status == LUA_ERRMEM;
      lua_close(probe);
    }
    require(test->failures != 0 && succeeded != 0 && failed != 0);
  } else {
    execute(state, "local p=pipe(); p:close()"); /* Warm immutable metadata. */
    lua_pushcfunction(state, newpipe);
    sync_failure = strcmp(test->mode, "mutex") == 0 ? 1 : 2;
    require(lua_pcall(state, 0, 1, 0) == LUA_ERRMEM);
    require(sync_failure == 0);
    lua_pop(state, 1);
    execute(state, "local p=pipe(); p:send('ok'); p:close(); "
                   "assert(p:recv()=='ok' and p:recv()==nil)");
  }
  require(!test->deny);
  if (strcmp(test->mode, "transfer_oom") == 0)
    require(test->failures == 0);
  lua_close(state);
  require(test->live == 0);
  return NULL;
}
int main(int argc, char **argv) {
  struct fixture *test = calloc(1, sizeof(*test));
  pthread_t worker;
  require(test != NULL && argc == 2);
  test->mode = argv[1];
  active = test;
  require(pthread_create(&worker, NULL, run, test) == 0);
  require(pthread_join(worker, NULL) == 0);
  free(test);
  return 0;
}
