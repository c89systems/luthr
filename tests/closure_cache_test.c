#include <laccess.h>
#include <lapi.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lgc.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <sched.h>
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

struct allocator {
  int reject;
  unsigned rejected;
};

static __thread Proto *interrupt;
static __thread unsigned interrupted;

void __real_luaA_end(lua_State *, LuaObjectScope *);
void __wrap_luaA_end(lua_State *state, LuaObjectScope *scope) {
  int collect = interrupt != NULL && ttisLclosure(&scope->root.value) &&
                clLvalue(&scope->root.value)->p == interrupt;
  __real_luaA_end(state, scope);
  if (collect) {
    /* The constructor's native root still retains the complete closure.
     * Change cohorts before its weak cache publication. */
    interrupt = NULL;
    interrupted++;
    luaC_fullgc(state, 0);
    luaD_shrinkstack(state, state);
  }
}

static void *allocate(void *ud, void *pointer, size_t old, size_t size) {
  struct allocator *allocator = ud;
  if (size == 0) {
    free(pointer);
    return NULL;
  }
  if (allocator->reject && (pointer == NULL || size > old)) {
    allocator->rejected++;
    return NULL;
  }
  return realloc(pointer, size);
}

static void factory(lua_State *state) {
  require(luaL_dostring(state, "return function() "
                               "return function(value) return value+1 end "
                               "end") == LUA_OK);
}

static void make(lua_State *state) {
  lua_pushvalue(state, 1);
  require(lua_pcall(state, 0, 1, 0) == LUA_OK);
}

static void invoke(lua_State *state, lua_Integer value) {
  lua_pushinteger(state, value);
  require(lua_pcall(state, 1, 1, 0) == LUA_OK);
  require(lua_tointeger(state, -1) == value + 1);
  lua_pop(state, 1);
}

static size_t bytes(lua_State *state) {
  return (size_t)lua_gc(state, LUA_GCCOUNT) * 1024 +
         (size_t)lua_gc(state, LUA_GCCOUNTB);
}

static void reuse(lua_State *state, struct allocator *allocator) {
  size_t before, after;
  unsigned i;
  factory(state);
  lua_gc(state, LUA_GCSTOP);
  for (i = 0; i < 32; i++) {
    make(state);
    invoke(state, i);
  }
  before = bytes(state);
  allocator->reject = 1;
  for (i = 0; i < 20000; i++) {
    make(state);
    invoke(state, i);
  }
  allocator->reject = 0;
  after = bytes(state);
  require(allocator->rejected == 0 && after >= before && after - before < 1024);
}

static void weak(lua_State *state) {
  unsigned i;
  factory(state);
  lua_createtable(state, 1, 0);
  lua_createtable(state, 0, 1);
  lua_pushliteral(state, "v");
  lua_setfield(state, -2, "__mode");
  require(lua_setmetatable(state, 2));
  for (i = 0; i < 64; i++) {
    make(state);
    lua_pushvalue(state, -1);
    lua_rawseti(state, 2, 1);
    invoke(state, i);
    lua_gc(state, LUA_GCCOLLECT);
    lua_gc(state, LUA_GCCOLLECT);
    require(lua_rawgeti(state, 2, 1) == LUA_TNIL);
    lua_pop(state, 1);
    /* Reuse allocator storage before reading the invalidated weak cache. */
    require(luaL_dostring(state, "local t={}; for i=1,64 do "
                                 "t[i]={i,string.rep('x',i)} end") == LUA_OK);
    lua_gc(state, LUA_GCSTEP, (size_t)0);
  }
}

static void cycles(lua_State *state, int generational) {
  unsigned i, young = 0;
  factory(state);
  for (i = 0; i < 32; i++) {
    uint64_t epoch;
    unsigned steps = 0;
    make(state);
    invoke(state, i);
    epoch = G(state)->trace_epoch;
    do {
      lua_gc(state, LUA_GCSTEP, (size_t)0);
      require(++steps < 10000);
    } while (G(state)->trace_epoch == epoch || luaC_tracing(state) ||
             luaC_sweeping(state));
    if (G(state)->sweep_plan.kind == SWEEP_YOUNG)
      young++;
    /* The previous weak cache can now refer to a reclaimed young object. */
    make(state);
    invoke(state, i + 1);
    if (i % 8 == 7) {
      lua_gc(state, generational ? LUA_GCINC : LUA_GCGEN);
      lua_gc(state, generational ? LUA_GCGEN : LUA_GCINC);
    }
  }
  require(!generational || young != 0);
}

