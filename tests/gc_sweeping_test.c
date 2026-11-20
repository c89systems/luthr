#include <lapi.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lfunc.h>
#include <lgc.h>
#include <lmem.h>
#include <lroots.h>
#include <lstate.h>
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

enum { OBJECTS = 4096 };
static _Thread_local size_t sweep_limit;
static struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *collector, *worker, *finisher;
  const void *revive;
  unsigned kind, helpers, scanned, freed;
  unsigned boundaries;
  int entered, release, emergency, finished, unwind, unwound, boundary;
} *fixture;

void __real_luaW_restore(lua_State *, LuaWorldScope, unsigned);
void __wrap_luaW_restore(lua_State *state, LuaWorldScope scope,
                         unsigned native) {
  struct fixture *test = fixture;
  if (test != NULL && test->boundaries != 0 && state == test->collector &&
      scope.collection != 0 && state->world_depth == 0 &&
      !luaC_tracing(state) && !luaC_sweeping(state)) {
    test->boundaries--;
    /* Reproduce another execution starting a cycle before the suspended
     * driver reacquires its collector scope. Native depth retains its roots. */
    lua_unlock(state);
    lua_lock(test->worker);
    luaE_setdebt(G(state), 0);
    luaC_step(test->worker);
    require(luaC_tracing(state) && !luaC_sweeping(state));
    lua_unlock(test->worker);
    lua_lock(state);
  }
  __real_luaW_restore(state, scope, native);
}

void __real_luaQ_poll(lua_State *);
void __wrap_luaQ_poll(lua_State *state) {
  struct fixture *test = fixture;
  if (test != NULL && state == test->collector && test->unwind &&
      test->entered && test->release && !test->unwound &&
      luaC_sweeping(state)) {
    LuaGCFrame *frame = luaC_gcframe();
    require(frame != NULL && frame->retired == NULL &&
            frame->retired_bytes == 0);
    test->unwound = 1;
    luaD_throw(state, LUA_ERRRUN);
  }
  __real_luaQ_poll(state);
}

void __real_luaC_scansweep(lua_State *, LuaSweepWork *);
void __wrap_luaC_scansweep(lua_State *state, LuaSweepWork *work) {
  struct fixture *test = fixture;
  GCObject *object;
  int candidate = work->page != NULL && test != NULL && test->revive == NULL;
  if (work->page != NULL) {
    size_t stride = sizeof(LuaAllocBlock) + luaMP_binbytes(work->page->bin);
    size_t cells = (work->page->bytes - LUA_ALLOC_PAGE_HEADER) / stride;
    require(work->page->bytes <= LUA_ALLOC_PAGE_MAX);
    if (cells > sweep_limit)
      sweep_limit = cells;
  }
  for (object = work->next; object != NULL;
       object = work->phase == SWEEP_GRAY ? *luaC_graylink(object)
                                          : luaC_allocnext(object))
    candidate |= test != NULL && test->revive != NULL
                     ? object == test->revive
                     : object->tt == LUA_VTABLE;
  if (test != NULL && !test->boundary && state == test->collector &&
      work->kind == test->kind && work->phase == SWEEP_OBJECTS && candidate) {
    require(pthread_mutex_lock(&test->mutex) == 0);
    if (!test->entered) {
      require(state->world_depth == 0 && !luaW_stopped(&G(state)->world));
      test->entered = 1;
      require(pthread_cond_broadcast(&test->changed) == 0);
      while (!test->release)
        require(pthread_cond_wait(&test->changed, &test->mutex) == 0);
    }
    require(pthread_mutex_unlock(&test->mutex) == 0);
  }
  __real_luaC_scansweep(state, work);
  if (test != NULL && state == test->worker)
    test->scanned += work->count;
}

void __real_luaC_reclaim(lua_State *);
void __wrap_luaC_reclaim(lua_State *state) {
  struct fixture *test = fixture;
  if (test != NULL && state == test->collector && test->emergency) {
    int finished;
    require(pthread_mutex_lock(&test->mutex) == 0);
    finished = test->finished;
    require(pthread_mutex_unlock(&test->mutex) == 0);
    if (!finished)
      return; /* The emergency must drain another owner's queue. */
  }
  __real_luaC_reclaim(state);
}

lu_mem __real_luaC_sweepbatch(lua_State *, unsigned, int);
lu_mem __wrap_luaC_sweepbatch(lua_State *state, unsigned limit, int complete) {
  struct fixture *test = fixture;
  lu_mem worked;
  if (test != NULL && state == test->worker && test->entered &&
      !test->release && test->helpers >= 8)
    return 0; /* Keep the cohort pending while its first scanner is parked. */
  sweep_limit = limit < LUA_TRACE_BATCH ? limit : LUA_TRACE_BATCH;
  worked = __real_luaC_sweepbatch(state, limit, complete);
  require(worked <= sweep_limit * sizeof(GCObject));
  if (test != NULL && state == test->worker && worked != 0)
    test->helpers++;
  return worked;
}

