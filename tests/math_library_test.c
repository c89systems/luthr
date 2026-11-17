#include <laccess.h>
#include <lauxlib.h>
#include <lualib.h>
#include <luthr/lua_runtime.h>
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
enum { DRAWS = 256 };
struct actor {
  lua_State *state;
  lua_Integer *values;
  unsigned count, limit;
  int status, results, done;
};
struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_barrier_t start;
  lua_State *main;
  luthr_lua_runtime *runtime;
  struct actor actors[2];
  lua_Integer values[2][DRAWS], expected[2 * DRAWS];
  const char *mode;
  const void *old_state;
  size_t live;
  unsigned failures, finalized;
  int deny, armed, entered, release;
};
static struct fixture *active;
static __thread struct actor *current;
void __real_luaA_beginpayload(lua_State *, Udata *, LuaObjectScope *);
void __wrap_luaA_beginpayload(lua_State *state, Udata *object,
                              LuaObjectScope *scope) {
  __real_luaA_beginpayload(state, object, scope);
  if (state == active->actors[0].state && active->armed) {
    active->armed = 0;
    require(state->native_depth != 0);
    luaX_unlockstate(state); /* Keep the payload claim and its explicit root. */
    require(pthread_mutex_lock(&active->mutex) == 0);
    active->entered = 1;
    require(pthread_cond_broadcast(&active->changed) == 0);
    while (!active->release)
      require(pthread_cond_wait(&active->changed, &active->mutex) == 0);
    require(pthread_mutex_unlock(&active->mutex) == 0);
    luaX_lockstate(state);
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
  if (test->deny && size > old_size) {
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
static void host_free(void *context, void *memory, size_t size) {
  (void)allocate(context, memory, size, 0);
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
    fprintf(stderr, "math library: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
}
static int emit(lua_State *state) {
  require(current != NULL && current->count < current->limit);
  require(lua_isinteger(state, 1));
  current->values[current->count++] = lua_tointeger(state, 1);
  return 0;
}
static int finalize(lua_State *state) {
  require(lua_touserdata(state, 1) == active->old_state);
  active->finalized++;
  return 0;
}
static int bad_userdata(lua_State *state) {
  lua_Integer size = lua_isuserdata(state, 1)
                         ? (lua_Integer)lua_rawlen(state, 1)
                         : luaL_checkinteger(state, 1);
  void *memory;
  require(size >= 0 && size <= 256);
  memory = lua_newuserdatauv(state, (size_t)size, 0);
  memset(memory, 0, (size_t)size);
  return 1;
}
static int new_math(lua_State *state) { return luaopen_math(state); }
static int strip_capture(lua_State *state) {
  lua_CFunction function = lua_tocfunction(state, 1);
  require(function != NULL);
  lua_pushcfunction(state, function);
  return 1;
}
static void *draw(void *context) {
  struct actor *actor = context;
  int status = pthread_barrier_wait(&active->start);
  require(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
  current = actor;
  actor->status = lua_resume(actor->state, NULL, 0, &actor->results);
  current = NULL;
  require(pthread_mutex_lock(&G(actor->state)->world.mutex) == 0);
  actor->done = 1;
  require(pthread_cond_broadcast(&G(actor->state)->world.changed) == 0);
  require(pthread_mutex_unlock(&G(actor->state)->world.mutex) == 0);
  return NULL;
}
static int compare(const void *left, const void *right) {
  lua_Integer a = *(const lua_Integer *)left, b = *(const lua_Integer *)right;
  return (a > b) - (a < b);
}
static const char golden[] =
    "gold={0xf5cf0742e04157af,0x373b7bbc6b7d870d,0x1477a412c0afc3a8,"
    "0x5e18d153765bfbc7,0xfb17a2b44519e8cd,0x59ede20b1f9bbc7e,"
    "0xd40cececeb593a93,0xf25630750c0d9f07,0x0012ceb74199bba6,"
    "0xcf06a4c0bbf1a2a0,0x94a61833292392a8,0x3bc82c7dbb5fde6b}";
static void *run(void *context) {
  struct fixture *test = context;
  int facade = strstr(test->mode, "facade") != NULL;
  lua_State *state;
  if (facade) {
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
    require(luthr_lua_runtime_set_instruction_limit(test->runtime, 2000000) ==
            LUTHR_LUA_RUNTIME_OK);
    require(luthr_lua_runtime_run_buffer(test->runtime, load, sizeof(load) - 1,
                                         "math-library", 0, NULL,
                                         0) == LUTHR_LUA_RUNTIME_OK);
  } else {
    test->main = lua_newstate(allocate, test, 0);
    require(test->main != NULL);
    luaL_openlibs(test->main);
  }
  state = test->main;
  lua_pushcfunction(state, emit);
  lua_setglobal(state, "emit");
  lua_pushcfunction(state, new_math);
  lua_setglobal(state, "new_math");
  lua_pushcfunction(state, bad_userdata);
  lua_setglobal(state, "bad_userdata");
  lua_pushcfunction(state, strip_capture);
  lua_setglobal(state, "strip_capture");
  lua_pushcfunction(state, finalize);
  lua_setglobal(state, "finalize");
  if (strstr(test->mode, "generational") != NULL)
    (void)lua_gc(state, LUA_GCGEN);
  execute(state, golden);
  if (strstr(test->mode, "sequence") != NULL) {
    execute(
        state,
        "local a,b=math.randomseed(11,22); assert(a==11 and b==22); "
        "for i=1,#gold do assert(math.random(0)==gold[i]) end; "
        "math.randomseed(11,22); assert(math.random()==0.96019025213923226); "
        "assert(math.random(10)==9 and math.random(-10,17)==-3); "
        "assert(math.random(0)==-353635150588155699 and "
        "math.random('1','7')==7); "
        "math.randomseed(11); local n=math.random(0); "
        "math.randomseed(11,nil,99); "
        "assert(math.random(0)==n); a,b=math.randomseed(); "
        "assert(math.type(a)=='integer' and math.type(b)=='integer'); "
        "math.randomseed(a,b); n=math.random(0); math.randomseed(a,b); "
        "assert(math.random(0)==n); "
        "math.randomseed(11,22); "
        "assert(math.random(math.mininteger,math.maxinteger)== "
        "(gold[1] ~ math.mininteger))");
  } else if (strstr(test->mode, "errors") != NULL) {
    execute(state,
            "local bad={function() math.random('bad') end, "
            "function() math.random(nil) end,function() math.random(1.5) end, "
            "function() math.random(1,{}) end,function() math.random(2,1) end, "
            "function() math.random(1,2,3) end}; "
            "for _,f in ipairs(bad) do math.randomseed(11,22); "
            "local ok,err=pcall(f); assert(not ok and type(err)=='string'); "
            "assert(math.random(0)==gold[2]) end; "
            "math.randomseed(11,22); local ok,n=xpcall(bad[1],function() "
            "collectgarbage('collect'); return math.random(0) end); "
            "assert(not ok and n==gold[2]); math.randomseed(11,22); "
            "assert(not pcall(math.randomseed,'bad')); "
            "assert(math.random(0)==gold[1])");
  } else if (strstr(test->mode, "bindings") != NULL) {
    execute(
        state,
        "local other=new_math(); assert(other.abs==math.abs and "
        "other.min==math.min); "
        "assert(other.random~=math.random); other.randomseed(11,22); "
        "assert(not pcall(strip_capture(math.random))); "
        "assert(not pcall(strip_capture(math.randomseed),11,22)); "
        "math.randomseed(11,22); assert(other.random(0)==math.random(0)); "
        "local _,saved=debug.getupvalue(math.random,1); "
        "for _,bad in ipairs({false,17,'bad',{},bad_userdata(0), "
        "bad_userdata(saved)}) do debug.setupvalue(math.random,1,bad); "
        "local ok,err=pcall(math.random); assert(not ok and "
        "err:find('invalid random generator state',1,true)); end; "
        "debug.setupvalue(math.random,1,nil); assert(not pcall(math.random)); "
        "debug.setupvalue(math.random,1,saved); math.randomseed(11,22); "
        "assert(math.random(0)==gold[1]); "
        "debug.setupvalue(math.randomseed,1,{}); "
        "assert(not pcall(math.randomseed,11)); "
        "debug.setupvalue(math.randomseed,1,saved); math.randomseed(11,22); "
        "assert(math.random(0)==gold[1])");
  } else if (strstr(test->mode, "oom") != NULL) {
    execute(state,
            "math.randomseed(11,22); math.random(0); math.randomseed(11,22)");
    require(luaL_loadstring(state, "return math.random(0)") == LUA_OK);
    test->deny = 1;
    require(lua_pcall(state, 0, 1, 0) == LUA_OK);
    test->deny = 0;
    require(test->failures == 0 &&
            lua_tointeger(state, -1) == (lua_Integer)-734360230426486865LL);
    lua_pop(state, 1);
    lua_getglobal(state, "math");
    lua_getfield(state, -1, "random");
    lua_remove(state, -2);
    /* A long public type name requires backing storage to format the error;
     * a short diagnostic can be satisfied by already allocated pages. */
    (void)lua_newuserdatauv(state, 1, 0);
    lua_newtable(state);
    {
      char *name = malloc(8192);
      require(name != NULL);
      memset(name, 'x', 8192);
      lua_pushlstring(state, name, 8192);
      free(name);
    }
    lua_setfield(state, -2, "__name");
    lua_setmetatable(state, -2);
    (void)lua_gc(state, LUA_GCSTOP); /* Keep the already warmed call frames. */
    test->deny = 1;
    require(lua_pcall(state, 1, 1, 0) == LUA_ERRMEM);
    test->deny = 0;
    require(test->failures != 0);
    lua_pop(state, 1);
    (void)lua_gc(state, LUA_GCRESTART);
    execute(state, "assert(math.random(0)==gold[3])");
  } else {
    pthread_t actors[2];
    struct actor reference = {0};
    unsigned count = 2;
    int snapshot = strstr(test->mode, "capture") != NULL;
    int id, reseed = strstr(test->mode, "reseed") != NULL;
    const char *body =
        strstr(test->mode, "bounded") != NULL
            ? "local count=...; for i=1,count do "
              "emit(math.random(0,0x4000000000000000)) end"
            : "local count=...; for i=1,count do emit(math.random(0)) end";
    execute(state, "math.randomseed(11,22)");
    if (!snapshot && !reseed) {
      reference.values = test->expected;
      reference.limit = 2 * DRAWS;
      current = &reference;
      require(luaL_loadstring(state, body) == LUA_OK);
      lua_pushinteger(state, 2 * DRAWS);
      require(lua_pcall(state, 1, 0, 0) == LUA_OK);
      current = NULL;
      require(reference.count == 2 * DRAWS);
      execute(state, "math.randomseed(11,22)");
    }
    if (snapshot) {
      execute(state, "local _,s=debug.getupvalue(math.random,1); "
                     "debug.setmetatable(s,{__gc=finalize})");
      lua_getglobal(state, "math");
      lua_getfield(state, -1, "random");
      require(lua_getupvalue(state, -1, 1) != NULL);
      test->old_state = lua_touserdata(state, -1);
      lua_pop(state, 3);
    }
    for (id = 0; id < (int)count; ++id) {
      struct actor *actor = &test->actors[id];
      actor->state = lua_newthread(state);
      actor->values = test->values[id];
      actor->limit = DRAWS;
      if (snapshot)
        require(luaL_loadstring(actor->state, "return math.random(0)") ==
                LUA_OK);
      else {
        require(luaL_loadstring(
                    actor->state,
                    reseed && id == 1
                        ? "for i=1,256 do local a,b=math.randomseed(i,i+1); "
                          "assert(a==i and b==i+1); emit(math.random(0)) end"
                    : strstr(test->mode, "bounded") != NULL
                        ? "for i=1,256 do "
                          "emit(math.random(0,0x4000000000000000)) end"
                        : "for i=1,256 do emit(math.random(0)) end") == LUA_OK);
      }
    }
    test->armed = snapshot;
    require(pthread_barrier_init(&test->start, NULL, snapshot ? 1 : count) ==
            0);
    for (id = 0; id < (snapshot ? 1 : (int)count); ++id)
      require(pthread_create(&actors[id], NULL, draw, &test->actors[id]) == 0);
    if (snapshot) {
      LuaWorld *world = &G(state)->world;
      require(pthread_mutex_lock(&test->mutex) == 0);
      while (!test->entered)
        require(pthread_cond_wait(&test->changed, &test->mutex) == 0);
      require(pthread_mutex_unlock(&test->mutex) == 0);
      require(pthread_create(&actors[1], NULL, draw, &test->actors[1]) == 0);
      require(pthread_mutex_lock(&world->mutex) == 0);
      while (test->actors[1].state->object_waiting == 0 &&
             !test->actors[1].done)
        luaQ_wait(state, &world->mutex, &world->changed, &world->waiters);
      require(!test->actors[1].done &&
              test->actors[1].state->object_waiting != 0);
      require(pthread_mutex_unlock(&world->mutex) == 0);
      execute(
          state,
          "debug.setupvalue(math.random,1,nil); "
          "debug.setupvalue(math.randomseed,1,nil); collectgarbage('collect')");
      require(test->finalized == 0);
      require(pthread_mutex_lock(&test->mutex) == 0);
      test->release = 1;
      require(pthread_cond_broadcast(&test->changed) == 0);
      require(pthread_mutex_unlock(&test->mutex) == 0);
    }
    for (id = 0; id < (int)count; ++id) {
      require(pthread_join(actors[id], NULL) == 0);
      require(test->actors[id].status == LUA_OK);
      require(test->actors[id].count == (snapshot ? 0 : DRAWS));
    }
    require(pthread_barrier_destroy(&test->start) == 0);
    if (snapshot) {
      require(test->actors[0].results == 1 &&
              lua_tointeger(test->actors[0].state, -1) ==
                  (lua_Integer)-734360230426486865LL);
      require(test->actors[1].results == 1 &&
              lua_tointeger(test->actors[1].state, -1) ==
                  (lua_Integer)0x373b7bbc6b7d870dULL);
      lua_settop(test->actors[0].state, 0);
      lua_settop(test->actors[1].state, 0);
      (void)lua_gc(state, LUA_GCCOLLECT);
      (void)lua_gc(state, LUA_GCCOLLECT);
      require(test->finalized == 1);
    } else if (!reseed) {
      qsort(test->expected, 2 * DRAWS, sizeof(lua_Integer), compare);
      qsort(test->values, 2 * DRAWS, sizeof(lua_Integer), compare);
      require(memcmp(test->values, test->expected, sizeof(test->expected)) ==
              0);
    }
  }
  require(state->object_scopes == NULL && state->native_roots == NULL);
  if (facade)
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
  require(pthread_mutex_init(&test->mutex, NULL) == 0);
  require(pthread_cond_init(&test->changed, NULL) == 0);
  require(pthread_create(&worker, NULL, run, test) == 0);
  require(pthread_join(worker, NULL) == 0);
  require(pthread_cond_destroy(&test->changed) == 0);
  require(pthread_mutex_destroy(&test->mutex) == 0);
  free(test);
  return 0;
}
