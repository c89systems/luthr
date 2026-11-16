#include <lauxlib.h>
#include <lcoroscope.h>
#include <ldo.h>
#include <lualib.h>
#include <luthrum/lua_runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *main, *caller, *target;
  luthrum_lua_runtime *runtime;
  const char *mode;
  int armed, entered, release, status, results, finalized, deny;
  unsigned failures, allocations, fail_at;
  size_t live;
};
static struct fixture *active;
#define require(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      abort();                                                                 \
    }                                                                          \
  } while (0)
static void hold(struct fixture *test) {
  require(pthread_mutex_lock(&test->mutex) == 0);
  test->entered = 1;
  require(pthread_cond_broadcast(&test->changed) == 0);
  while (!test->release)
    require(pthread_cond_wait(&test->changed, &test->mutex) == 0);
  require(pthread_mutex_unlock(&test->mutex) == 0);
}
int __real_lua_checkstack(lua_State *, int);
int __wrap_lua_checkstack(lua_State *state, int count) {
  int result = __real_lua_checkstack(state, count);
  if (active->armed && state == active->target &&
      strstr(active->mode, "prepare") != NULL &&
      strstr(active->mode, "close") == NULL) {
    active->armed = 0;
    hold(active);
  }
  return result;
}
int __real_lua_resume(lua_State *, lua_State *, int, int *);
int __wrap_lua_resume(lua_State *state, lua_State *from, int count,
                      int *results) {
  int status = __real_lua_resume(state, from, count, results);
  if (active->armed && state == active->target &&
      strstr(active->mode, "result") != NULL &&
      strstr(active->mode, "close") == NULL) {
    active->armed = 0;
    hold(active);
  }
  return status;
}
int __real_lua_closethread(lua_State *, lua_State *);
int __wrap_lua_closethread(lua_State *state, lua_State *from) {
  int status;
  if (active->armed && state == active->target &&
      strstr(active->mode, "close_prepare") != NULL) {
    active->armed = 0;
    hold(active);
  }
  status = __real_lua_closethread(state, from);
  if (active->armed && state == active->target &&
      (strstr(active->mode, "close_result") != NULL ||
       strstr(active->mode, "close_reentrant") != NULL)) {
    active->armed = 0;
    hold(active);
  }
  return status;
}
void __real_luaW_restore(lua_State *, LuaWorldScope, unsigned);
void __wrap_luaW_restore(lua_State *state, LuaWorldScope scope,
                         unsigned depth) {
  __real_luaW_restore(state, scope, depth);
  if (active->armed && state == active->target && state->status == LUA_YIELD &&
      strstr(active->mode, "active_yield") != NULL) {
    LuaWorldScope suspended;
    active->armed = 0;
    require(state->resume_active != 0);
    suspended = luaW_detach(state);
    hold(active);
    luaW_attach(state, suspended);
  }
}
static void *allocate(void *context, void *old, size_t old_size, size_t size) {
  struct fixture *test = context;
  void *memory;
  if (old == NULL)
    old_size = 0;
  if (size == 0) {
    test->live -= old_size;
    free(old);
    return NULL;
  }
  test->allocations++;
  if (test->deny && size > old_size && test->allocations >= test->fail_at) {
    test->failures++;
    return NULL;
  }
  memory = malloc(size);
  if (memory != NULL) {
    if (old != NULL) {
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
static void host_free(void *context, void *pointer, size_t size) {
  (void)allocate(context, pointer, size, 0);
}
static int capture(void *context) {
  active->main = context;
  return 0;
}
static void execute(lua_State *state, const char *source) {
  int status = luaL_loadstring(state, source);
  if (status == LUA_OK)
    status = lua_pcall(state, 0, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "operation test: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
}
static int finalize(lua_State *state) {
  unsigned *token = lua_touserdata(state, 1);
  require(token != NULL && *token == 12345);
  *token = 0;
  active->finalized++;
  return 0;
}
static void push_token(lua_State *state) {
  unsigned *token = lua_newuserdatauv(state, sizeof(*token), 0);
  *token = 12345;
  lua_newtable(state);
  lua_pushcfunction(state, finalize);
  lua_setfield(state, -2, "__gc");
  lua_setmetatable(state, -2);
}
static int hold_close(lua_State *state) {
  (void)state;
  active->armed = 0;
  hold(active);
  return 0;
}
static int obtain_target(lua_State *state) {
  luaX_lockstate(state);
  luaD_checkstack(state, 1);
  setthvalue(state, s2v(state->top.p++), active->target);
  luaX_unlockstate(state);
  return 1;
}
static int reenter_owner(lua_State *state) {
  LuaCoroutineScope *scope = luaE_getnativestackcontext()->coroutine_scopes;
  lua_State *owner = active->caller;
  require(owner->owned_operations == 0);
  require(scope != NULL && scope->source != owner && scope->target == state &&
          scope->held);
  hold(active);
  lua_getglobal(owner, "request_close");
  require(lua_pcall(owner, 0, 0, 0) == LUA_ERRRUN);
  require(strstr(lua_tostring(owner, -1), "active operations") != NULL);
  lua_pop(owner, 1);
  require(luaE_getnativestackcontext()->coroutine_scopes == scope &&
          scope->held);
  lua_pushinteger(state, 42);
  return 1;
}
static int bridge_owner(lua_State *state) {
  lua_State *source;
  push_token(state);
  require(luaL_loadstring(
              state, "local token=...; return function() "
                     "local ok,n=coroutine.resume(coroutine.create(function() "
                     "local retained=token; return reenter_owner() end)); "
                     "assert(ok and n==42); return n end") == LUA_OK);
  lua_pushvalue(state, -2);
  lua_call(state, 1, 1);
  source = lua_newthread(state);
  lua_pushvalue(state, -2);
  lua_xmove(state, source, 1);
  lua_call(source, 0, 1);
  require(lua_tointeger(source, -1) == 42);
  lua_settop(source, 0);
  lua_settop(state, 0);
  lua_pushinteger(state, 99);
  return 1;
}
static int rooted_operation(lua_State *state) {
  LuaCoroutineScope scope;
  struct fixture *test = active;
  push_token(state);
  require(
      luaL_loadstring(
          state, "local token=...; return function() return token,42 end") ==
      LUA_OK);
  lua_pushvalue(state, -2);
  lua_call(state, 1, 1);
  test->target = lua_newthread(state);
  lua_pushvalue(state, -2);
  lua_xmove(state, test->target, 1);
  require(luaO_begin(state, test->target, &scope));
  lua_settop(state, 0); /* Only the operation root retains the child/token. */
  if (strstr(test->mode, "construction") != NULL) {
    unsigned offset, failed = 0, succeeded = 0;
    for (offset = 1; offset < 100; ++offset) {
      lua_State *inner;
      test->fail_at = test->allocations + offset;
      test->deny = 1;
      inner = lua_newstate(allocate, test, 0);
      test->deny = 0;
      if (inner == NULL)
        failed++;
      else {
        succeeded++;
        lua_close(inner);
      }
      require(test->target->operation == &scope);
    }
    require(failed != 0 && succeeded != 0 && test->failures != 0);
    test->fail_at = 0;
  }
  if (strstr(test->mode, "recover") != NULL)
    execute(state,
            "local handled=false; local ok,err=xpcall(function() "
            "error('nested') end, function(err) "
            "local co=obtain_target(); assert(coroutine.status(co)=='normal'); "
            "assert(not coroutine.resume(co)); handled=true; return err end); "
            "assert(not ok and handled and type(err)=='string')");
  hold(test);
  if (strstr(test->mode, "selfclose") != NULL) {
    test->deny = 1;
    require(lua_closethread(state, state) == LUA_ERRRUN);
    require(test->failures == 0);
    test->deny = 0;
    require(strstr(lua_tostring(state, -1), "active operations") != NULL);
    lua_pop(state, 1);
    require(test->target->operation == &scope);
  }
  if (strstr(test->mode, "oom") != NULL) {
    test->deny = 1;
    (void)lua_newuserdatauv(state, 4096, 0);
    abort();
  }
  if (strstr(test->mode, "error") != NULL)
    return luaL_error(state, "operation expected");
  luaO_end(&scope);
  lua_pushinteger(state, 99);
  return 1;
}
static void *invoke(void *context) {
  struct fixture *test = context;
  int raw = strstr(test->mode, "active_yield") != NULL;
  if (raw) {
    lua_pushinteger(test->target, 11);
    test->status = lua_resume(test->target, NULL, 1, &test->results);
  } else
    test->status = lua_resume(test->caller, NULL, 0, &test->results);
  return NULL;
}
struct snapshot {
  int count;
  TStatus status;
  TValue values[8];
};
static void snapshot(lua_State *state, struct snapshot *value, int compare) {
  int i, count;
  luaX_lockstate(state);
  count = (int)(state->top.p - (state->ci->func.p + 1));
  require(count >= 0 && count <= 8);
  if (compare)
    require(value->count == count && value->status == state->status);
  else {
    value->count = count;
    value->status = state->status;
  }
  for (i = 0; i < count; ++i) {
    const TValue *item = s2v(state->ci->func.p + 1 + i);
    if (!compare)
      value->values[i] = *item;
    else {
      require(rawtt(item) == rawtt(&value->values[i]));
      require(memcmp(&item->value_, &value->values[i].value_, sizeof(Value)) ==
              0);
    }
  }
  luaX_unlockstate(state);
}
static void *run(void *context) {
  struct fixture *test = context;
  struct snapshot before;
  pthread_t owner;
  lua_State *state;
  int root = strstr(test->mode, "root_") != NULL;
  int bridge = strstr(test->mode, "bridge_") != NULL;
  int close = strstr(test->mode, "close_") != NULL;
  int wrapped = strstr(test->mode, "wrap_") != NULL;
  int facade = strstr(test->mode, "facade") != NULL;
  if (facade) {
    static const unsigned char load[] = "require('fixture')";
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
    require(luthrum_lua_runtime_set_instruction_limit(test->runtime, 50000) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_run_buffer(
                test->runtime, load, sizeof(load) - 1, "coroutine-operation", 0,
                NULL, 0) == LUTHRUM_LUA_RUNTIME_OK);
  } else {
    test->main = lua_newstate(allocate, test, 0);
    require(test->main != NULL);
    luaL_openlibs(test->main);
  }
  state = test->main;
  if (strstr(test->mode, "generational") != NULL)
    (void)lua_gc(state, LUA_GCGEN);
  luaQ_pushmanaged(state, rooted_operation, 0);
  lua_setglobal(state, "rooted_operation");
  luaQ_pushmanaged(state, obtain_target, 0);
  lua_setglobal(state, "obtain_target");
  lua_pushcfunction(state, hold_close);
  lua_setglobal(state, "hold_close");
  lua_pushcfunction(state, bridge_owner);
  lua_setglobal(state, "bridge_owner");
  lua_pushcfunction(state, reenter_owner);
  lua_setglobal(state, "reenter_owner");
  if (bridge)
    execute(state, "function request_close() coroutine.close() end");
  if (!root && !bridge) {
    push_token(state);
    require(luaL_loadstring(
                state,
                "local token=...; close_calls=0; local function body(arg) "
                "local retained=token; "
                "local value <close> =setmetatable({}, {__close=function() "
                "close_calls=close_calls+1; if closing_reentrant then "
                "coroutine.close() end; "
                "if closing_error then error('close "
                "expected') end; "
                "if closing_wait then hold_close() end end}); "
                "local n=coroutine.yield('yielded',arg); return n+1 end; "
                "wrapped=coroutine.wrap(body); local "
                "_,co=debug.getupvalue(wrapped,1); shared=co") == LUA_OK);
    lua_pushvalue(state, -2);
    require(lua_pcall(state, 1, 0, 0) == LUA_OK);
    lua_settop(state, 0);
    lua_getglobal(state, "shared");
    test->target = lua_tothread(state, -1);
    lua_pop(state, 1);
    if (close) {
      execute(state, "assert(coroutine.resume(shared,11))");
      if (strstr(test->mode, "result") != NULL)
        execute(state, "closing_error=true");
      if (strstr(test->mode, "reentrant") != NULL)
        execute(state, "closing_reentrant=true");
      if (strstr(test->mode, "cleanup") != NULL)
        execute(state, "closing_wait=true");
    }
  }
  test->caller = lua_newthread(state);
  require(luaL_loadstring(test->caller,
                          bridge  ? "return bridge_owner()"
                          : !root ? close ? "return coroutine.close(shared)"
                                    : wrapped
                                        ? "return wrapped(11)"
                                        : "return coroutine.resume(shared,11)"
                          : strstr(test->mode, "handler_error") != NULL
                              ? "return xpcall(rooted_operation,function() "
                                "error('handler expected') end)"
                          : strstr(test->mode, "error") != NULL
                              ? "return xpcall(rooted_operation,function(err) "
                                "collectgarbage('collect'); "
                                "local co=obtain_target(); "
                                "assert(coroutine.status(co)=='suspended'); "
                                "local ok,token,n=coroutine.resume(co); "
                                "assert(ok and n==42); return n end)"
                              : "return rooted_operation()") == LUA_OK);
  test->armed = !root && !bridge;
  require(pthread_create(&owner, NULL, invoke, test) == 0);
  require(pthread_mutex_lock(&test->mutex) == 0);
  while (!test->entered)
    require(pthread_cond_wait(&test->changed, &test->mutex) == 0);
  require(pthread_mutex_unlock(&test->mutex) == 0);
  if (!root && !bridge) {
    snapshot(test->target, &before, 0);
    if (strstr(test->mode, "oom") != NULL) {
      require(luaL_loadstring(state, "return coroutine.resume(shared,77)") ==
              LUA_OK);
      test->deny = 1;
      require(lua_pcall(state, 0, LUA_MULTRET, 0) == LUA_ERRMEM);
      test->deny = 0;
      require(test->failures != 0);
      lua_pop(state, 1);
    }
    execute(state, "assert(coroutine.status(shared)=='normal'); "
                   "local ok,err=coroutine.resume(shared,77); assert(not ok "
                   "and type(err)=='string'); "
                   "ok,err=pcall(wrapped,77); assert(not ok); "
                   "ok,err=pcall(coroutine.close,shared); assert(not ok)");
  }
  (void)lua_gc(state, LUA_GCCOLLECT);
  (void)lua_gc(state, LUA_GCCOLLECT);
  if (root || bridge)
    require(test->finalized == 0);
  else
    snapshot(test->target, &before, 1);
  require(pthread_mutex_lock(&test->mutex) == 0);
  test->release = 1;
  require(pthread_cond_broadcast(&test->changed) == 0);
  require(pthread_mutex_unlock(&test->mutex) == 0);
  require(pthread_join(owner, NULL) == 0);
  test->deny = 0;
  if (root && strstr(test->mode, "oom") != NULL) {
    require(test->status == LUA_ERRMEM && test->failures != 0);
  } else {
    lua_State *result = strstr(test->mode, "active_yield") != NULL
                            ? test->target
                            : test->caller;
    require(test->status == (result == test->target ? LUA_YIELD : LUA_OK));
    if (bridge || (root && strstr(test->mode, "error") == NULL))
      require(test->results == 1 && lua_tointeger(result, -1) == 99);
    else if (root) {
      require(test->results == 2 && !lua_toboolean(result, -2));
      if (strstr(test->mode, "handler_error") == NULL)
        require(lua_tointeger(result, -1) == 42);
    } else if (close) {
      int failure = strstr(test->mode, "result") != NULL;
      require(lua_toboolean(result, 1) == !failure);
      if (strstr(test->mode, "result") != NULL)
        require(strstr(lua_tostring(result, -1), "close expected") != NULL);
      if (strstr(test->mode, "reentrant") != NULL)
        require(test->results == 1);
    } else
      require(lua_tointeger(result, -1) == 11);
  }
  require(G(state)->coroutine_scopes == NULL);
  require(test->caller->owned_operations == 0);
  if (!root && !bridge)
    require(test->target->operation == NULL &&
            test->target->owned_operations == 0 &&
            test->target->resume_active == 0 &&
            test->target->close_active == 0);
  if (!root && !bridge) {
    if (!close) {
      if (strstr(test->mode, "active_yield") != NULL)
        lua_pop(test->target, test->results);
      execute(state,
              "assert(coroutine.status(shared)=='suspended'); "
              "local ok,n=coroutine.resume(shared,88); assert(ok and n==89)");
    }
    execute(state, "assert(coroutine.status(shared)=='dead'); "
                   "assert(coroutine.close(shared)); assert(close_calls==1)");
  }
  lua_settop(test->caller, 0);
  lua_settop(state, 0);
  execute(state, "shared=nil; wrapped=nil; closing_wait=false");
  (void)lua_gc(state, LUA_GCCOLLECT);
  (void)lua_gc(state, LUA_GCCOLLECT);
  require(test->finalized == 1);
  if (facade)
    luthrum_lua_runtime_free(test->runtime);
  else
    lua_close(state);
  require(test->live == 0 && test->finalized == 1);
  return NULL;
}
int main(int argc, char **argv) {
  struct fixture *test = calloc(1, sizeof(*test));
  pthread_t worker;
  require(test != NULL && argc == 2);
  active = test;
  test->mode = argv[1];
  require(pthread_mutex_init(&test->mutex, NULL) == 0);
  require(pthread_cond_init(&test->changed, NULL) == 0);
  require(pthread_create(&worker, NULL, run, test) == 0);
  require(pthread_join(worker, NULL) == 0);
  require(pthread_cond_destroy(&test->changed) == 0);
  require(pthread_mutex_destroy(&test->mutex) == 0);
  free(test);
  return 0;
}
