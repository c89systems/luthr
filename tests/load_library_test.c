#include <lapi.h>
#include <lauxlib.h>
#include <lstate.h>
#include <lualib.h>
#include <luthrum/lua_runtime.h>
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
struct input {
  char *bytes;
  size_t size;
  unsigned released;
};
struct fixture {
  lua_State *state;
  luthrum_lua_runtime *runtime;
  const char *mode;
  size_t live, peak, limit, external, external_peak;
  unsigned remaining, failures, calls, admissions, produced, released,
      finalized;
  int deny, armed;
  struct input inputs[3];
  pthread_t carrier;
};
static struct fixture *active;
static void *allocate(void *context, void *old, size_t old_size, size_t size) {
  struct fixture *test = context;
  void *memory;
  if (old == NULL)
    old_size = 0;
  if (!size) {
    test->live -= old_size;
    free(old);
    return NULL;
  }
  if ((test->limit && test->live - old_size + size > test->limit) ||
      (test->deny && size > old_size && test->remaining == 0)) {
    test->failures++;
    return NULL;
  }
  if (test->deny && size > old_size)
    test->remaining--;
  memory = malloc(size);
  if (memory != NULL) {
    if (old != NULL) {
      memcpy(memory, old, old_size < size ? old_size : size);
      free(old);
    }
    test->live = test->live - old_size + size;
    if (test->live > test->peak)
      test->peak = test->live;
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
  int code = luaL_loadstring(state, source);
  if (code == LUA_OK)
    code = lua_pcall(state, 0, 0, 0);
  if (code != LUA_OK)
    fprintf(stderr, "load library: %s\n", lua_tostring(state, -1));
  require(code == LUA_OK);
}
static int finalize(lua_State *state) {
  (void)state;
  active->finalized++;
  return 0;
}
static void *release_input(void *context, void *bytes, size_t old_size,
                           size_t size) {
  struct input *input = context;
  require(bytes == input->bytes && old_size == input->size && size == 0);
  input->released++;
  free(bytes);
  return NULL;
}
static void input(lua_State *state, unsigned id, size_t len, int fill,
                  const char *prefix, const char *suffix) {
  struct input *input = &active->inputs[id];
  input->size = len + 1;
  input->bytes = malloc(len + 1);
  require(input->bytes != NULL);
  memset(input->bytes, fill, len);
  memcpy(input->bytes, prefix, strlen(prefix));
  memcpy(input->bytes + len - strlen(suffix), suffix, strlen(suffix));
  input->bytes[len] = 0;
  (void)lua_pushexternalstring(state, input->bytes, len, release_input, input);
}
static int fragment(lua_State *state) {
  if (active->calls++ == 0) {
    input(state, 0, 8194, ' ', "--", "\nreturn marker");
    return 1;
  }
  return 0;
}
static void snapshot_poll(lua_State *state, void *context) {
  struct fixture *test = context;
  if (test->armed) {
    LuaWorldScope scope = luaW_detach(state);
    lua_Debug frame;
    int arg;
    test->armed = 0;
    require(lua_getstack(state, 0, &frame));
    for (arg = 1; arg <= 4; ++arg) {
      lua_pushnil(state);
      require(lua_setlocal(state, &frame, arg) != NULL);
    }
    (void)lua_gc(state, LUA_GCCOLLECT);
    (void)lua_gc(state, LUA_GCCOLLECT);
    for (arg = 0; arg < 3; ++arg)
      require(test->inputs[arg].released == 0);
    require(test->finalized == 0);
    luaW_attach(state, scope);
  }
}
static void snapshot(struct fixture *test, int reader, int binary) {
  lua_State *state = test->state;
  lua_getglobal(state, "load");
  if (reader) {
    (void)lua_newuserdatauv(state, 1, 0);
    lua_newtable(state);
    lua_pushcfunction(state, finalize);
    lua_setfield(state, -2, "__gc");
    lua_setmetatable(state, -2);
    lua_pushcclosure(state, fragment, 1);
  } else if (binary) {
    size_t len;
    const char *bytes;
    struct input *input = &test->inputs[0];
    require(luaL_loadstring(
                state,
                "return string.dump(assert(load('return function() return "
                "marker,\"'..string.rep('R',8194)..'\" end'))())") == LUA_OK);
    require(lua_pcall(state, 0, 1, 0) == LUA_OK);
    bytes = lua_tolstring(state, -1, &len);
    require(bytes != NULL && len > 8194);
    input->size = len + 1;
    input->bytes = malloc(len + 1);
    require(input->bytes != NULL);
    memcpy(input->bytes, bytes, len);
    input->bytes[len] = 0;
    lua_pop(state, 1);
    (void)lua_pushexternalstring(state, input->bytes, len, release_input,
                                 input);
  } else
    input(state, 0, 8194, ' ', "--", "\nreturn marker");
  input(state, 1, 2050, 'N', "=original-source", "");
  input(state, 2, 2050, ' ', binary ? "b" : "t", "");
  lua_newtable(state);
  lua_pushinteger(state, 42);
  lua_setfield(state, -2, "marker");
  lua_newtable(state);
  lua_pushcfunction(state, finalize);
  lua_setfield(state, -2, "__gc");
  lua_setmetatable(state, -2);
  test->armed = 1;
  state->state_poll = snapshot_poll;
  state->state_poll_context = test;
  require(lua_pcall(state, 4, 1, 0) == LUA_OK && !test->armed &&
          lua_isfunction(state, 1));
  state->state_poll = NULL;
  require(lua_pcall(state, 0, 1, 0) == LUA_OK && lua_tointeger(state, 1) == 42);
  lua_settop(state, 0);
  (void)lua_gc(state, LUA_GCCOLLECT);
  (void)lua_gc(state, LUA_GCCOLLECT);
  for (unsigned i = 0; i < 3; ++i)
    require(test->inputs[i].released == 1);
  require(test->finalized == (reader ? 2u : 1u));
  if (reader)
    require(test->calls == 2);
}
static void *release_fragment(void *context, void *bytes, size_t old_size,
                              size_t size) {
  struct fixture *test = context;
  require(old_size == 2049 && size == 0);
  test->released++;
  test->external -= old_size;
  free(bytes);
  return NULL;
}
static int stream_reader(lua_State *state) {
  struct fixture *test = active;
  if (test->calls++ < 512) {
    char *bytes;
    (void)lua_gc(state, LUA_GCCOLLECT);
    require(test->produced - test->released <= 1);
    bytes = malloc(2049);
    require(bytes != NULL);
    memset(bytes, 'a', 2048);
    bytes[0] = bytes[1] = '-';
    bytes[2047] = '\n';
    bytes[2048] = 0;
    test->produced++;
    test->external += 2049;
    if (test->external > test->external_peak)
      test->external_peak = test->external;
    (void)lua_pushexternalstring(state, bytes, 2048, release_fragment, test);
    return 1;
  }
  if (test->calls == 514)
    return 0;
  lua_pushliteral(state, "return 42");
  return 1;
}
static void stream(struct fixture *test) {
  lua_State *state = test->state;
  test->limit = 128u * 1024u;
  lua_getglobal(state, "load");
  lua_pushcfunction(state, stream_reader);
  require(lua_pcall(state, 1, 1, 0) == LUA_OK && lua_isfunction(state, 1));
  require(lua_pcall(state, 0, 1, 0) == LUA_OK && lua_tointeger(state, 1) == 42);
  lua_settop(state, 0);
  (void)lua_gc(state, LUA_GCCOLLECT);
  (void)lua_gc(state, LUA_GCCOLLECT);
  require(test->produced == 512 && test->released == 512 &&
          test->external == 0 && test->external_peak <= 4098);
  require(test->failures == 0 && test->peak <= test->limit);
}
static const char *public_reader(lua_State *state, void *context,
                                 size_t *size) {
  (void)state;
  struct fixture *test = context;
  LuaExecution *execution = luaQ_current();
  require(execution != NULL && execution->native_depth != 0 &&
          pthread_equal(test->carrier, pthread_self()));
  test->calls++;
  if (test->calls == 1) {
    *size = 9;
    return "return 42";
  }
  *size = 0;
  return NULL;
}
static int native_reader(lua_State *state) {
  require(luaQ_current() != NULL && luaQ_current()->native_depth != 0);
  active->calls++;
  if (active->calls == 1) {
    lua_pushliteral(state, "return 42");
    return 1;
  }
  return 0;
}
static void native_enter(LuaExecution *execution, lua_State *state) {
  (void)state;
  ((struct fixture *)execution->context)->admissions++;
}
static void admission(struct fixture *test) {
  lua_State *state = test->state;
  LuaExecution execution = {0};
  NativeStackContext *previous = luaE_getnativestackcontext(),
                     context = *previous;
  execute(state, "function owned() return assert(load('return 42'))() end; "
                 "function scripted() local n=0; return assert(load(function() "
                 "n=n+1; if n==1 then return 'return 42' end end))() end");
  execution.context = test;
  execution.native_enter = native_enter;
  context.execution = &execution;
  (void)luaE_setnativestackcontext(&context);
  test->carrier = pthread_self();
  for (int i = 0; i < 2; ++i) {
    lua_getglobal(state, i ? "scripted" : "owned");
    require(lua_pcall(state, 0, 1, 0) == LUA_OK &&
            lua_tointeger(state, -1) == 42);
    lua_pop(state, 1);
    require(test->admissions == 0);
  }
  lua_getglobal(state, "load");
  lua_pushcfunction(state, native_reader);
  require(lua_pcall(state, 1, 1, 0) == LUA_OK && lua_isfunction(state, -1));
  lua_pop(state, 1);
  require(test->calls == 2 && test->admissions == 2);
  test->calls = test->admissions = 0;
  require(lua_load(state, public_reader, test, "=public", "t") == LUA_OK);
  require(test->calls == 2 && test->admissions == 2 &&
          execution.native_depth == 0 && execution.managed_depth == 0);
  lua_pop(state, 1);
  (void)luaE_setnativestackcontext(previous);
}
static void oom(struct fixture *test) {
  lua_State *state = test->state;
  execute(state,
          "source='local n=0; '..string.rep('n=n+1; ',128)..'return n'; "
          "binary=string.dump(assert(load(source))); "
          "function text_load() return load(source,'=text','t') end; "
          "function binary_load() return load(binary,'=binary','b') end; "
          "function reader_load() local n=0; return load(function() n=n+1; if "
          "n==1 then return source end end) end");
  for (int kind = 0; kind < 3; ++kind) {
    int complete = 0;
    for (unsigned budget = 0; budget < 160 && !complete; ++budget) {
      int status;
      lua_getglobal(state, kind == 0   ? "text_load"
                           : kind == 1 ? "binary_load"
                                       : "reader_load");
      test->remaining = budget;
      test->failures = 0;
      test->deny = 1;
      status = lua_pcall(state, 0, 2, 0);
      test->deny = 0;
      if (status == LUA_OK && lua_isfunction(state, 1)) {
        lua_pop(state, 1);
        require(lua_pcall(state, 0, 1, 0) == LUA_OK &&
                lua_tointeger(state, 1) == 128);
        complete = 1;
      } else {
        require(status == LUA_ERRMEM ||
                (status == LUA_OK && lua_isnil(state, 1)));
        require(test->failures > 0);
      }
      require(state->native_roots == NULL && state->object_scopes == NULL);
      lua_settop(state, 0);
      (void)lua_gc(state, LUA_GCCOLLECT);
    }
    require(complete);
  }
}
static void *run(void *context) {
  struct fixture *test = context;
  lua_State *state;
  int facade = strstr(test->mode, "facade") != NULL;
  if (facade) {
    static const unsigned char src[] = "require('fixture')";
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
    require(luthrum_lua_runtime_run_buffer(test->runtime, src, sizeof(src) - 1,
                                           "load-library", 0, NULL,
                                           0) == LUTHRUM_LUA_RUNTIME_OK);
  } else {
    test->state = lua_newstate(allocate, test, 0);
    require(test->state != NULL);
    luaL_openlibs(test->state);
  }
  state = test->state;
  if (strstr(test->mode, "generational"))
    (void)lua_gc(state, LUA_GCGEN);
  if (strstr(test->mode, "semantics"))
    execute(state,
            "assert(assert(load('return 42'))()==42); local "
            "a,b=assert(load('return marker,_ENV','=name','t',{marker=42}))(); "
            "assert(a==42 and b.marker==42); "
            "assert(assert(load('return _ENV','=nil','t',nil))()==nil); "
            "local n=0; assert(assert(load(function() n=n+1; if n==1 then "
            "return 'return ' elseif n==2 then return 42 end end))()==42); "
            "local binary=string.dump(function() return 42 end); "
            "assert(assert(load(binary,'=binary','b'))()==42); "
            "local i=0; assert(assert(load(function() i=i+1; return i<=#binary "
            "and binary:sub(i,i) or nil end,'=pieces','b'))()==42); "
            "local token={}; local f,e=load(function() error(token) end); "
            "assert(f==nil and e==token); "
            "local s='return \"a\\0b\"'; "
            "assert(assert(load(s))()=='a'..string.char(0)..'b'); "
            "assert(type(assert(load('return 1','=source')))=='function'); "
            "local before=load; local base=assert(package.loaded._G); "
            "assert(base.load==before)");
  else if (strstr(test->mode, "errors"))
    execute(
        state,
        "for _,f in ipairs({function() load({}) end,function() load('return "
        "1',{}) end,function() load('return 1','n',{}) end,function() "
        "load('return 1','n','B') end}) do assert(not pcall(f)) end; "
        "for _,f in ipairs({function() return load('return {') end,function() "
        "return load('return 1','n','b') end,function() return "
        "load(string.dump(function() end),'n','t') end,function() return "
        "load(function() return {} end) end}) do local v,e=f(); assert(v==nil "
        "and type(e)=='string') end; "
        "assert(select('#',load(function() return nil end))==1)");
  else if (strstr(test->mode, "reader_snapshot"))
    snapshot(test, 1, 0);
  else if (strstr(test->mode, "binary_snapshot"))
    snapshot(test, 0, 1);
  else if (strstr(test->mode, "snapshot"))
    snapshot(test, 0, 0);
  else if (strstr(test->mode, "admission"))
    admission(test);
  else if (strstr(test->mode, "stream"))
    stream(test);
  else
    oom(test);
  require(state->native_roots == NULL && state->object_scopes == NULL);
  if (facade)
    luthrum_lua_runtime_free(test->runtime);
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