static void *release_string(void *context, void *pointer, size_t old,
                            size_t size) {
  struct fixture *test = context;
  require(old == 65 && size == 0);
  __atomic_fetch_add(&test->freed, 1, __ATOMIC_RELAXED);
  free(pointer);
  return NULL;
}

static void minor(lua_State *state) {
  unsigned steps = 0;
  do {
    lua_gc(state, LUA_GCSTEP, (size_t)0);
    require(++steps < 10000);
  } while (luaC_tracing(state) || luaC_sweeping(state));
}

static void *collect(void *argument) {
  struct fixture *test = argument;
  if (test->unwind) {
    require(luaL_loadstring(test->collector, "collectgarbage('collect')") ==
            LUA_OK);
    require(lua_pcall(test->collector, 0, 0, 0) == LUA_ERRRUN);
    lua_lock(test->collector);
    require(luaC_gcframe() == NULL && test->collector->native_depth == 0 &&
            test->collector->world_depth == 0);
    lua_unlock(test->collector);
  } else if (test->kind == SWEEP_YOUNG)
    minor(test->collector);
  else
    lua_gc(test->collector,
           test->kind == SWEEP_OLD ? LUA_GCGEN : LUA_GCCOLLECT);
  return NULL;
}

static void *emergency(void *argument) {
  struct fixture *test = argument;
  lua_lock(test->finisher);
  luaC_fullgc(test->finisher, 1);
  require(__atomic_load_n(&test->freed, __ATOMIC_RELAXED) == OBJECTS);
  lua_unlock(test->finisher);
  require(pthread_mutex_lock(&test->mutex) == 0);
  test->finished = 1;
  require(pthread_cond_broadcast(&test->changed) == 0);
  require(pthread_mutex_unlock(&test->mutex) == 0);
  return NULL;
}

static int store_userdata(lua_State *state) {
  lua_getglobal(state, "ud");
  lua_pushvalue(state, 1);
  require(lua_setiuservalue(state, -2, 1));
  return 0;
}

static int store_native(lua_State *state) {
  if (lua_gettop(state) == 0) {
    lua_pushvalue(state, lua_upvalueindex(1));
    return 1;
  }
  lua_copy(state, 1, lua_upvalueindex(1));
  return 0;
}

static const char updates[] =
    "round=round+1; anchor.child={round*10+1}; "
    "store_capture({round*10+2}); store_ud({round*10+3}); "
    "store_native({round*10+4}); "
    "for i=1,20000 do progress.value=i end";

static void execute(lua_State *state, const char *script) {
  require(luaL_loadstring(state, script) == LUA_OK);
  if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
    fprintf(stderr, "%s\n", lua_tostring(state, -1));
    abort();
  }
}