static void construction(lua_State *state) {
  const void *first;
  int capacity;
  factory(state);
  lua_gc(state, LUA_GCCOLLECT);
  lua_lock(state);
  /* Leave the enclosing frame's promised API capacity unchanged, so this
   * excess backing storage can actually shrink inside the constructor. */
  require(luaD_growstack(state, 8192, 1));
  capacity = stacksize(state);
  lua_unlock(state);
  interrupt = ((LClosure *)lua_topointer(state, 1))->p->p[0];
  make(state);
  require(interrupted == 1 && interrupt == NULL);
  require(stacksize(state) < capacity);
  first = lua_topointer(state, -1);
  /* No cohort has changed since publication. The second use must consume the
   * same completed object after collection and stack resizing. */
  make(state);
  require(lua_topointer(state, -1) == first);
  invoke(state, 41);
  invoke(state, 99);
}

struct worker {
  lua_State *state;
  unsigned *started, *finished;
  int status;
};

static void *run(void *ud) {
  struct worker *worker = ud;
  __atomic_fetch_add(worker->started, 1, __ATOMIC_RELEASE);
  worker->status = lua_pcall(worker->state, 1, 1, 0);
  if (worker->status != LUA_OK)
    fprintf(stderr, "parallel closure: %s\n", lua_tostring(worker->state, -1));
  require(worker->status == LUA_OK);
  require(lua_tointeger(worker->state, -1) == 50015000);
  __atomic_fetch_add(worker->finished, 1, __ATOMIC_RELEASE);
  return NULL;
}

static void parallel(lua_State *state) {
  struct worker workers[4];
  pthread_t threads[4];
  unsigned i, started = 0, finished = 0, collections = 0;
  factory(state);
  require(luaL_dostring(state, "return function(factory) local sum=0; "
                               "for i=1,10000 do local f=factory(); "
                               "sum=sum+f(i) end; return sum end") == LUA_OK);
  for (i = 0; i < 4; i++) {
    workers[i].state = lua_newthread(state);
    workers[i].started = &started;
    workers[i].finished = &finished;
    workers[i].status = -1;
    lua_pushvalue(state, 2);
    lua_pushvalue(state, 1);
    lua_xmove(state, workers[i].state, 2);
  }
  for (i = 0; i < 4; i++)
    require(pthread_create(&threads[i], NULL, run, &workers[i]) == 0);
  while (__atomic_load_n(&started, __ATOMIC_ACQUIRE) != 4)
    sched_yield();
  do {
    lua_gc(state, LUA_GCCOLLECT);
    collections++;
  } while (__atomic_load_n(&finished, __ATOMIC_ACQUIRE) != 4);
  for (i = 0; i < 4; i++)
    require(pthread_join(threads[i], NULL) == 0 && workers[i].status == LUA_OK);
  require(collections != 0);
}

int main(int argc, char **argv) {
  struct allocator allocator = {0};
  lua_State *state = lua_newstate(allocate, &allocator, 0);
  require(argc == 3 && state != NULL);
  luaL_openlibs(state);
  lua_gc(state, strcmp(argv[2], "generational") == 0 ? LUA_GCGEN : LUA_GCINC);
  if (strcmp(argv[1], "reuse") == 0)
    reuse(state, &allocator);
  else if (strcmp(argv[1], "weak") == 0)
    weak(state);
  else if (strcmp(argv[1], "construction") == 0)
    construction(state);
  else if (strcmp(argv[1], "cycles") == 0)
    cycles(state, strcmp(argv[2], "generational") == 0);
  else if (strcmp(argv[1], "parallel") == 0)
    parallel(state);
  else {
    require(strcmp(argv[1], "captures") == 0);
    require(luaL_dostring(state, "local function make(v) return function(d) "
                                 "v=v+d; return v end end; "
                                 "local a,b=make(40),make(100); "
                                 "assert(a(2)==42 and b(1)==101); "
                                 "debug.upvaluejoin(a,1,b,1); "
                                 "assert(a(1)==102 and b(1)==103)") == LUA_OK);
  }
  lua_close(state);
  return 0;
}
