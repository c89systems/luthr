#define _GNU_SOURCE
#include <errno.h>
#include <lapi.h>
#include <lauxlib.h>
#include <lstate.h>
#include <lualib.h>
#include <luthr/lua_runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
  const char *mode, *directory;
  char *path;
  size_t live;
  unsigned remaining, failures, opened, closed, released, finalized, admissions;
  int deny, fail_open, armed, pinned;
  pthread_t carrier;
};
static struct fixture *active;
FILE *__real_fopen(const char *, const char *);
int __real_fclose(FILE *);
FILE *__wrap_fopen(const char *path, const char *mode) {
  FILE *file;
  if (active->fail_open) {
    errno = EMFILE;
    return NULL;
  }
  file = __real_fopen(path, mode);
  if (file)
    active->opened++;
  return file;
}
/* Both entrypoints have 64-bit offsets on the supported x86_64 targets. */
FILE *__wrap_fopen64(const char *path, const char *mode) {
  return __wrap_fopen(path, mode);
}
int __wrap_fclose(FILE *file) {
  active->closed++;
  return __real_fclose(file);
}
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
  if (test->deny && size > old_size) {
    if (!test->remaining) {
      test->failures++;
      return NULL;
    }
    test->remaining--;
  }
  memory = malloc(size);
  if (memory) {
    if (old) {
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
    fprintf(stderr, "file library: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
}
static void write_file(struct fixture *test, const char *bytes, size_t size) {
  FILE *file = fopen(test->path, "wb");
  require(file != NULL && fwrite(bytes, 1, size, file) == size);
  require(fclose(file) == 0);
}
static void *release_input(void *context, void *bytes, size_t old_size,
                           size_t size) {
  struct fixture *test = context;
  require(old_size > 0 && size == 0);
  test->released++;
  free(bytes);
  return NULL;
}
static void external(lua_State *state, const char *bytes, size_t size) {
  char *copy = malloc(size + 1);
  require(copy != NULL);
  memcpy(copy, bytes, size);
  copy[size] = 0;
  lua_pushexternalstring(state, copy, size, release_input, active);
}
static int finalize(lua_State *state) {
  (void)state;
  active->finalized++;
  return 0;
}
static void snapshot_poll(lua_State *state, void *context) {
  struct fixture *test = context;
  if (test->armed) {
    LuaWorldScope scope = luaW_detach(state);
    lua_Debug frame;
    test->armed = 0;
    require(lua_getstack(state, 0, &frame));
    for (int arg = 1; arg <= 3; ++arg) {
      lua_pushnil(state);
      require(lua_setlocal(state, &frame, arg) != NULL);
    }
    lua_gc(state, LUA_GCCOLLECT);
    lua_gc(state, LUA_GCCOLLECT);
    require(test->released == 0 && test->finalized == 0);
    luaW_attach(state, scope);
  }
}
static void snapshot(struct fixture *test) {
  lua_State *state = test->state;
  char *source = malloc(8194), *mode = malloc(8194);
  require(source != NULL && mode != NULL);
  memset(source, ' ', 8194);
  source[0] = source[1] = '-';
  memcpy(source + 8194 - 14, "\nreturn marker", 14);
  write_file(test, source, 8194);
  free(source);
  lua_getglobal(state, "loadfile");
  external(state, test->path, strlen(test->path));
  memset(mode, ' ', 8194);
  mode[0] = 't';
  external(state, mode, 8194);
  free(mode);
  lua_newtable(state);
  lua_pushinteger(state, 42);
  lua_setfield(state, -2, "marker");
  lua_newtable(state);
  lua_pushcfunction(state, finalize);
  lua_setfield(state, -2, "__gc");
  lua_setmetatable(state, -2);
  state->state_poll = snapshot_poll;
  state->state_poll_context = test;
  test->armed = 1;
  require(lua_pcall(state, 3, 1, 0) == LUA_OK && !test->armed &&
          lua_isfunction(state, 1));
  state->state_poll = NULL;
  require(lua_pcall(state, 0, 1, 0) == LUA_OK && lua_tointeger(state, 1) == 42);
  lua_settop(state, 0);
  lua_gc(state, LUA_GCCOLLECT);
  lua_gc(state, LUA_GCCOLLECT);
  require(test->released == 2 && test->finalized == 1);
}
static int check_body(lua_State *state) {
  LuaExecution *execution = luaQ_current();
  (void)state;
  require(execution != NULL &&
          (execution->native_depth != 0) == active->pinned);
  if (active->pinned)
    require(pthread_equal(active->carrier, pthread_self()));
  return 0;
}
static int native_dofile(lua_State *state) {
  lua_getglobal(state, "dofile");
  lua_getglobal(state, "path");
  lua_call(state, 1, 1);
  return 1;
}
static void native_enter(LuaExecution *execution, lua_State *state) {
  (void)state;
  ((struct fixture *)execution->context)->admissions++;
}
static void admission(struct fixture *test) {
  static const char source[] = "check_body(); return 42";
  LuaExecution execution = {0};
  NativeStackContext *previous = luaE_getnativestackcontext(),
                     context = *previous;
  lua_State *state = test->state;
  unsigned baseline;
  write_file(test, source, sizeof(source) - 1);
  luaQ_pushmanaged(state, check_body, 0);
  lua_setglobal(state, "check_body");
  lua_pushcfunction(state, native_dofile);
  lua_setglobal(state, "native_dofile");
  execute(state, "function loaded() return assert(loadfile(path))() end; "
                 "function direct() return dofile(path) end");
  execution.context = test;
  execution.native_enter = native_enter;
  context.execution = &execution;
  luaE_setnativestackcontext(&context);
  test->carrier = pthread_self();
  baseline = 0;
  for (int i = 0; i < 3; ++i) {
    test->pinned = i == 2;
    test->admissions = 0;
    lua_getglobal(state, i == 0   ? "loaded"
                         : i == 1 ? "direct"
                                  : "native_dofile");
    require(lua_pcall(state, 0, 1, 0) == LUA_OK &&
            lua_tointeger(state, -1) == 42);
    lua_pop(state, 1);
    if (i == 0) {
      baseline = test->admissions;
      require(baseline >= 3);
    } else
      require(test->admissions == baseline + (i == 2 ? 1u : 0u));
    require(execution.native_depth == 0 && execution.managed_depth == 0);
  }
  luaE_setnativestackcontext(previous);
}
static void oom(struct fixture *test) {
  lua_State *state = test->state;
  execute(state,
          "function attempt() return loadfile(path,'t',{marker=42}) end; "
          "function direct() marker=42; return dofile(path) end");
  for (int kind = 0; kind < 2; ++kind) {
    int complete = 0;
    for (unsigned budget = 0; budget < 160 && !complete; ++budget) {
      int status;
      lua_getglobal(state, kind ? "direct" : "attempt");
      test->remaining = budget;
      test->failures = 0;
      test->deny = 1;
      status = lua_pcall(state, 0, 2, 0);
      test->deny = 0;
      if (status == LUA_OK &&
          (kind ? lua_isinteger(state, 1) : lua_isfunction(state, 1))) {
        if (!kind) {
          lua_pop(state, 1);
          require(lua_pcall(state, 0, 1, 0) == LUA_OK);
        }
        require(lua_tointeger(state, 1) == 42);
        complete = 1;
      } else {
        require(status == LUA_ERRMEM ||
                (status == LUA_OK && lua_isnil(state, 1)));
        require(test->failures > 0);
      }
      require(test->opened == test->closed);
      require(state->native_roots == NULL && state->object_scopes == NULL);
      lua_settop(state, 0);
      lua_gc(state, LUA_GCCOLLECT);
    }
    require(complete);
  }
}
static void stdin_case(struct fixture *test) {
  lua_State *state = test->state;
  int saved = dup(STDIN_FILENO);
  FILE *file = fopen(test->path, "rb");
  require(saved >= 0 && file != NULL && dup2(fileno(file), STDIN_FILENO) >= 0);
  require(fclose(file) == 0);
  clearerr(stdin);
  execute(state, "marker=42; local f=assert(loadfile()); assert(f()==42)");
  rewind(stdin);
  execute(state, "assert(dofile(nil)==42)");
  require(dup2(saved, STDIN_FILENO) >= 0 && close(saved) == 0);
  clearerr(stdin);
}
static void *run(void *context) {
  static const char source[] = "return marker,42,nil,7\n";
  struct fixture *test = context;
  lua_State *state;
  size_t size = strlen(test->directory) + 40;
  int descriptor;
  test->path = malloc(size);
  require(test->path != NULL);
  require(snprintf(test->path, size, "%s/file-library-XXXXXX",
                   test->directory) > 0);
  descriptor = mkstemp(test->path);
  require(descriptor >= 0 && close(descriptor) == 0);
  if (strstr(test->mode, "facade")) {
    static const unsigned char load[] = "require('fixture')";
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
    require(luthr_lua_runtime_run_buffer(test->runtime, load, sizeof(load) - 1,
                                         "file-library", 0, NULL,
                                         0) == LUTHR_LUA_RUNTIME_OK);
  } else {
    test->state = lua_newstate(allocate, test, 0);
    require(test->state != NULL);
    luaL_openlibs(test->state);
  }
  state = test->state;
  if (strstr(test->mode, "generational"))
    lua_gc(state, LUA_GCGEN);
  lua_pushstring(state, test->path);
  lua_setglobal(state, "path");
  lua_pushstring(state, test->directory);
  lua_setglobal(state, "directory");
  write_file(test, source, sizeof(source) - 1);
  if (strstr(test->mode, "semantics"))
    execute(state,
            "local f=assert(loadfile(path,'t',{marker=9})); local a,b,c,d=f(); "
            "assert(a==9 and b==42 and c==nil and d==7); marker=9; "
            "a,b,c,d=dofile(path); assert(a==9 and b==42 and c==nil and d==7); "
            "assert(select('#',dofile(path))==4); "
            "assert(not pcall(assert(loadfile(path,'t',nil)))); "
            "local old=dofile; assert(require('_G').dofile==old)");
  else if (strstr(test->mode, "errors")) {
    execute(
        state,
        "for _,f in ipairs({function() loadfile({}) end, "
        "function() loadfile(path,{}) end,function() loadfile(path,'B') end, "
        "function() dofile({}) end}) do assert(not pcall(f)) end; "
        "local f,e=loadfile(path..'.absent'); assert(f==nil and "
        "type(e)=='string'); "
        "assert(not pcall(dofile,path..'.absent')); "
        "f,e=loadfile(directory); assert(f==nil and type(e)=='string'); "
        "assert(not pcall(dofile,directory)); assert(loadfile(path,'b')==nil)");
    test->fail_open = 1;
    execute(state, "local f,e=loadfile(path); assert(f==nil and "
                   "type(e)=='string'); assert(not pcall(dofile,path))");
    test->fail_open = 0;
    write_file(test, "return {", 8);
    execute(state,
            "assert(loadfile(path)==nil); assert(not pcall(dofile,path))");
  } else if (strstr(test->mode, "binary")) {
    size_t len;
    const char *bytes;
    execute(state, "binary=string.dump(function() return marker,42 end)");
    lua_getglobal(state, "binary");
    bytes = lua_tolstring(state, -1, &len);
    write_file(test, bytes, len);
    lua_pop(state, 1);
    execute(state, "local a,b=assert(loadfile(path,'b',{marker=9}))(); "
                   "assert(a==9 and b==42); "
                   "marker=9; a,b=dofile(path); assert(a==9 and b==42); "
                   "assert(loadfile(path,'t')==nil)");
  } else if (strstr(test->mode, "yield"))
    execute(state,
            "local f=assert(io.open(path,'wb')); f:write('local "
            "x=coroutine.yield(7); return x,42'); f:close(); "
            "local co=coroutine.create(function() return dofile(path) end); "
            "local ok,v=coroutine.resume(co); assert(ok and v==7); "
            "local a,b; ok,a,b=coroutine.resume(co,9); assert(ok and a==9 and "
            "b==42); "
            "assert(coroutine.status(co)=='dead'); "
            "co=coroutine.create(function() return dofile(path) end); "
            "assert(coroutine.resume(co)); "
            "assert(coroutine.close(co))");
  else if (strstr(test->mode, "bom")) {
    static const char bom[] = "\357\273\277#!/usr/bin/luthr\nreturn 42\n";
    write_file(test, bom, sizeof(bom) - 1);
    execute(state, "assert(assert(loadfile(path))()==42 and dofile(path)==42)");
    write_file(test, "", 0);
    execute(state, "assert(select('#',assert(loadfile(path))())==0 and "
                   "select('#',dofile(path))==0)");
  } else if (strstr(test->mode, "stack")) {
    static const char recursive[] =
        "file_depth=(file_depth or 0)+1; return dofile(path)\n";
    write_file(test, recursive, sizeof(recursive) - 1);
    execute(
        state,
        "local ok,e=pcall(dofile,path); assert(not ok and type(e)=='string' "
        "and e:find('stack overflow',1,true)); assert(file_depth>1)");
    write_file(test, "return 42", 9);
    execute(state, "assert(dofile(path)==42)");
  } else if (strstr(test->mode, "stdin"))
    stdin_case(test);
  else if (strstr(test->mode, "snapshot"))
    snapshot(test);
  else if (strstr(test->mode, "admission"))
    admission(test);
  else
    oom(test);
  require(state->native_roots == NULL && state->object_scopes == NULL);
  if (test->runtime)
    luthr_lua_runtime_free(test->runtime);
  else
    lua_close(state);
  if (test->live != 0 || test->opened != test->closed)
    fprintf(stderr, "file cleanup: live=%zu opened=%u closed=%u\n", test->live,
            test->opened, test->closed);
  require(test->live == 0 && test->opened == test->closed);
  require(unlink(test->path) == 0);
  free(test->path);
  return NULL;
}
int main(int argc, char **argv) {
  struct fixture *test = calloc(1, sizeof(*test));
  pthread_t worker;
  require(test != NULL && argc == 3);
  active = test;
  test->mode = argv[1];
  test->directory = argv[2];
#if defined(LUTHR_TEST_SMALL_NATIVE_STACK)
  if (strstr(test->mode, "stack")) {
    pthread_attr_t attributes;
    require(pthread_attr_init(&attributes) == 0);
    require(pthread_attr_setstacksize(&attributes, 128u * 1024u) == 0);
    require(pthread_create(&worker, &attributes, run, test) == 0);
    require(pthread_attr_destroy(&attributes) == 0);
  } else
#endif
    require(pthread_create(&worker, NULL, run, test) == 0);
  require(pthread_join(worker, NULL) == 0);
  free(test);
  return 0;
}
