#include <lauxlib.h>
#include <ldo.h>
#include <lstate.h>
#include <lualib.h>
#include <lundump.h>
#include <luthrum/lua_runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define require(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      abort();                                                                 \
    }                                                                          \
  } while (0)
struct fixture {
  lua_State *state;
  luthrum_lua_runtime *runtime;
  const char *mode;
  size_t live, size, capacity;
  unsigned remaining, failures, finalized, released, calls, ends, fail_at,
      admissions;
  char *bytes, *input, *deep;
  size_t deep_size;
  const void *function_identity;
  int deny, armed, stack_top, writer_code, throwing, denial, snapshot,
      private_dump;
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
  if (test->deny && size > old_size) {
    if (test->remaining)
      test->remaining--;
    else {
      test->failures++;
      return NULL;
    }
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
static void host_free(void *context, void *memory, size_t size) {
  (void)allocate(context, memory, size, 0);
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
    fprintf(stderr, "dump library: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
}
static int finalize(lua_State *state) {
  (void)state;
  active->finalized++;
  return 0;
}
static int token(lua_State *state) {
  (void)lua_newuserdatauv(state, 1, 0);
  lua_newtable(state);
  lua_pushcfunction(state, finalize);
  lua_setfield(state, -2, "__gc");
  lua_setmetatable(state, -2);
  return 1;
}
static int reopen(lua_State *state) { return luaopen_string(state); }
static void prepare(lua_State *state) {
  execute(
      state,
      "f=assert(load('local token=...; return function() local _=token; local "
      "n=0; '.."
      "string.rep('n=n+1;',512)..'return n,\"'..string.rep('R',8194)..'\" "
      "end'))(token()); "
      "baseline_full=string.dump(f,false); baseline_strip=string.dump(f,true)");
}
static void verify_chunk(lua_State *state, int index) {
  size_t len;
  const char *bytes = lua_tolstring(state, index, &len);
  require(bytes != NULL && len > 8194);
  require(luaL_loadbuffer(state, bytes, len, "restored") == LUA_OK);
  require(lua_pcall(state, 0, 2, 0) == LUA_OK);
  require(lua_tointeger(state, -2) == 512);
  bytes = lua_tolstring(state, -1, &len);
  require(bytes != NULL && len == 8194 && bytes[0] == 'R' &&
          bytes[len - 1] == 'R');
  lua_pop(state, 2);
}
static void drop_function(lua_State *state) {
  lua_pushnil(state);
  lua_setglobal(state, "f");
}
static int collect_writer(lua_State *state, const void *data, size_t size,
                          void *context) {
  struct fixture *test = context;
  LuaExecution *execution = luaQ_current();
  require(pthread_equal(test->carrier, pthread_self()));
  if (execution != NULL)
    require(execution->native_depth != 0);
  require(lua_gettop(state) == test->stack_top);
  if (!test->snapshot)
    require(lua_topointer(state, 1) == test->function_identity);
  test->calls++;
  if (test->snapshot && test->calls == 1) {
    lua_settop(state, 0);
    test->stack_top = 0;
    (void)lua_gc(state, LUA_GCCOLLECT);
    (void)lua_gc(state, LUA_GCCOLLECT);
    require(test->finalized == (test->private_dump ? 1u : 0u));
  }
  if (test->throwing && test->calls == test->fail_at)
    return luaL_error(state, "writer expected");
  if (test->calls == test->fail_at || (data == NULL && test->writer_code)) {
    if (test->denial) {
      test->remaining = 0;
      test->deny = 1;
    }
    return test->writer_code;
  }
  if (data == NULL) {
    require(size == 0);
    test->ends++;
    return 0;
  }
  if (test->size + size > test->capacity) {
    size_t capacity = (test->size + size) * 2;
    char *bytes = realloc(test->bytes, capacity);
    require(bytes != NULL);
    test->bytes = bytes;
    test->capacity = capacity;
  }
  memcpy(test->bytes + test->size, data, size);
  test->size += size;
  return 0;
}
static int public_dump(lua_State *state) {
  struct fixture *test = active;
  int code;
  test->size = test->calls = test->ends = 0;
  test->stack_top = lua_gettop(state);
  test->function_identity = lua_topointer(state, 1);
  test->carrier = pthread_self();
  if (test->private_dump) {
    ptrdiff_t top;
    luaX_lockstate(state);
    top = savestack(state, state->top.p);
    code = luaU_dump(state, clLvalue(s2v(state->top.p - 1))->p, collect_writer,
                     test, 0, 0);
    state->top.p = restorestack(state, top);
    luaX_unlockstate(state);
  } else
    code = lua_dump(state, collect_writer, test, 0);
  require(lua_gettop(state) == 1);
  if (!test->snapshot)
    require(lua_topointer(state, 1) == test->function_identity);
  test->deny = 0;
  lua_pushinteger(state, code);
  if (code)
    return 1;
  require(test->ends == 1);
  lua_pushlstring(state, test->bytes, test->size);
  return 2;
}
static void snapshot_poll(lua_State *state, void *context) {
  struct fixture *test = context;
  if (test->armed) {
    LuaWorldScope scope = luaW_detach(state);
    lua_Debug frame;
    test->armed = 0;
    require(lua_getstack(state, 0, &frame));
    lua_pushnil(state);
    require(lua_setlocal(state, &frame, 1) != NULL);
    (void)lua_gc(state, LUA_GCCOLLECT);
    (void)lua_gc(state, LUA_GCCOLLECT);
    require(test->finalized == 0);
    luaW_attach(state, scope);
  }
}
static void dump_function(lua_State *state) {
  lua_getglobal(state, "string");
  lua_getfield(state, -1, "dump");
  lua_remove(state, -2);
}
static void snapshot(struct fixture *test, int public_path, int private_path) {
  lua_State *state = test->state;
  if (public_path)
    lua_pushcfunction(state, public_dump);
  else
    dump_function(state);
  lua_getglobal(state, "f");
  drop_function(state);
  test->snapshot = public_path;
  test->private_dump = private_path;
  test->armed = !public_path;
  state->state_poll = snapshot_poll;
  state->state_poll_context = test;
  require(lua_pcall(state, 1, public_path ? 2 : 1, 0) == LUA_OK);
  state->state_poll = NULL;
  require(!test->armed);
  verify_chunk(state, public_path ? 2 : 1);
  lua_getglobal(state, "baseline_full");
  require(lua_rawequal(state, -1, public_path ? 2 : 1));
  lua_settop(state, 0);
  (void)lua_gc(state, LUA_GCCOLLECT);
  (void)lua_gc(state, LUA_GCCOLLECT);
  require(test->finalized == 1);
}
static void native_enter(LuaExecution *execution, lua_State *state) {
  (void)state;
  ((struct fixture *)execution->context)->admissions++;
}
static void admission(struct fixture *test) {
  NativeStackContext *previous = luaE_getnativestackcontext(),
                     context = *previous;
  LuaExecution execution = {0};
  lua_State *state = test->state;
  execution.context = test;
  execution.native_enter = native_enter;
  context.execution = &execution;
  (void)luaE_setnativestackcontext(&context);
  dump_function(state);
  lua_getglobal(state, "f");
  require(lua_pcall(state, 1, 1, 0) == LUA_OK && test->admissions == 0);
  lua_pop(state, 1);
  lua_pushcfunction(state, public_dump);
  lua_getglobal(state, "f");
  require(lua_pcall(state, 1, 2, 0) == LUA_OK);
  /* Ordinary C entry adds one admission; each public writer remains native. */
  require(test->admissions == test->calls + 1 && execution.native_depth == 0 &&
          execution.managed_depth == 0);
  lua_settop(state, 0);
  (void)luaE_setnativestackcontext(previous);
}
static void errors(struct fixture *test) {
  lua_State *state = test->state;
  unsigned index;
  execute(state, "assert(not pcall(string.dump,{})); assert(not "
                 "pcall(string.dump,print)); "
                 "assert(not pcall(string.dump)); local mt=getmetatable(''); "
                 "for _,k in "
                 "ipairs({'__add','__sub','__mul','__mod','__pow','__div','__"
                 "idiv','__unm'}) do "
                 "assert(not pcall(mt[k],'no',{})) end; "
                 "assert(not pcall(function() return '1'//'0' end))");
  for (index = 1; index <= 24; ++index) {
    test->fail_at = index;
    test->writer_code = index % 2 ? 7 : -23;
    test->denial = 1;
    lua_pushcfunction(state, public_dump);
    lua_getglobal(state, "f");
    require(lua_pcall(state, 1, 1, 0) == LUA_OK);
    require(lua_tointeger(state, -1) == test->writer_code &&
            test->calls == index && test->ends == 0);
    lua_settop(state, 0);
    require(state->native_roots == NULL && state->object_scopes == NULL);
  }
  test->denial = 0;
  test->fail_at = 2;
  test->throwing = 1;
  lua_pushcfunction(state, public_dump);
  lua_getglobal(state, "f");
  require(lua_pcall(state, 1, 1, 0) == LUA_ERRRUN);
  lua_pop(state, 1);
  require(state->native_roots == NULL && state->object_scopes == NULL);
  test->throwing = 0;
  test->fail_at = 0;
  test->writer_code = 19;
  lua_pushcfunction(state, public_dump);
  lua_getglobal(state, "f");
  require(lua_pcall(state, 1, 1, 0) == LUA_OK &&
          lua_tointeger(state, -1) == 19);
  lua_settop(state, 0);
  test->writer_code = 0;
}
static void oom(struct fixture *test) {
  lua_State *state = test->state;
  int public_path;
  for (public_path = 0; public_path < 2; ++public_path) {
    unsigned budget;
    int complete = 0;
    for (budget = 0; budget < 64 && !complete; ++budget) {
      int status;
      if (public_path)
        lua_pushcfunction(state, public_dump);
      else
        dump_function(state);
      lua_getglobal(state, "f");
      test->remaining = budget;
      test->failures = 0;
      test->deny = 1;
      status = lua_pcall(state, 1, public_path ? 2 : 1, 0);
      test->deny = 0;
      if (status == LUA_OK) {
        verify_chunk(state, public_path ? 2 : 1);
        complete = 1;
      } else
        require(status == LUA_ERRMEM && test->failures != 0);
      require(state->native_roots == NULL && state->object_scopes == NULL);
      lua_settop(state, 0);
      (void)lua_gc(state, LUA_GCCOLLECT);
    }
    require(complete);
  }
}
size_t __real_lua_stringtonumber(lua_State *state, const char *source);
size_t __wrap_lua_stringtonumber(lua_State *state, const char *source) {
  if (active->armed && source == active->input) {
    lua_Debug frame;
    active->armed = 0;
    require(lua_getstack(state, 0, &frame));
    lua_pushnil(state);
    require(lua_setlocal(state, &frame, 1) != NULL);
    (void)lua_gc(state, LUA_GCCOLLECT);
    (void)lua_gc(state, LUA_GCCOLLECT);
    require(active->released == 0);
  }
  return __real_lua_stringtonumber(state, source);
}
static void *release_input(void *context, void *bytes, size_t old_size,
                           size_t size) {
  struct fixture *test = context;
  require(bytes == test->input && old_size == 8195 && size == 0);
  test->released++;
  free(bytes);
  return NULL;
}
static void number_snapshot(struct fixture *test) {
  lua_State *state = test->state;
  lua_pushliteral(state, "");
  require(lua_getmetatable(state, -1));
  lua_getfield(state, -1, "__add");
  lua_remove(state, -2);
  lua_remove(state, -2);
  test->input = malloc(8195);
  require(test->input != NULL);
  memset(test->input, ' ', 8194);
  memcpy(test->input, "21", 2);
  test->input[8194] = 0;
  (void)lua_pushexternalstring(state, test->input, 8194, release_input, test);
  lua_pushinteger(state, 21);
  test->armed = 1;
  require(lua_pcall(state, 2, 1, 0) == LUA_OK && !test->armed &&
          lua_tointeger(state, -1) == 42);
  lua_settop(state, 0);
  (void)lua_gc(state, LUA_GCCOLLECT);
  (void)lua_gc(state, LUA_GCCOLLECT);
  require(test->released == 1);
}
static void baseline(lua_State *state) {
  /* Lua 5.5.1 x86_64 bytes from the verified b703a61 Release runtime, before
   * these dumper changes. A fixed source name excludes checkout/build paths. */
  static const char source[] =
      "return function(a,...) local t={x=\"value\",n=1.25}; return function(b) "
      "return a+b,t.x end,... end";
  static const char *const expected[] = {
      "1b4c7561550019930d0a1a0a0488a9ffff04785634120888a9ffffffffffff0800000000"
      "002877c000010101010509005300000013010200540000001281000112810203cf010000"
      "50020100c6810002c68101020404027800040676616c75650004026e0013000000000000"
      "f43f0001010101000306000089000000a2000100ae0000060b010100c6000300c7000100"
      "0104000102010000010200000d3d64756d702d636f6d7061740006000000000000000102"
      "62000006020261000274000004090000000000000000000003000600090f287661726172"
      "67207461626c65290001090007050900",
      "1b4c7561550019930d0a1a0a0488a9ffff04785634120888a9ffffffffffff0800000000"
      "002877c000010101010509005300000013010200540000001281000112810203cf010000"
      "50020100c6810002c68101020404027800040676616c75650004026e0013000000000000"
      "f43f0001010101000306000089000000a2000100ae0000060b010100c6000300c7000100"
      "010400010201000001020000000000000000000000000000"};
  static const char digits[] = "0123456789abcdef";
  unsigned strip;
  require(luaL_loadbuffer(state, source, sizeof(source) - 1, "=dump-compat") ==
          LUA_OK);
  require(lua_pcall(state, 0, 1, 0) == LUA_OK);
  for (strip = 0; strip < 2; ++strip) {
    const unsigned char *bytes;
    size_t len, index;
    dump_function(state);
    lua_pushvalue(state, 1);
    lua_pushboolean(state, strip);
    require(lua_pcall(state, 2, 1, 0) == LUA_OK);
    bytes = (const unsigned char *)lua_tolstring(state, -1, &len);
    require(bytes != NULL && len * 2 == strlen(expected[strip]));
    for (index = 0; index < len; ++index) {
      require(expected[strip][index * 2] == digits[bytes[index] >> 4]);
      require(expected[strip][index * 2 + 1] == digits[bytes[index] & 15]);
    }
    lua_pop(state, 1);
  }
  lua_settop(state, 0);
}
static void dump_body(lua_State *state, void *context) {
  struct fixture *test = context;
  require(luaU_dump(state, clLvalue(s2v(state->top.p - 1))->p, collect_writer,
                    test, 0, 0) == 0);
}
/* Deliberately consume the caller's stack in small bounded frames. The final
 * dump must stop inside its recursion guard while error-handling reserve stays
 * available. Volatile reads after recursion prevent tail-call elimination. */
static int __attribute__((noinline)) near_limit(lua_State *state,
                                                struct fixture *test) {
  volatile unsigned char reserved[256];
  int status;
  reserved[0] = 19;
  reserved[255] = 37;
  if (luaD_nativeposition() > luaE_getnativestackcontext()->limit + 1024u)
    status = near_limit(state, test);
  else
    status = luaD_rawrunprotected(state, dump_body, test);
  require(reserved[0] == 19 && reserved[255] == 37);
  return status;
}
static void stack(struct fixture *test) {
  lua_State *state = test->state;
  int status;
  require(luaL_loadbuffer(state, test->deep, test->deep_size, "deep") ==
          LUA_OK);
  test->stack_top = 1;
  test->function_identity = lua_topointer(state, 1);
  test->carrier = pthread_self();
  luaX_lockstate(state);
  status = near_limit(state, test);
  luaX_unlockstate(state);
  require(status == LUA_ERRRUN && test->ends == 0);
  require(state->native_roots == NULL && state->object_scopes == NULL);
  lua_settop(state, 0);
  dump_function(state);
  lua_getglobal(state, "f");
  require(lua_pcall(state, 1, 1, 0) == LUA_OK);
  verify_chunk(state, 1);
  lua_settop(state, 0);
}
static void *run(void *context) {
  struct fixture *test = context;
  lua_State *state;
  int facade = strstr(test->mode, "facade") != NULL;
  if (facade) {
    static const unsigned char source[] = "require('fixture')";
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
    require(luthrum_lua_runtime_run_buffer(
                test->runtime, source, sizeof(source) - 1, "dump-library", 0,
                NULL, 0) == LUTHRUM_LUA_RUNTIME_OK);
  } else {
    test->state = lua_newstate(allocate, test, 0);
    require(test->state != NULL);
    luaL_openlibs(test->state);
  }
  state = test->state;
  if (strstr(test->mode, "generational") != NULL)
    (void)lua_gc(state, LUA_GCGEN);
  lua_pushcfunction(state, token);
  lua_setglobal(state, "token");
  lua_pushcfunction(state, reopen);
  lua_setglobal(state, "reopen");
  prepare(state);
  if (strstr(test->mode, "semantics") != NULL) {
    execute(state,
            "for _,strip in ipairs({false,true}) do local "
            "b=string.dump(f,strip); local g=assert(load(b)); "
            "local n,s=g(); assert(n==512 and s==string.rep('R',8194)); "
            "assert(string.dump(g,strip)==b) end; "
            "assert('21'+21==42 and '5'-2==3 and '6'*7==42 and '9'%4==1); "
            "assert('2'^3==8 and '9'/2==4.5 and '9'//2==4 and -'3'==-3); "
            "assert('0x20'+10==42 and '1.5'+2==3.5); "
            "local obj=setmetatable({}, {__add=function(a,b) assert(a=='x'); "
            "return b end}); assert('x'+obj==obj)");
    baseline(state);
  } else if (strstr(test->mode, "bindings") != NULL)
    execute(state, "local old=getmetatable(''); local funcs={}; for k,v in "
                   "pairs(old) do funcs[k]=v end; "
                   "local dump=string.dump; local other=reopen(); "
                   "assert(other.dump==dump); "
                   "for k,v in pairs(funcs) do if k~='__index' then "
                   "assert(getmetatable('')[k]==v) end end");
  else if (strstr(test->mode, "number_snapshot") != NULL)
    number_snapshot(test);
  else if (strstr(test->mode, "private_snapshot") != NULL)
    snapshot(test, 1, 1);
  else if (strstr(test->mode, "public_snapshot") != NULL)
    snapshot(test, 1, 0);
  else if (strstr(test->mode, "snapshot") != NULL)
    snapshot(test, 0, 0);
  else if (strstr(test->mode, "admission") != NULL)
    admission(test);
  else if (strstr(test->mode, "errors") != NULL)
    errors(test);
  else if (strstr(test->mode, "stack") != NULL)
    stack(test);
  else
    oom(test);
  require(state->native_roots == NULL && state->object_scopes == NULL);
  if (facade)
    luthrum_lua_runtime_free(test->runtime);
  else
    lua_close(state);
  require(test->live == 0);
  free(test->bytes);
  return NULL;
}
int main(int argc, char **argv) {
  struct fixture *test = calloc(1, sizeof(*test));
  pthread_t worker;
  require(test != NULL && argc == 2);
  active = test;
  test->mode = argv[1];
  if (strstr(test->mode, "stack") != NULL) {
    lua_State *state = luaL_newstate();
    const char *bytes;
#if defined(LUTHRUM_TEST_SMALL_NATIVE_STACK)
    pthread_attr_t attributes;
#endif
    require(state != NULL);
    luaL_openlibs(state);
    require(luaL_loadstring(state, "return string.dump(assert(load('return '.."
                                   "string.rep('function() return "
                                   "',40)..'42'..string.rep(' end',40)))())") ==
            LUA_OK);
    require(lua_pcall(state, 0, 1, 0) == LUA_OK);
    bytes = lua_tolstring(state, -1, &test->deep_size);
    require(bytes != NULL);
    test->deep = malloc(test->deep_size);
    require(test->deep != NULL);
    memcpy(test->deep, bytes, test->deep_size);
    lua_close(state);
#if defined(LUTHRUM_TEST_SMALL_NATIVE_STACK)
    require(pthread_attr_init(&attributes) == 0);
    require(pthread_attr_setstacksize(&attributes, 128u * 1024u) == 0);
    require(pthread_create(&worker, &attributes, run, test) == 0);
    require(pthread_attr_destroy(&attributes) == 0);
#else
    require(pthread_create(&worker, NULL, run, test) == 0);
#endif
  } else
    require(pthread_create(&worker, NULL, run, test) == 0);
  require(pthread_join(worker, NULL) == 0);
  free(test->deep);
  free(test);
  return 0;
}
