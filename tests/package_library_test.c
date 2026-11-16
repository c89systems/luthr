#include <lapi.h>
#include <lauxlib.h>
#include <lstate.h>
#include <lualib.h>
#include <luthr/lua_runtime.h>
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
  lua_State *state;
  luthr_lua_runtime *runtime;
  const char *mode;
  size_t live;
  unsigned remaining, failures, admissions, released;
  int deny;
  pthread_barrier_t rendezvous;
};
struct actor {
  lua_State *state;
  int status, results;
};
static struct fixture *active;
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
  if (test->deny && size > old_size) {
    if (test->remaining == 0) {
      test->failures++;
      return NULL;
    }
    test->remaining--;
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
static void host_free(void *context, void *p, size_t size) {
  (void)allocate(context, p, size, 0);
}
static int capture(void *state) {
  active->state = state;
  return 0;
}
static void execute(lua_State *state, const char *source) {
  int status = luaL_loadstring(state, source);
  if (status == LUA_OK)
    status = lua_pcall(state, 0, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "package library: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
}
static void *release_name(void *context, void *bytes, size_t old_size,
                          size_t size) {
  struct fixture *test = context;
  require(old_size == 8195 && size == 0);
  test->released++;
  free(bytes);
  return NULL;
}
static int name_alive(lua_State *state) {
  (void)state;
  require(active->released == 0);
  return 0;
}
static void snapshot(struct fixture *test) {
  char *name = malloc(8195);
  require(name != NULL);
  memset(name, 'N', 8194);
  name[8194] = 0;
  lua_pushexternalstring(test->state, name, 8194, release_name, test);
  lua_setglobal(test->state, "original_name");
  lua_pushcfunction(test->state, name_alive);
  lua_setglobal(test->state, "name_alive");
  execute(
      test->state,
      "local req=require; local _,original=debug.getupvalue(req,1); "
      "local registry=debug.getregistry(); local loaded=registry._LOADED; "
      "local weak=setmetatable({}, {__mode='v'}); local finalized=0; "
      "local function object(t) return setmetatable(t or {}, "
      "{__gc=function() finalized=finalized+1 end}) end; "
      "do local owner=object(); local data=object({answer=42}); "
      "local function loader(n,d) assert(n==original_name and d.answer==42); "
      "assert(owner~=nil); n=nil; d=nil; original_name=nil; "
      "debug.setupvalue(req,1,{searchers={}}); registry._LOADED={}; "
      "assert(debug.setlocal(2,1,nil)); "
      "collectgarbage('collect'); collectgarbage('collect'); name_alive(); "
      "assert(finalized==0 and weak.pkg and weak.loaded and weak.searchers "
      "and weak.data and weak.owner); return 42 end; "
      "local searchers=object({function(n) assert(n==original_name); "
      "return loader,data end}); local pkg=object({searchers=searchers}); "
      "local selected=object(); weak.pkg=pkg; weak.loaded=selected; "
      "weak.searchers=searchers; weak.data=data; weak.owner=owner; "
      "debug.setupvalue(req,1,pkg); registry._LOADED=selected end; "
      "local v,d=req(original_name); assert(v==42 and d==weak.data); "
      "local count=0; for _,value in pairs(weak.loaded) do "
      "assert(value==42); count=count+1 end; assert(count==1); "
      "assert(next(registry._LOADED)==nil); d=nil; "
      "debug.setupvalue(req,1,original); registry._LOADED=loaded; "
      "collectgarbage('collect'); collectgarbage('collect'); "
      "assert(finalized==5)");
  require(test->released == 1);
}
static void native_enter(LuaExecution *execution, lua_State *state) {
  (void)state;
  ((struct fixture *)execution->context)->admissions++;
}
static int native_loader(lua_State *state) {
  require(luaQ_current() != NULL && luaQ_current()->native_depth != 0);
  lua_pushinteger(state, 42);
  return 1;
}
static int native_searcher(lua_State *state) {
  require(luaQ_current() != NULL && luaQ_current()->native_depth != 0);
  lua_pushcfunction(state, native_loader);
  return 1;
}
static void admission(struct fixture *test) {
  LuaExecution execution = {0};
  NativeStackContext *previous = luaE_getnativestackcontext(),
                     context = *previous;
  lua_State *state = test->state;
  lua_pushcfunction(state, native_loader);
  lua_setglobal(state, "native_loader");
  lua_pushcfunction(state, native_searcher);
  lua_setglobal(state, "native_searcher");
  execute(
      state,
      "package.preload.owned=function(n,d) assert(d==':preload:'); return 42 "
      "end; "
      "package.preload.foreign=native_loader; "
      "local file=package.searchers[2]; "
      "function file_call() "
      "assert(type(file('luthr_package_absent_123'))=='string') end; "
      "function path_call() "
      "assert(package.searchpath('absent','/luthr-absent/?.lua')==nil) end; "
      "function library_call() "
      "assert(package.loadlib('/luthr-absent/lib.so','none')==nil) end; "
      "local preload=package.searchers[1]; package.searchers={preload, "
      "function(n) if n=='scripted' then return function() return 42 end end; "
      "if n=='native' then return native_searcher(n) end end}; "
      "function owned_call() assert(require('owned')==42); "
      "assert(require('owned')==42); assert(require('scripted')==42) end; "
      "function foreign_call() assert(require('foreign')==42) end; "
      "function search_call() assert(require('native')==42) end");
  execution.context = test;
  execution.native_enter = native_enter;
  context.execution = &execution;
  luaE_setnativestackcontext(&context);
  for (int i = 0; i < 6; ++i) {
    static const char *const names[] = {"owned_call",  "foreign_call",
                                        "search_call", "file_call",
                                        "path_call",   "library_call"};
    test->admissions = 0;
    lua_getglobal(state, names[i]);
    require(lua_pcall(state, 0, 0, 0) == LUA_OK);
    require(test->admissions == (i == 0 ? 0u : i == 2 ? 2u : 1u));
  }
  require(execution.native_depth == 0 && execution.managed_depth == 0);
  luaE_setnativestackcontext(previous);
}
static void oom(struct fixture *test) {
  lua_State *state = test->state;
  execute(state, "local message=string.rep('missing ',1024); "
                 "package.searchers={function() return message end, "
                 "function(n) if n=='oom_missing' then return nil end; "
                 "return function() return {answer=42} end, {token=42} end}; "
                 "function attempt() package.loaded.oom_ok=nil; return "
                 "require('oom_ok') end; "
                 "function absent() return require('oom_missing') end");
  for (int kind = 0; kind < 2; ++kind) {
    int complete = 0;
    for (unsigned budget = 0; budget < 160 && !complete; ++budget) {
      int status;
      lua_getglobal(state, kind ? "absent" : "attempt");
      test->remaining = budget;
      test->failures = 0;
      test->deny = 1;
      status = lua_pcall(state, 0, 2, 0);
      test->deny = 0;
      if (test->failures == 0) {
        if (kind)
          require(status == LUA_ERRRUN &&
                  strstr(lua_tostring(state, -1), "not found") != NULL);
        else {
          require(status == LUA_OK && lua_istable(state, 1) &&
                  lua_istable(state, 2));
          lua_getfield(state, 1, "answer");
          require(lua_tointeger(state, -1) == 42);
        }
        complete = 1;
      } else
        require(status == LUA_ERRMEM);
      require(state->native_roots == NULL && state->object_scopes == NULL);
      lua_settop(state, 0);
      lua_gc(state, LUA_GCCOLLECT);
    }
    require(complete);
  }
}
static int stripped(lua_State *state) {
  lua_CFunction function = lua_tocfunction(state, 1);
  require(function != NULL);
  lua_pushcfunction(state, function);
  return 1;
}
static int join_loader(lua_State *state) {
  int status;
  (void)state;
  status = pthread_barrier_wait(&active->rendezvous);
  require(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
  return 0;
}
static void *load_parallel(void *context) {
  struct actor *actor = context;
  actor->status = lua_resume(actor->state, NULL, 0, &actor->results);
  return NULL;
}
static void parallel(struct fixture *test) {
  struct actor actors[2] = {{0}};
  pthread_t workers[2];
  const void *values[2];
  lua_State *state = test->state;
  require(pthread_barrier_init(&test->rendezvous, NULL, 2) == 0);
  lua_pushcfunction(state, join_loader);
  lua_setglobal(state, "join_loader");
  execute(state, "package.preload.parallel=function(n,d) assert(n=='parallel' "
                 "and d==':preload:'); join_loader(); "
                 "collectgarbage('collect'); return {answer=42} end");
  for (int i = 0; i < 2; ++i) {
    actors[i].state = lua_newthread(state);
    require(luaL_loadstring(actors[i].state, "return require('parallel')") ==
            LUA_OK);
  }
  for (int i = 0; i < 2; ++i)
    require(pthread_create(&workers[i], NULL, load_parallel, &actors[i]) == 0);
  for (int i = 0; i < 2; ++i)
    require(pthread_join(workers[i], NULL) == 0);
  for (int i = 0; i < 2; ++i) {
    lua_State *child = actors[i].state;
    require(actors[i].status == LUA_OK && actors[i].results == 2);
    require(lua_istable(child, 1) &&
            strcmp(lua_tostring(child, 2), ":preload:") == 0);
    values[i] = lua_topointer(child, 1);
    lua_getfield(child, 1, "answer");
    require(lua_tointeger(child, -1) == 42);
    require(child->native_roots == NULL && child->object_scopes == NULL);
  }
  lua_getglobal(state, "package");
  lua_getfield(state, -1, "loaded");
  lua_getfield(state, -1, "parallel");
  require(lua_topointer(state, -1) == values[0] ||
          lua_topointer(state, -1) == values[1]);
  lua_settop(state, 0);
  require(pthread_barrier_destroy(&test->rendezvous) == 0);
}
static void *run(void *context) {
  struct fixture *test = context;
  lua_State *state;
  if (strstr(test->mode, "facade")) {
    static const unsigned char source[] = "require('fixture')";
    luthr_lua_runtime_allocator_config config = {0};
    config.user = test;
    config.alloc_fn = host_allocate;
    config.realloc_fn = allocate;
    config.free_fn = host_free;
    require(luthr_lua_runtime_new_with_allocator(&test->runtime, &config) ==
            LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_openlibs(test->runtime) == LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_register_c_module(
                test->runtime, "fixture", capture) == LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_run_buffer(test->runtime, source,
                                         sizeof(source) - 1, "package-library",
                                         0, NULL, 0) == LUTHR_LUA_RUNTIME_OK);
  } else {
    test->state = lua_newstate(allocate, test, 0);
    require(test->state != NULL);
    luaL_openlibs(test->state);
  }
  state = test->state;
  if (strstr(test->mode, "generational"))
    lua_gc(state, LUA_GCGEN);
  if (strstr(test->mode, "semantics"))
    execute(
        state,
        "local count=0; package.preload.answer=function(n,d) count=count+1; "
        "assert(n=='answer' and d==':preload:'); return {answer=42} end; "
        "local v,d=require('answer'); assert(v.answer==42 and d==':preload:'); "
        "assert(select('#',require('answer'))==1 and count==1); "
        "package.preload.noresult=function() end; "
        "assert(require('noresult')==true and package.loaded.noresult==true); "
        "package.preload.manual=function() package.loaded.manual=42 end; "
        "assert(require('manual')==42); "
        "package.preload.negative=function() count=count+1; return false end; "
        "assert(require('negative')==false and require('negative')==false and "
        "count==3); "
        "package.preload.parent=function() return require('answer') end; "
        "assert(require('parent')==v); "
        "package.preload['42']=function(n) assert(n=='42'); return 42 end; "
        "assert(require(42)==42); package.preload.nul=function(n) "
        "assert(n=='nul\\0tail'); return 42 end; "
        "assert(require('nul\\0tail')==42); "
        "local data={}; local searchers=package.searchers; package.searchers={ "
        "function(n) return false end, function(n) return function(n,d) "
        "assert(n=='custom' and d==data); return 42 end,data end}; "
        "local a,b=require('custom'); assert(a==42 and b==data); "
        "package.searchers=searchers");
  else if (strstr(test->mode, "errors"))
    execute(state,
            "assert(not pcall(require,{})); local original=package.searchers; "
            "package.searchers=false; assert(not pcall(require,'none')); "
            "local token={}; package.searchers={function() error(token) end}; "
            "local ok,e=pcall(require,'none'); assert(not ok and e==token); "
            "package.searchers={function() return function() error(token) end "
            "end}; "
            "ok,e=pcall(require,'none'); assert(not ok and e==token); "
            "package.searchers={function() return string.rep('x',4096) end, "
            "function() return 42 end, function() return nil end}; "
            "ok,e=pcall(require,'missing'); assert(not ok and "
            "e:find(\"module 'missing' not "
            "found:\\n\\t\"..string.rep('x',4096)..'\\n\\t42',1,true)); "
            "package.searchers=original; package.preload.retry=function() "
            "error(token) end; "
            "assert(not pcall(require,'retry')); "
            "package.preload.retry=function() return 42 end; "
            "assert(require('retry')==42)");
  else if (strstr(test->mode, "metamethods"))
    execute(
        state,
        "local registry=debug.getregistry(); local oldloaded=registry._LOADED; "
        "local oldpreload=registry._PRELOAD; local "
        "_,oldpackage=debug.getupvalue(require,1); "
        "local preload=package.searchers[1]; local store={}; local token={}; "
        "local calls=0; "
        "registry._LOADED=setmetatable({}, {__index=function(t,k) "
        "collectgarbage('collect'); if k=='get_error' then error(token) end; "
        "return store[k] end, "
        "__newindex=function(t,k,v) collectgarbage('collect'); "
        "if k=='set_error' then error(token) end; store[k]=v end}); "
        "registry._PRELOAD=setmetatable({}, {__index=function(t,k) "
        "collectgarbage('collect'); if k=='pre_error' then error(token) end; "
        "return function(n,d) assert(n==k and d==':preload:'); calls=calls+1; "
        "if k=='nil_result' then return end; return {answer=42} end end}); "
        "debug.setupvalue(require,1,setmetatable({}, {__index=function(t,k) "
        "assert(k=='searchers'); collectgarbage('collect'); return {preload} "
        "end})); "
        "local v,d=require('meta'); assert(v.answer==42 and d==':preload:'); "
        "assert(require('meta')==v and calls==1); "
        "assert(require('nil_result')==true); "
        "assert(store.nil_result==true); for _,name in "
        "ipairs({'get_error','set_error','pre_error'}) do "
        "local ok,e=pcall(require,name); assert(not ok and e==token) end; "
        "debug.setupvalue(require,1,oldpackage); registry._LOADED=oldloaded; "
        "registry._PRELOAD=oldpreload");
  else if (strstr(test->mode, "snapshot"))
    snapshot(test);
  else if (strstr(test->mode, "admission"))
    admission(test);
  else if (strstr(test->mode, "parallel"))
    parallel(test);
  else if (strstr(test->mode, "bindings")) {
    lua_pushcfunction(state, stripped);
    lua_setglobal(state, "stripped");
    execute(
        state,
        "local r=stripped(require); assert(r('_G')==_G); "
        "assert(not pcall(r,'absent')); local _,p=debug.getupvalue(require,1); "
        "debug.setupvalue(require,1,false); assert(require('_G')==_G); "
        "assert(not pcall(require,'absent')); debug.setupvalue(require,1,p)");
  } else
    oom(test);
  require(state->native_roots == NULL && state->object_scopes == NULL);
  if (test->runtime)
    luthr_lua_runtime_free(test->runtime);
  else
    lua_close(state);
  require(test->live == 0);
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