static int boundary(lua_State *state) {
  struct fixture test = {0};
  test.boundary = 1;
  luaL_openlibs(state);
  lua_gc(state, LUA_GCGEN);
  lua_gc(state, LUA_GCSTOP);
  lua_gc(state, LUA_GCPARAM, LUA_GCPMINORMAJOR, 0);
  lua_gc(state, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  execute(state, "rows={}; for i=1,4096 do rows[i]={i} end");
  test.collector = state;
  test.worker = lua_newthread(state);
  lua_gc(state, LUA_GCRESTART);
  lua_lock(state);
  luaE_setdebt(G(state), 0);
  luaC_step(state);
  require(luaC_tracing(state));
  lua_unlock(state);
  test.boundaries = 2;
  fixture = &test;
  lua_gc(state, LUA_GCCOLLECT);
  require(test.boundaries == 0);
  fixture = NULL;
  execute(state, "for i=1,4096 do assert(rows[i][1]==i) end");
  lua_close(state);
  return 0;
}

static int empty_bands(lua_State *state) {
  unsigned i;
  luaL_openlibs(state);
  lua_gc(state, LUA_GCGEN);
  lua_gc(state, LUA_GCSTOP);
  lua_gc(state, LUA_GCPARAM, LUA_GCPMINORMAJOR, 0);
  execute(state, "anchor={17}");
  for (i = 0; i < 8; i++) {
    minor(state);
    lua_lock(state);
    require(G(state)->old1 != NULL && G(state)->survival != NULL);
    if (i == 0)
      require(G(state)->sweep_plan.tail[1] == &G(state)->sweep_plan.live[1]);
    if (i == 1) {
      require(G(state)->sweep_plan.tail[0] == &G(state)->sweep_plan.live[0]);
      require(G(state)->survival == G(state)->old1);
    }
    lua_unlock(state);
  }
  execute(state, "assert(anchor[1]==17)");
  lua_close(state);
  return 0;
}

static int promoted_builder(lua_State *state) {
  struct fixture test = {0};
  LuaNativeRoot root;
  Proto *proto;
  unsigned i;
  char *text = malloc(65);
  require(text != NULL);
  memset(text, 'x', 64);
  text[64] = '\0';
  luaL_openlibs(state);
  lua_gc(state, LUA_GCGEN);
  lua_gc(state, LUA_GCSTOP);
  lua_gc(state, LUA_GCPARAM, LUA_GCPMINORMAJOR, 0);
  lua_pushexternalstring(state, text, 64, release_string, &test);
  lua_lock(state);
  proto = luaF_newproto(state);
  proto->k = luaM_newvector(state, 1, TValue);
  proto->sizek = 1;
  setobj(state, &proto->k[0], s2v(state->top.p - 1));
  /* A surviving builder can promote before supplying a size snapshot.
   * Zero-sized accounting must not suppress its subsequent scans. */
  require(proto->trace_bytes == 0);
  setmarked(proto, luaC_newmark(G(state)) | G_SURVIVAL);
  setmarked(gcvalue(&proto->k[0]), luaC_newmark(G(state)));
  state->building_protos = proto->build_next;
  proto->build_next = NULL;
  proto->builder = NULL;
  luaR_pushroot(state, &root);
  setgcovalue(state, &root.value, obj2gco(proto));
  luaW_stop(state);
  luaC_beginsweep(state, SWEEP_YOUNG, 0);
  luaW_start(state);
  luaC_waitsweep(state);
  lua_unlock(state);
  lua_pop(state, 1); /* Only the promoted prototype retains the string now. */
  for (i = 0; i < 4; i++) {
    minor(state);
    require(test.freed == 0);
  }
  lua_lock(state);
  luaR_poproot(state, &root);
  lua_unlock(state);
  lua_close(state);
  require(test.freed == 1);
  return 0;
}

int main(int argc, char **argv) {
  struct fixture test = {0};
  lua_State *state = luaL_newstate();
  pthread_t collector, finisher;
  unsigned i;
  int generational, white, threads;
  require(argc == 2 && state != NULL);
  if (strcmp(argv[1], "boundary") == 0)
    return boundary(state);
  if (strcmp(argv[1], "empty_bands") == 0)
    return empty_bands(state);
  if (strcmp(argv[1], "promoted_builder") == 0)
    return promoted_builder(state);
  test.kind = strstr(argv[1], "minor") != NULL        ? SWEEP_YOUNG
              : strstr(argv[1], "conversion") != NULL ? SWEEP_OLD
                                                      : SWEEP_MAJOR;
  test.emergency = strstr(argv[1], "emergency") != NULL;
  test.unwind = strcmp(argv[1], "unwind") == 0;
  white = strstr(argv[1], "white") != NULL;
  threads = strstr(argv[1], "threads") != NULL;
  generational = test.kind == SWEEP_YOUNG;
  require(pthread_mutex_init(&test.mutex, NULL) == 0);
  require(pthread_cond_init(&test.changed, NULL) == 0);
  luaL_openlibs(state);
  lua_gc(state, LUA_GCSTOP);
  lua_newuserdatauv(state, 0, 1);
  lua_setglobal(state, "ud");
  lua_pushcfunction(state, store_userdata);
  lua_setglobal(state, "store_ud");
  lua_pushnil(state);
  lua_pushcclosure(state, store_native, 1);
  lua_setglobal(state, "store_native");
  execute(state, "anchor={}; progress={value=0}; round=0; local captured; "
                 "function store_capture(v) captured=v end; "
                 "function check_capture() return captured end");
  lua_gc(state, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(state, LUA_GCCOLLECT);
  lua_gc(state, LUA_GCPARAM, LUA_GCPMINORMAJOR, 0);
  lua_gc(state, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  execute(state, "rows={}; for i=1,4096 do rows[i]={i}; local dead={i} end");
  if (strstr(argv[1], "resurrect") != NULL) {
    lua_pushliteral(state, "sweep resurrection sentinel");
    test.revive = lua_topointer(state, -1);
    minor(state);
    require(getage((GCObject *)test.revive) == G_SURVIVAL);
    lua_pop(state, 1);
  }
  for (i = 0; i < OBJECTS; i++) {
    char *text = malloc(65);
    require(text != NULL);
    memset(text, 'x', 64);
    text[64] = '\0';
    lua_pushexternalstring(state, text, 64, release_string, &test);
    lua_pop(state, 1);
  }
  if (threads)
    execute(state,
            "captures={}; for i=1,128 do local co=coroutine.create("
            "function() local x={i}; captures[i]=function() return x end; "
            "coroutine.yield() end); assert(coroutine.resume(co)) end");
  test.collector = lua_newthread(state);
  test.worker = lua_newthread(state);
  test.finisher = lua_newthread(state);
  require(luaL_loadstring(test.worker, updates) == LUA_OK);
  lua_gc(state, LUA_GCRESTART);
  lua_lock(state);
  G(state)->pacer.goal = MAX_LMEM;
  luaE_setdebt(G(state), MAX_LMEM);
  lua_unlock(state);
  fixture = &test;
  require(pthread_create(&collector, NULL, collect, &test) == 0);
  require(pthread_mutex_lock(&test.mutex) == 0);
  while (!test.entered)
    require(pthread_cond_wait(&test.changed, &test.mutex) == 0);
  require(pthread_mutex_unlock(&test.mutex) == 0);
  if (test.revive != NULL) {
    require(isdead(G(state), (GCObject *)test.revive));
    lua_pushliteral(state, "sweep resurrection sentinel");
    require(lua_topointer(state, -1) == test.revive);
    lua_setglobal(state, "revived");
  }
  if (white) {
    GCObject *owner;
    lua_getglobal(state, "anchor");
    owner = (GCObject *)lua_topointer(state, -1);
    require(isold(owner));
    setmarked(owner, (getmarked(owner) & ~(1 << MINORBIT)) |
                         (G(state)->minor_mark ^ (1 << MINORBIT)));
    lua_pop(state, 1);
  }
  require(lua_pcall(test.worker, 0, 0, 0) == LUA_OK);
  if (test.revive == NULL)
    require(test.helpers != 0 && test.scanned != 0);
  require(pthread_mutex_lock(&G(state)->world.mutex) == 0);
  require(G(state)->sweep_plan.busy == 1 && !luaW_stopped(&G(state)->world));
  require(pthread_mutex_unlock(&G(state)->world.mutex) == 0);
  if (test.emergency) {
    require(pthread_create(&finisher, NULL, emergency, &test) == 0);
    while (!luaW_stopped(&G(state)->world))
      sched_yield();
  }
  require(pthread_mutex_lock(&test.mutex) == 0);
  require(!test.finished);
  test.release = 1;
  require(pthread_cond_broadcast(&test.changed) == 0);
  require(pthread_mutex_unlock(&test.mutex) == 0);
  require(pthread_join(collector, NULL) == 0);
  if (test.emergency) {
    require(pthread_join(finisher, NULL) == 0);
    require(__atomic_load_n(&test.freed, __ATOMIC_RELAXED) == OBJECTS);
  }
  if (test.unwind) {
    require(test.unwound && luaC_sweeping(state));
    lua_lock(test.worker);
    luaC_waitsweep(test.worker);
    lua_unlock(test.worker);
  }
  fixture = NULL;
  lua_lock(test.worker);
  luaC_reclaim(test.worker);
  lua_unlock(test.worker);
  lua_lock(test.collector);
  luaC_reclaim(test.collector);
  lua_unlock(test.collector);
  for (i = 0; i < 4; i++) {
    if (generational || test.kind == SWEEP_OLD)
      minor(state);
    else
      lua_gc(state, LUA_GCCOLLECT);
    execute(state, "assert(anchor.child[1]==round*10+1); "
                   "assert(check_capture()[1]==round*10+2); "
                   "assert(store_native()[1]==round*10+4)");
    require(lua_getglobal(state, "ud") == LUA_TUSERDATA);
    require(lua_getiuservalue(state, -1, 1) == LUA_TTABLE);
    lua_rawgeti(state, -1, 1);
    require(lua_tointeger(state, -1) == (lua_Integer)((i + 1) * 10 + 3));
    lua_pop(state, 3);
    if (i != 3)
      execute(state, updates);
  }
  require(__atomic_load_n(&test.freed, __ATOMIC_RELAXED) == OBJECTS);
  execute(state, "for i=1,4096 do assert(rows[i][1]==i) end; "
                 "if captures then for i=1,128 do assert(captures[i]()[1]==i) "
                 "end end; assert(progress.value==20000)");
  if (test.revive != NULL) {
    require(lua_getglobal(state, "revived") == LUA_TSTRING);
    require(lua_topointer(state, -1) == test.revive);
    require(strcmp(lua_tostring(state, -1), "sweep resurrection sentinel") ==
            0);
    lua_pop(state, 1);
  }
  lua_close(state);
  require(pthread_cond_destroy(&test.changed) == 0);
  require(pthread_mutex_destroy(&test.mutex) == 0);
  return 0;
}
