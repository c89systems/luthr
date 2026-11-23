#include <laccess.h>
#include <lapi.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lfunc.h>
#include <lgc.h>
#include <lmem.h>
#include <lroots.h>
#include <lstackapi.h>
#include <lstate.h>
#include <ltable.h>
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

struct collection {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *main, *collector, *worker;
  size_t live;
  unsigned allocating, failures, fail, assisted, stops, sweep_stops;
  int entered, release, done, emergency, minor, help;
};
static struct collection *collecting;
static lua_State *weak_marker;
static GCObject *weak_table, *weak_key, *weak_target;
static unsigned weak_deferred_seen, weak_rescans, weak_target_scanned;
static unsigned weak_round_stops;
static unsigned weak_retries[TRACE_VERIFY + 1];
static int retry_weak;
static struct parallel_scan {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *workers[2], *finisher;
  int entered, progressed, release, finished, waiting;
} *parallel_scanning;
static __thread unsigned parallel_worker;

void __real_luaW_waitcollector(lua_State *, int);
void __wrap_luaW_waitcollector(lua_State *L, int reason) {
  struct parallel_scan *scan = parallel_scanning;
  if (scan != NULL && L == scan->finisher && reason == LUA_WAIT_TRACE)
    __atomic_store_n(&scan->waiting, 1, __ATOMIC_RELEASE);
  __real_luaW_waitcollector(L, reason);
}

static struct closure_builder {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *worker;
  lua_State *child;
  LClosure *closure;
  int entered, release, thread_build, fail;
} *building_closure;

static void pause_builder(struct closure_builder *builder) {
  /* The existing header is reachable before allocation of its payload. */
  require(pthread_mutex_lock(&builder->mutex) == 0);
  builder->entered = 1;
  require(pthread_cond_broadcast(&builder->changed) == 0);
  while (!builder->release)
    require(pthread_cond_wait(&builder->changed, &builder->mutex) == 0);
  require(pthread_mutex_unlock(&builder->mutex) == 0);
}

void *__real_luaM_malloc_(lua_State *, size_t, int);
void *__wrap_luaM_malloc_(lua_State *L, size_t size, int tag) {
  struct closure_builder *builder = building_closure;
  if (builder != NULL && L == builder->worker && !builder->thread_build &&
      size == sizeof(UpVal))
    pause_builder(builder);
  return __real_luaM_malloc_(L, size, tag);
}

void *__real_luaM_allocstack(lua_State *, size_t, int);
void *__wrap_luaM_allocstack(lua_State *L, size_t size, int emergency) {
  struct closure_builder *builder = building_closure;
  if (builder != NULL && L == builder->worker && builder->thread_build &&
      size == (BASIC_STACK_SIZE + EXTRA_STACK) * sizeof(StackValue)) {
    LuaWorldScope scope;
    if (builder->fail)
      return NULL;
    scope = luaW_detach(L);
    pause_builder(builder);
    luaW_attach(L, scope);
  }
  return __real_luaM_allocstack(L, size, emergency);
}

void __real_luaW_stop(lua_State *);
void __wrap_luaW_stop(lua_State *L) {
  if (collecting != NULL && L == collecting->collector && L->world_depth == 0) {
    require(pthread_mutex_lock(&G(L)->world.mutex) == 0);
    if (luaC_tracing(L))
      collecting->stops++;
    if (luaC_sweeping(L) && luaC_sweepready(G(L)))
      collecting->sweep_stops++;
    require(pthread_mutex_unlock(&G(L)->world.mutex) == 0);
  }
  if (L == weak_marker && luaC_tracephase(G(L)) == TRACE_MARK &&
      G(L)->trace_plan.again == NULL && G(L)->ephemeron != NULL)
    weak_round_stops++;
  __real_luaW_stop(L);
}

lu_mem __real_luaC_tracebatch(lua_State *, unsigned);
lu_mem __wrap_luaC_tracebatch(lua_State *L, unsigned limit) {
  lu_mem marked = __real_luaC_tracebatch(L, limit);
  if (collecting != NULL && L == collecting->worker && marked != 0 &&
      L->world_depth == 0) {
    collecting->assisted++;
  }
  return marked;
}

static void observescan(GCObject *object) {
  if (parallel_scanning != NULL && parallel_worker != 0 &&
      object->tt == LUA_VTABLE) {
    struct parallel_scan *scan = parallel_scanning;
    require(pthread_mutex_lock(&scan->mutex) == 0);
    if (parallel_worker == 1 && !scan->entered) {
      scan->entered = 1;
      require(pthread_cond_broadcast(&scan->changed) == 0);
      while (!scan->release)
        require(pthread_cond_wait(&scan->changed, &scan->mutex) == 0);
    } else if (parallel_worker == 2) {
      require(scan->entered && !scan->release);
      scan->progressed = 1;
      require(pthread_cond_broadcast(&scan->changed) == 0);
    }
    require(pthread_mutex_unlock(&scan->mutex) == 0);
  }
  if (weak_marker != NULL && luaC_tracing(weak_marker)) {
    if (object == weak_table) {
      if (iswhite(G(weak_marker), weak_key))
        weak_deferred_seen = 1;
      else if (!weak_target_scanned) {
        require(!luaW_stopped(&G(weak_marker)->world) &&
                !weak_marker->world_depth);
        weak_rescans++;
      }
    }
    if (object == weak_target) {
      require(!luaW_stopped(&G(weak_marker)->world) &&
              !weak_marker->world_depth);
      weak_target_scanned++;
    }
  }
}

int __real_luaA_tryscan(lua_State *, GCObject *);
int __wrap_luaA_tryscan(lua_State *state, GCObject *object) {
  int acquired;
  unsigned phase = luaC_tracephase(G(state));
  if (retry_weak && !luaW_iscollector(state) && object->tt == LUA_VTABLE &&
      (phase == TRACE_EPHEMERONS || phase == TRACE_VALUES ||
       phase == TRACE_CLEAR) &&
      weak_retries[phase] < 129) {
    weak_retries[phase]++;
    return 0;
  }
  acquired = __real_luaA_tryscan(state, object);
  if (acquired)
    observescan(object);
  return acquired;
}

int __real_luaC_scanstrong(lua_State *, LuaTraceWork *, Table *);
int __wrap_luaC_scanstrong(lua_State *state, LuaTraceWork *work, Table *table) {
  int result = __real_luaC_scanstrong(state, work, table);
  if (result == 1)
    observescan((GCObject *)table);
  return result;
}

static void *backing(void *context, void *pointer, size_t old, size_t size) {
  struct collection *collection = context;
  void *replacement;
  require(__atomic_fetch_add(&collection->allocating, 1, __ATOMIC_ACQUIRE) ==
          0);
  if (pointer == NULL)
    old = 0;
  require(collection->live >= old);
  if (size >= 1024 * 1024 &&
      __atomic_exchange_n(&collection->fail, 0, __ATOMIC_ACQ_REL)) {
    collection->failures++;
    replacement = NULL;
  } else if (size == 0) {
    collection->live -= old;
    free(pointer);
    replacement = NULL;
  } else {
    replacement = realloc(pointer, size);
    if (replacement != NULL)
      collection->live = collection->live - old + size;
  }
  require(__atomic_fetch_sub(&collection->allocating, 1, __ATOMIC_RELEASE) ==
          1);
  return replacement;
}

void __real_luaQ_poll(lua_State *);
void __wrap_luaQ_poll(lua_State *L) {
  struct collection *collection = collecting;
  if (collection != NULL && L == collection->collector && luaC_tracing(L) &&
      !collection->entered &&
      __atomic_load_n(&G(L)->trace_ready, __ATOMIC_ACQUIRE)) {
    LuaWorldScope scope = {0};
    require(L->world_depth == 0 && !luaW_stopped(&G(L)->world) &&
            !G(L)->trace_busy);
    /* These cases isolate graph publication and explicit helper handoffs.
     * Keep automatic allocation backpressure in gc_pacing's pressure cases. */
    __atomic_store_n(&G(L)->pacer.hard, MAX_LMEM, __ATOMIC_RELAXED);
    if (collection->emergency || collection->help)
      scope = luaW_detach(L); /* a real safepoint with no claimed graph work */
    require(pthread_mutex_lock(&collection->mutex) == 0);
    collection->entered = 1;
    require(pthread_cond_broadcast(&collection->changed) == 0);
    while (!collection->release)
      require(pthread_cond_wait(&collection->changed, &collection->mutex) == 0);
    require(pthread_mutex_unlock(&collection->mutex) == 0);
    if (collection->emergency || collection->help)
      luaW_attach(L, scope);
  }
  __real_luaQ_poll(L);
}

/* Start at a real published graph boundary. The normal collector driver will
 * own this phase transition; these units isolate the graph protocol from its
 * scheduling and pacing policy. */
static lua_State *newstate(void) {
  lua_State *L = luaL_newstate();
  require(L != NULL);
  lua_gc(L, LUA_GCINC);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCSTOP);
  return L;
}

static void begin(lua_State *L) {
  global_State *g = G(L);
  lua_lock(L);
  luaW_stop(L);
  require(g->gray == NULL && g->grayagain == NULL && !luaC_pagetracepending(g));
  luaC_runtilstate(L, GCSpropagate, 1);
  require(luaC_tracing(L));
  g->pacer.hard = MAX_LMEM; /* This fixture supplies its own phase driver. */
  luaW_start(L);
  lua_unlock(L);
}

static void drain(lua_State *L) {
  unsigned rounds;
  lua_lock(L);
  for (rounds = 0; rounds < 10000; rounds++) {
    luaC_tracebatch(L, 64);
    if (G(L)->gray == NULL && !luaC_pagetracepending(G(L)) &&
        G(L)->barrier_ready == 0)
      break;
  }
  require(rounds < 10000);
  lua_unlock(L);
}

static void finish(lua_State *L) {
  drain(L);
  lua_lock(L);
  require(G(L)->trace_deferred == NULL && !G(L)->trace_busy);
  lua_unlock(L);
  lua_gc(L, LUA_GCCOLLECT);
  lua_close(L);
}

static void page_frontier(void) {
  enum { ENTRIES = 4096 };
  lua_State *L = newstate();
  Table *root;
  unsigned i, rounds;
  lua_createtable(L, ENTRIES, 0);
  root = (Table *)lua_topointer(L, 1);
  for (i = 1; i <= ENTRIES; i++) {
    lua_createtable(L, 1, 0);
    lua_createtable(L, 0, i % 2 ? 1 : 3);
    lua_rawseti(L, -2, 1);
    lua_rawseti(L, 1, i);
  }
  begin(L);
  lua_lock(L);
  for (rounds = 0; rounds < 10000; rounds++) {
    luaC_tracebatch(L, 1 + rounds % 7);
    if (G(L)->gray == NULL && !luaC_pagetracepending(G(L)) &&
        G(L)->barrier_ready == 0)
      break;
  }
  require(rounds < 10000);
  /* Inspect the original graph before any API reads can shade a missed edge. */
  for (i = 0; i < ENTRIES; i++) {
    GCObject *child = getArrVal(root, i)->gc;
    GCObject *leaf = getArrVal(gco2t(child), 0)->gc;
    require(isblack(G(L), child) && isblack(G(L), leaf));
  }
  lua_unlock(L);
  finish(L);
}

static void read_before_weakening(int array) {
  lua_State *L = newstate();
  const void *child, *descendant;
  lua_newtable(L);
  lua_newtable(L);
  child = lua_topointer(L, -1);
  lua_newtable(L);
  descendant = lua_topointer(L, -1);
  lua_rawseti(L, -2, 1);
  if (array)
    lua_rawseti(L, 1, 1);
  else
    lua_setfield(L, 1, "child");
  require(luaL_loadstring(L, array ? "local t=...; return t[1]"
                                   : "local t=...; return t.child") == LUA_OK);

  begin(L);
  /* The marker has yet to freeze the mode or reach the child. */
  require(iswhite(G(L), (GCObject *)child));
  lua_pushvalue(L, 1);
  require(lua_pcall(L, 1, 1, 0) == LUA_OK);
  lua_lock(L);
  require(ttistable(s2v(L->top.p - 1)) && gcvalue(s2v(L->top.p - 1)) == child);
  lua_unlock(L);
  lua_newtable(L);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  require(lua_setmetatable(L, 1));
  drain(L);
  require(isblack(G(L), (GCObject *)child));
  require(isblack(G(L), (GCObject *)descendant));
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_rawgeti(L, 2, 1) == LUA_TTABLE);
  require(lua_topointer(L, -1) == descendant);

  lua_settop(L, 1);
  lua_gc(L, LUA_GCCOLLECT);
  if (array)
    require(lua_rawgeti(L, 1, 1) == LUA_TNIL);
  else
    require(lua_getfield(L, 1, "child") == LUA_TNIL);
  lua_close(L);
}

static int noresult(lua_State *L) {
  (void)L;
  return 0;
}

static void overwrite(const char *kind) {
  lua_State *L = newstate();
  GCObject *child, *descendant;
  int read_capture = strcmp(kind, "capture_read") == 0;
  require(L != NULL);
  /* Put the only child reference in a heap edge; retain its descendant only
   * through that child. No C pointer below is registered as a collector root.
   */
  lua_newtable(L);
  child = (GCObject *)lua_topointer(L, -1);
  if (read_capture) {
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "n");
  }
  lua_newtable(L);
  descendant = (GCObject *)lua_topointer(L, -1);
  lua_rawseti(L, -2, 1);
  if (strcmp(kind, "array") == 0 || strcmp(kind, "hash") == 0) {
    lua_newtable(L);
    lua_pushvalue(L, 1);
    if (strcmp(kind, "array") == 0)
      lua_rawseti(L, -2, 1);
    else
      lua_setfield(L, -2, "child");
  } else if (strcmp(kind, "userdata") == 0) {
    lua_newuserdatauv(L, 1, 1);
    lua_pushvalue(L, 1);
    require(lua_setiuservalue(L, -2, 1));
  } else if (strcmp(kind, "cclosure") == 0) {
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, noresult, 1);
  } else if (read_capture || strcmp(kind, "capture") == 0) {
    const char *source =
        read_capture ? "local value=...; return function() value.n=42; "
                       "return value, value.n end"
                     : "local value=...; return function() return value end";
    require(luaL_loadstring(L, source) == LUA_OK);
    lua_pushvalue(L, 1);
    require(lua_pcall(L, 1, 1, 0) == LUA_OK);
  } else {
    require(strcmp(kind, "metatable") == 0);
    lua_newtable(L);
    lua_pushvalue(L, 1);
    require(lua_setmetatable(L, -2));
  }
  lua_remove(L, 1);
  begin(L);
  require(iswhite(G(L), child) && iswhite(G(L), descendant));
  if (read_capture) {
    TValue *result;
    lua_pushvalue(L, 1);
    require(lua_pcall(L, 0, 2, 0) == LUA_OK && lua_tointeger(L, -1) == 42);
    lua_pop(L, 1);
    lua_lock(L);
    result = s2v(L->top.p - 1);
    require(ttistable(result) && gcvalue(result) == child);
    require(iswhite(G(L), child));
    lua_unlock(L);
  }
  lua_pushnil(L);
  if (strcmp(kind, "array") == 0)
    lua_rawseti(L, 1, 1);
  else if (strcmp(kind, "hash") == 0)
    lua_setfield(L, 1, "child");
  else if (strcmp(kind, "userdata") == 0)
    require(lua_setiuservalue(L, 1, 1));
  else if (read_capture || strcmp(kind, "cclosure") == 0 ||
           strcmp(kind, "capture") == 0)
    require(lua_setupvalue(L, 1, 1) != NULL);
  else
    require(lua_setmetatable(L, 1));
  require(!iswhite(G(L), child));
  drain(L);
  require(isblack(G(L), child) && isblack(G(L), descendant));
  if (read_capture) {
    lua_gc(L, LUA_GCCOLLECT);
    require(lua_rawgeti(L, -1, 1) == LUA_TTABLE);
    require(lua_topointer(L, -1) == descendant);
  }
  finish(L);
}

static void stack_snapshot(void) {
  lua_State *L = newstate();
  lua_State *worker = lua_newthread(L);
  GCObject *child;
  lua_newtable(worker);
  child = (GCObject *)lua_topointer(worker, -1);
  begin(L);
  require(iswhite(G(L), child));
  lua_settop(worker, 0); /* admission must snapshot before removing the root */
  require(worker->trace_snapshot == G(L)->trace_epoch && !iswhite(G(L), child));
  drain(L);
  require(isblack(G(L), child));
  finish(L);
}

enum {
  STACK_RELEASE,
  STACK_OFFER,
  STACK_SNAPSHOT,
  STACK_REOPEN,
  STACK_COLLECT,
  STACK_PARK
};

#define TEST_SLOT_OFFER ((uintptr_t)1 << LUA_GATE_OFFER_SHIFT)

struct stack_wait {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *thread;
  int stage, action;
};

static void *reserve_stack(void *context) {
  struct stack_wait *wait = context;
  LuaWorld *world = &G(wait->thread)->world;
  int reservation = luaX_tryreserve(wait->thread);
  require(reservation == 1);
  require(pthread_mutex_lock(&wait->mutex) == 0);
  wait->stage = 1;
  require(pthread_cond_broadcast(&wait->changed) == 0);
  while (wait->stage == 1)
    require(pthread_cond_wait(&wait->changed, &wait->mutex) == 0);
  if (wait->action == STACK_SNAPSHOT || wait->action == STACK_PARK) {
    if (wait->action == STACK_SNAPSHOT)
      luaC_snapshot(wait->thread, wait->thread);
    else {
      require(pthread_mutex_lock(&world->mutex) == 0);
      luaW_activate(wait->thread);
      luaW_deactivate(wait->thread);
      require(pthread_mutex_unlock(&world->mutex) == 0);
    }
    wait->stage = 3;
    require(pthread_cond_broadcast(&wait->changed) == 0);
    while (wait->stage == 3)
      require(pthread_cond_wait(&wait->changed, &wait->mutex) == 0);
    if (wait->action == STACK_PARK) {
      require(pthread_mutex_lock(&world->mutex) == 0);
      luaW_resume(wait->thread);
      require(pthread_mutex_unlock(&world->mutex) == 0);
    }
  }
  require(pthread_mutex_unlock(&wait->mutex) == 0);
  if (wait->action == STACK_OFFER) {
    /* Mirror a managed stack withdrawal that also publishes its slot. */
    luaX_releaseto(wait->thread, (uintptr_t)luaQ_ownertoken(), TEST_SLOT_OFFER);
  } else {
    luaX_endreserve(wait->thread, reservation);
  }
  return NULL;
}

static void wait_for_stack(int action) {
  struct stack_wait wait = {.mutex = PTHREAD_MUTEX_INITIALIZER,
                            .changed = PTHREAD_COND_INITIALIZER,
                            .action = action};
  lua_State *L = newstate();
  global_State *g = G(L);
  GCObject *child;
  pthread_t owner;
  unsigned i;
  wait.thread = lua_newthread(L);
  lua_newtable(wait.thread);
  child = (GCObject *)lua_topointer(wait.thread, -1);
  begin(L);
  require(pthread_create(&owner, NULL, reserve_stack, &wait) == 0);
  require(pthread_mutex_lock(&wait.mutex) == 0);
  while (wait.stage == 0)
    require(pthread_cond_wait(&wait.changed, &wait.mutex) == 0);
  lua_lock(L);
  for (i = 0; i < 64 && luaC_tracerunnable(g); i++)
    luaC_tracebatch(L, 64);
  require(!luaC_tracerunnable(g) && luaC_tracepending(g));
  require(g->trace_stacks == obj2gco(wait.thread) && iswhite(g, child));
  for (i = 0; i < 64; i++)
    require(luaC_tracebatch(L, 64) == 0);
  require(g->trace_stacks == obj2gco(wait.thread) && iswhite(g, child));
  if (action == STACK_REOPEN || action == STACK_COLLECT)
    luaW_stop(L);
  else
    lua_unlock(L);
  if (action == STACK_COLLECT) {
    require(luaC_tracebatch(L, 64) != 0);
    require(!luaC_waitingstacks(g) && !iswhite(g, child));
    luaW_start(L);
    lua_unlock(L);
  }
  wait.stage = 2;
  require(pthread_cond_broadcast(&wait.changed) == 0);
  if (action == STACK_SNAPSHOT || action == STACK_PARK) {
    while (wait.stage == 2)
      require(pthread_cond_wait(&wait.changed, &wait.mutex) == 0);
    require(!luaC_waitingstacks(g) && luaC_markready(g));
    drain(L);
    require(isblack(g, child) && !luaW_stopped(&g->world));
    wait.stage = 4;
    require(pthread_cond_broadcast(&wait.changed) == 0);
  }
  require(pthread_mutex_unlock(&wait.mutex) == 0);
  require(pthread_join(owner, NULL) == 0);
  if (action == STACK_REOPEN) {
    require(luaC_waitingstacks(g));
    luaW_start(L);
    lua_unlock(L);
  }
  require(!luaC_waitingstacks(g));
  drain(L);
  require(isblack(g, child));
  if (action == STACK_OFFER)
    require(__atomic_exchange_n(&wait.thread->stack_gate.word, 0,
                                __ATOMIC_SEQ_CST) == TEST_SLOT_OFFER);
  finish(L);
  require(pthread_cond_destroy(&wait.changed) == 0);
  require(pthread_mutex_destroy(&wait.mutex) == 0);
}

struct busy {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *worker;
  Table *table;
  int entered, release;
};

static void *hold(void *context) {
  struct busy *busy = context;
  LuaObjectScope scope;
  lua_lock(busy->worker);
  luaA_begin(busy->worker, obj2gco(busy->table), &scope);
  require(pthread_mutex_lock(&busy->mutex) == 0);
  busy->entered = 1;
  require(pthread_cond_broadcast(&busy->changed) == 0);
  while (!busy->release)
    require(pthread_cond_wait(&busy->changed, &busy->mutex) == 0);
  require(pthread_mutex_unlock(&busy->mutex) == 0);
  luaA_end(busy->worker, &scope);
  lua_unlock(busy->worker);
  return NULL;
}

static void busy_object(void) {
  lua_State *L = newstate();
  struct busy busy = {0};
  pthread_t thread;
  GCObject *independent;
  require(pthread_mutex_init(&busy.mutex, NULL) == 0);
  require(pthread_cond_init(&busy.changed, NULL) == 0);
  busy.worker = lua_newthread(L);
  lua_newtable(L);
  busy.table = (Table *)lua_topointer(L, -1);
  lua_newtable(L);
  independent = (GCObject *)lua_topointer(L, -1);
  begin(L);
  require(pthread_create(&thread, NULL, hold, &busy) == 0);
  require(pthread_mutex_lock(&busy.mutex) == 0);
  while (!busy.entered)
    require(pthread_cond_wait(&busy.changed, &busy.mutex) == 0);
  require(pthread_mutex_unlock(&busy.mutex) == 0);
  lua_lock(L);
  luaC_tracebatch(L, 256);
  require(!luaW_stopped(&G(L)->world) && L->world_depth == 0);
  require(isgray(busy.table) && isblack(G(L), independent));
  lua_unlock(L);
  require(pthread_mutex_lock(&busy.mutex) == 0);
  busy.release = 1;
  require(pthread_cond_broadcast(&busy.changed) == 0);
  require(pthread_mutex_unlock(&busy.mutex) == 0);
  require(pthread_join(thread, NULL) == 0);
  drain(L);
  require(isblack(G(L), busy.table));
  require(pthread_cond_destroy(&busy.changed) == 0);
  require(pthread_mutex_destroy(&busy.mutex) == 0);
  finish(L);
}

static void *initialize_closure(void *context) {
  struct closure_builder *builder = context;
  if (builder->thread_build)
    builder->child = lua_newthread(builder->worker);
  else {
    lua_lock(builder->worker);
    luaF_initupvals(builder->worker, builder->closure);
    lua_unlock(builder->worker);
  }
  return NULL;
}

static void closure_construction(int thread_build) {
  lua_State *L = newstate();
  struct closure_builder builder = {0};
  pthread_t thread;
  GCObject *independent;
  Proto *proto;
  require(pthread_mutex_init(&builder.mutex, NULL) == 0);
  require(pthread_cond_init(&builder.changed, NULL) == 0);
  builder.thread_build = thread_build;
  builder.worker = lua_newthread(L);
  if (!thread_build) {
    require(luaL_loadstring(builder.worker, "return _ENV") == LUA_OK);
    lua_lock(builder.worker);
    proto = clLvalue(s2v(builder.worker->top.p - 1))->p;
    builder.closure = luaF_newLclosure(builder.worker, 1);
    luaF_setproto(builder.worker, builder.closure, proto);
    setclLvalue2s(builder.worker, builder.worker->top.p - 1, builder.closure);
    lua_unlock(builder.worker);
  }
  lua_newtable(L);
  independent = (GCObject *)lua_topointer(L, -1);
  if (!thread_build)
    begin(L);
  building_closure = &builder;
  require(pthread_create(&thread, NULL, initialize_closure, &builder) == 0);
  require(pthread_mutex_lock(&builder.mutex) == 0);
  while (!builder.entered)
    require(pthread_cond_wait(&builder.changed, &builder.mutex) == 0);
  require(pthread_mutex_unlock(&builder.mutex) == 0);
  if (thread_build) {
    begin(L);
    lua_lock(builder.worker);
    builder.child = thvalue(s2v(builder.worker->top.p - 1));
    lua_unlock(builder.worker);
  }
  lua_lock(L);
  luaC_tracebatch(L, 256);
  require(!luaW_stopped(&G(L)->world) &&
          (thread_build ? isgray(obj2gco(builder.child))
                        : isblack(G(L), builder.closure)) &&
          isblack(G(L), independent));
  /* Atomic closure pointers can be marked even while capture allocation is
   * paused. The later inserted cell must still be marked before termination. */
  lua_unlock(L);
  require(pthread_mutex_lock(&builder.mutex) == 0);
  builder.release = 1;
  require(pthread_cond_broadcast(&builder.changed) == 0);
  require(pthread_mutex_unlock(&builder.mutex) == 0);
  require(pthread_join(thread, NULL) == 0);
  building_closure = NULL;
  drain(L);
  if (thread_build) {
    require(isblack(G(L), builder.child));
    require(luaL_loadstring(builder.child, "return 42") == LUA_OK);
    require(lua_pcall(builder.child, 0, 1, 0) == LUA_OK);
    require(lua_tointeger(builder.child, -1) == 42);
  } else {
    require(isblack(G(L), builder.closure) &&
            isblack(G(L), builder.closure->upvals[0]));
    lua_pushglobaltable(builder.worker);
    require(lua_setupvalue(builder.worker, 1, 1) != NULL);
    require(lua_pcall(builder.worker, 0, 1, 0) == LUA_OK);
    lua_pushglobaltable(builder.worker);
    require(lua_rawequal(builder.worker, -1, -2));
  }
  require(pthread_cond_destroy(&builder.changed) == 0);
  require(pthread_mutex_destroy(&builder.mutex) == 0);
  finish(L);
}

static int create_child(lua_State *L) {
  lua_newthread(L);
  return 1;
}

static void thread_construction_error(void) {
  lua_State *L = newstate();
  struct closure_builder builder = {0};
  builder.worker = L;
  builder.thread_build = builder.fail = 1;
  lua_pushcfunction(L, create_child);
  building_closure = &builder;
  require(lua_pcall(L, 0, 1, 0) == LUA_ERRMEM);
  building_closure = NULL;
  lua_settop(L, 0);
  lua_gc(L, LUA_GCCOLLECT);
  require(luaL_dostring(L, "return 42") == LUA_OK);
  require(lua_tointeger(L, -1) == 42);
  lua_close(L);
}

struct parser {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *worker;
  unsigned reads;
  int entered, release;
};

static const char *read_source(lua_State *L, void *context, size_t *size) {
  struct parser *parser = context;
  static const char prefix[] = "return function() local a=";
  static const char suffix[] = "{}; return a end";
  (void)L;
  if (parser->reads++ == 0) {
    *size = sizeof(prefix) - 1;
    return prefix;
  }
  if (parser->reads == 2) {
    require(pthread_mutex_lock(&parser->mutex) == 0);
    parser->entered = 1;
    require(pthread_cond_broadcast(&parser->changed) == 0);
    while (!parser->release)
      require(pthread_cond_wait(&parser->changed, &parser->mutex) == 0);
    require(pthread_mutex_unlock(&parser->mutex) == 0);
    *size = sizeof(suffix) - 1;
    return suffix;
  }
  *size = 0;
  return NULL;
}

static void *parse(void *context) {
  struct parser *parser = context;
  require(lua_load(parser->worker, read_source, parser, "trace-parser", "t") ==
          LUA_OK);
  return NULL;
}

static void parser_snapshot(void) {
  lua_State *L = newstate();
  struct parser parser = {0};
  pthread_t thread;
  Proto *proto;
  require(pthread_mutex_init(&parser.mutex, NULL) == 0);
  require(pthread_cond_init(&parser.changed, NULL) == 0);
  parser.worker = lua_newthread(L);
  require(pthread_create(&thread, NULL, parse, &parser) == 0);
  require(pthread_mutex_lock(&parser.mutex) == 0);
  while (!parser.entered)
    require(pthread_cond_wait(&parser.changed, &parser.mutex) == 0);
  require(pthread_mutex_unlock(&parser.mutex) == 0);
  require(parser.worker->building_protos != NULL);
  begin(L);
  drain(L);
  for (proto = parser.worker->building_protos; proto != NULL;
       proto = proto->build_next)
    require(proto->trace_snapshot == G(L)->trace_epoch && isblack(G(L), proto));
  require(pthread_mutex_lock(&parser.mutex) == 0);
  parser.release = 1;
  require(pthread_cond_broadcast(&parser.changed) == 0);
  require(pthread_mutex_unlock(&parser.mutex) == 0);
  require(pthread_join(thread, NULL) == 0);
  require(parser.worker->building_protos == NULL);
  proto = clLvalue(s2v(parser.worker->top.p - 1))->p;
  require(proto->builder == NULL && proto->p[0]->builder == NULL);
  require(lua_pcall(parser.worker, 0, 1, 0) == LUA_OK);
  require(lua_pcall(parser.worker, 0, 1, 0) == LUA_OK);
  require(lua_istable(parser.worker, -1));
  require(isblack(G(L), (GCObject *)lua_topointer(parser.worker, -1)));
  require(pthread_cond_destroy(&parser.changed) == 0);
  require(pthread_mutex_destroy(&parser.mutex) == 0);
  finish(L);
}

static void weak_cutoff(const char *kind) {
  lua_State *L = newstate();
  int keys = strcmp(kind, "weakkeys") == 0;
  int array = strcmp(kind, "weakarray") == 0;
  GCObject *child;
  lua_newtable(L);
  lua_newtable(L);
  child = (GCObject *)lua_topointer(L, -1);
  if (keys) {
    lua_pushinteger(L, 42);
    lua_rawset(L, 1);
  } else if (array)
    lua_rawseti(L, 1, 1);
  else
    lua_setfield(L, 1, "child");
  lua_newtable(L);
  lua_pushstring(L, keys ? "k" : "v");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, 1);
  begin(L);
  drain(L);
  require(iswhite(G(L), child));
  lua_lock(L);
  luaW_stop(L);
  __atomic_store_n(&G(L)->trace_weakcutoff, keys ? 3 : 1, __ATOMIC_RELEASE);
  luaW_start(L);
  lua_unlock(L);
  if (keys) {
    lua_pushnil(L);
    require(lua_next(L, 1) == 0);
  } else if (array) {
    require(lua_rawlen(L, 1) == 0);
    require(lua_rawgeti(L, 1, 1) == LUA_TNIL);
    lua_pop(L, 1);
  } else {
    require(lua_getfield(L, 1, "child") == LUA_TNIL);
    lua_pop(L, 1);
  }
  require(iswhite(G(L),
                  child)); /* resolving a dead weak edge must not promote it */
  /* A new entry after the cutoff remains valid. The first read must not
   * repeat an old clearing decision against new allocations. */
  lua_newtable(L);
  require(isblack(G(L), (GCObject *)lua_topointer(L, -1)));
  lua_setfield(L, 1, "new");
  require(lua_getfield(L, 1, "new") == LUA_TTABLE);
  lua_pop(L, 1);
  finish(L);
}

static void resurrected_weak(void) {
  lua_State *L = newstate();
  Table *inner;
  GCObject *child;
  /* A weak table which the initial strong closure cannot reach. Finalization
   * later publishes it as a root; its values belong to that later decision. */
  lua_newtable(L);
  inner = (Table *)lua_topointer(L, 1);
  lua_newtable(L);
  child = (GCObject *)lua_topointer(L, -1);
  lua_setfield(L, 1, "child");
  lua_newtable(L);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, 1);
  lua_pop(L, 1);
  begin(L);
  drain(L);
  require(iswhite(G(L), inner) && iswhite(G(L), child));
  lua_lock(L);
  luaW_stop(L);
  __atomic_store_n(&G(L)->trace_weakcutoff, 1, __ATOMIC_RELEASE);
  luaC_traceobject(L, obj2gco(inner));
  sethvalue2s(L, L->top.p++, inner);
  luaW_start(L);
  lua_unlock(L);
  drain(L);
  require(luaC_weakvalid(G(L), luaC_tablestate(inner)) && iswhite(G(L), child));
  /* A weak-value decision from before resurrection cannot erase this edge.
   * The read promotes it; completing the later cutoff must retain that root. */
  require(lua_getfield(L, 1, "child") == LUA_TTABLE);
  drain(L);
  require(isblack(G(L), child));
  lua_lock(L);
  luaW_stop(L);
  __atomic_store_n(&G(L)->trace_weakcutoff, 3, __ATOMIC_RELEASE);
  luaW_start(L);
  lua_unlock(L);
  require(lua_getfield(L, 1, "child") == LUA_TTABLE);
  finish(L);
}

static void weak_stamp_wrap(void) {
  lua_State *L = newstate();
  Table *table;
  lua_gc(L, LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_createtable(L, 1, 0);
  lua_newtable(L);
  lua_pushvalue(L, 2);
  lua_rawseti(L, 1, 1);
  lua_newtable(L);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, 1);
  lua_gc(L, LUA_GCCOLLECT);
  lua_pop(L, 1);
  table = (Table *)lua_topointer(L, 1);
  lua_lock(L);
  /* An old strong decision would otherwise look current after stamp wrap. */
  G(L)->weak_stamp = UINT64_MAX - (LUA_WEAK_STAMP_STEP - 1);
  luaC_settablestate(table, LUA_WEAK_STAMP_STEP);
  lua_unlock(L);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_rawgeti(L, 1, 1) == LUA_TNIL);
  lua_close(L);
}

static void string_lookup(int cache) {
  lua_State *L = newstate();
  static const char interned[] = "trace-interned-string";
  static const char cached[] =
      "trace-cached-string-long-enough-to-have-no-short-string-interning-entry";
  GCObject *string;
  if (cache)
    lua_pushstring(L, cached);
  else
    lua_pushlstring(L, interned, sizeof(interned) - 1);
  string = gcvalue(s2v(L->top.p - 1));
  lua_pop(L, 1);
  begin(L);
  require(iswhite(G(L), string));
  if (cache)
    lua_pushstring(L, cached);
  else
    lua_pushlstring(L, interned, sizeof(interned) - 1);
  require(gcvalue(s2v(L->top.p - 1)) == string && !iswhite(G(L), string));
  drain(L);
  require(isblack(G(L), string));
  finish(L);
}

static void *collect_graph(void *context) {
  struct collection *collection = context;
  if (collection->minor) {
    require(lua_gc(collection->collector, LUA_GCSTEP, (size_t)0) >= 0);
    /* A normal minor request is a slice too. This fixture needs a complete
     * job to check the overlapping helper/emergency termination handshake. */
    lua_lock(collection->collector);
    luaC_waittrace(collection->collector);
    lua_unlock(collection->collector);
  } else
    require(lua_gc(collection->collector, LUA_GCCOLLECT) == 0);
  require(!luaC_tracing(collection->collector) &&
          !G(collection->collector)->trace_busy);
  require(pthread_mutex_lock(&collection->mutex) == 0);
  collection->done = 1;
  require(pthread_cond_broadcast(&collection->changed) == 0);
  require(pthread_mutex_unlock(&collection->mutex) == 0);
  return NULL;
}

static void concurrent_collection(const char *kind) {
  struct collection collection = {0};
  pthread_t collector;
  unsigned length = 0;
  uint64_t interrupted;
  int generational = strstr(kind, "generational") != NULL ||
                     strstr(kind, "minor") != NULL ||
                     strcmp(kind, "help_to_incremental") == 0;
  int to_generation = strcmp(kind, "help_to_generational") == 0;
  int to_incremental = strcmp(kind, "help_to_incremental") == 0;
  int check_generations;
  if (to_generation)
    generational = 0;
  require(pthread_mutex_init(&collection.mutex, NULL) == 0);
  require(pthread_cond_init(&collection.changed, NULL) == 0);
  collection.emergency = strncmp(kind, "emergency", 9) == 0;
  check_generations = (generational || to_generation) && !to_incremental &&
                      !collection.emergency;
  collection.help = strncmp(kind, "help", 4) == 0;
  collection.minor = strstr(kind, "minor") != NULL;
  collection.main = lua_newstate(backing, &collection, 0);
  require(collection.main != NULL);
  lua_gc(collection.main, LUA_GCSTOP);
  lua_gc(collection.main, LUA_GCINC);
  /* The 1 MiB allocation after emergency recovery should fit the next idle
   * allowance. This fixture observes the interrupted driver's completion. */
  if (collection.emergency && !generational)
    lua_gc(collection.main, LUA_GCPARAM, LUA_GCPPAUSE, 400);
  /* Leave an otherwise unreachable old dirty table on the remembered list.
   * Emergency marking may free it while the original controller is parked. */
  lua_newtable(collection.main);
  lua_newtable(collection.main);
  lua_setglobal(collection.main, "retained");
  if (generational) {
    lua_gc(collection.main, LUA_GCGEN);
    lua_gc(collection.main, LUA_GCPARAM, LUA_GCPMINORMAJOR, 0);
  } else
    lua_gc(collection.main, LUA_GCCOLLECT);
  lua_newtable(collection.main);
  lua_rawseti(collection.main, -2, 1);
  lua_pop(collection.main, 1);
  require(luaL_loadstring(
              collection.main,
              "local head; for i=1,10000 do head={head} end; return head") ==
          LUA_OK);
  require(lua_pcall(collection.main, 0, 1, 0) == LUA_OK);
  collection.worker = lua_newthread(collection.main);
  collection.collector = lua_newthread(collection.main);
  lua_pushvalue(collection.main, 1);
  lua_xmove(collection.main, collection.collector, 1);
  lua_gc(collection.main, LUA_GCRESTART); /* Workers may assist this job. */
  interrupted = G(collection.main)->trace_interrupt;
  collecting = &collection;
  require(pthread_create(&collector, NULL, collect_graph, &collection) == 0);
  require(pthread_mutex_lock(&collection.mutex) == 0);
  while (!collection.entered)
    require(pthread_cond_wait(&collection.changed, &collection.mutex) == 0);
  require(!collection.done);
  require(pthread_mutex_unlock(&collection.mutex) == 0);
  /* Actual Lua execution, object construction and compilation must finish
   * while a normal collection still has graph work pending. */
  require(luaL_loadstring(collection.worker,
                          "local n=0; for i=1,2000 do local t={i,i+1}; "
                          "n=n+t[2] end; return n") == LUA_OK);
  require(lua_pcall(collection.worker, 0, 1, 0) == LUA_OK);
  require(lua_tointeger(collection.worker, -1) == 2003000);
  require(collection.assisted != 0);
  if (check_generations) {
    /* Retain a graph allocated while marking, including objects born old in
     * a generational major. Locals must not mask missing promotion/barriers. */
    require(luaL_loadstring(collection.worker, "retained[1]={{42}}") == LUA_OK);
    require(lua_pcall(collection.worker, 0, 0, 0) == LUA_OK);
  }
  if (collection.emergency) {
    __atomic_store_n(&collection.fail, 1, __ATOMIC_RELEASE);
    require(lua_newuserdatauv(collection.worker, 1024 * 1024, 0) != NULL);
    require(collection.failures == 1 &&
            G(collection.main)->trace_interrupt != interrupted);
  }
  if (collection.help) {
    unsigned assisted = collection.assisted;
    /* The host has parked the original driver. Collection on another stack
     * must complete before that driver resumes, without aborting and globally
     * restarting the trace. The helper must perform real concurrent marking. */
    require(lua_gc(collection.worker, to_generation    ? LUA_GCGEN
                                      : to_incremental ? LUA_GCINC
                                                       : LUA_GCCOLLECT) >= 0);
    require(!luaC_tracing(collection.worker));
    require(G(collection.main)->trace_interrupt == interrupted);
    require(collection.assisted > assisted);
  }
  require(pthread_mutex_lock(&collection.mutex) == 0);
  require(!collection.done);
  collection.release = 1;
  require(pthread_cond_broadcast(&collection.changed) == 0);
  require(pthread_mutex_unlock(&collection.mutex) == 0);
  require(pthread_join(collector, NULL) == 0);
  collecting = NULL;
  require(collection.done && !collection.collector->native_depth);
  /* Ten thousand retained objects require over 150 mark batches. Global
   * synchronization must follow phases, rather than stopping at every batch. */
  require(collection.stops < 64);
  if (!collection.emergency && !collection.minor && !collection.help)
    require(collection.sweep_stops == 0);
  if (to_generation)
    require(G(collection.main)->gckind == KGC_GENMINOR);
  if (to_incremental)
    require(G(collection.main)->gckind == KGC_INC);
  if (check_generations) {
    unsigned cycle;
    require(luaL_loadstring(collection.worker, "retained[1][1].later={{99}}") ==
            LUA_OK);
    require(lua_pcall(collection.worker, 0, 0, 0) == LUA_OK);
    for (cycle = 0; cycle < 8; cycle++) {
      unsigned steps = 0;
      do {
        require(lua_gc(collection.main, LUA_GCSTEP, (size_t)0) >= 0);
        require(++steps < 10000);
      } while (luaC_tracing(collection.main) || luaC_sweeping(collection.main));
    }
    require(G(collection.main)->gckind == KGC_GENMINOR);
    require(luaL_loadstring(collection.worker,
                            "return retained[1][1][1], "
                            "retained[1][1].later[1][1]") == LUA_OK);
    require(lua_pcall(collection.worker, 0, 2, 0) == LUA_OK);
    require(lua_tointeger(collection.worker, -2) == 42 &&
            lua_tointeger(collection.worker, -1) == 99);
  }
  lua_pushvalue(collection.main, 1);
  while (lua_istable(collection.main, -1)) {
    lua_rawgeti(collection.main, -1, 1);
    lua_remove(collection.main, -2);
    length++;
  }
  require(length == 10000);
  lua_close(collection.main);
  require(collection.live == 0 && !collection.allocating);
  require(pthread_cond_destroy(&collection.changed) == 0);
  require(pthread_mutex_destroy(&collection.mutex) == 0);
}

static void ephemerons(void) {
  lua_State *L = newstate();
  lua_newtable(L); /* second ephemeron, visited before its key becomes live */
  weak_table = (GCObject *)lua_topointer(L, 1);
  lua_newtable(L); /* first ephemeron */
  lua_newtable(L); /* first key, independently rooted */
  lua_newtable(L); /* second key, reachable only through first's value */
  weak_key = (GCObject *)lua_topointer(L, 4);
  lua_newtable(L); /* conditional value: must be traced concurrently */
  weak_target = (GCObject *)lua_topointer(L, 5);
  lua_pushvalue(L, 4);
  lua_pushvalue(L, 5);
  lua_rawset(L, 1);
  lua_newtable(L); /* first value holds the second key */
  lua_pushvalue(L, 4);
  lua_rawseti(L, 6, 1);
  lua_pushvalue(L, 3);
  lua_pushvalue(L, 6);
  lua_rawset(L, 2);
  lua_pop(L, 3);
  lua_newtable(L);
  lua_pushliteral(L, "k");
  lua_setfield(L, -2, "__mode");
  lua_pushvalue(L, -1);
  lua_setmetatable(L, 1);
  lua_setmetatable(L, 2);
  weak_marker = L;
  require(lua_gc(L, LUA_GCCOLLECT) == 0);
  require(weak_deferred_seen && weak_rescans && weak_target_scanned);
  require(weak_round_stops == 0);
  lua_pushvalue(L, 3);
  require(lua_rawget(L, 2) == LUA_TTABLE);
  require(lua_rawgeti(L, -1, 1) == LUA_TTABLE);
  require(lua_rawget(L, 1) == LUA_TTABLE);
  require(lua_topointer(L, -1) == weak_target);
  weak_marker = NULL;
  weak_table = weak_key = weak_target = NULL;
  lua_close(L);
}

static void *parallel_scanner(void *context) {
  struct parallel_scan *scan = parallel_scanning;
  unsigned worker = *(unsigned *)context;
  lua_State *L = scan->workers[worker - 1];
  unsigned rounds;
  parallel_worker = worker;
  lua_lock(L);
  for (rounds = 0; rounds < 10000; rounds++) {
    int done;
    luaC_tracebatch(L, 1);
    require(pthread_mutex_lock(&scan->mutex) == 0);
    done = worker == 1 ? scan->release : scan->progressed;
    require(pthread_mutex_unlock(&scan->mutex) == 0);
    if (done)
      break;
  }
  require(rounds < 10000);
  lua_unlock(L);
  parallel_worker = 0;
  return NULL;
}

static void weak_retry(void) {
  lua_State *L = newstate();
  luaL_openlibs(L);
  require(luaL_loadstring(
              L,
              "finalized=0; roots={}; "
              "local v={__mode='v',__gc=function() finalized=finalized+1 end}; "
              "local k={__mode='k',__gc=v.__gc}; for i=1,128 do "
              "roots[i]=setmetatable({{i}},v); "
              "roots[i+128]=setmetatable({[{}]={i}},k) end") == LUA_OK);
  require(lua_pcall(L, 0, 0, 0) == LUA_OK);
  /* More than one batch must fail ownership in each weak phase. Allocation,
   * weak-list and finalizer membership coexist throughout those retries. */
  retry_weak = 1;
  lua_gc(L, LUA_GCCOLLECT);
  retry_weak = 0;
  require(weak_retries[TRACE_EPHEMERONS] == 129 &&
          weak_retries[TRACE_VALUES] == 129 &&
          weak_retries[TRACE_CLEAR] == 129);
  require(
      luaL_loadstring(L, "assert(finalized==0); for i=1,256 do "
                         "assert(next(roots[i])==nil) end; roots=nil; "
                         "collectgarbage('collect'); assert(finalized==256)") ==
      LUA_OK);
  require(lua_pcall(L, 0, 0, 0) == LUA_OK);
  lua_close(L);
}

static void *parallel_finisher(void *context) {
  struct parallel_scan *scan = context;
  require(lua_gc(scan->finisher, LUA_GCCOLLECT) == 0);
  require(pthread_mutex_lock(&scan->mutex) == 0);
  scan->finished = 1;
  require(pthread_cond_broadcast(&scan->changed) == 0);
  require(pthread_mutex_unlock(&scan->mutex) == 0);
  return NULL;
}

static void parallel_marking(const char *kind) {
  struct parallel_scan scan = {0};
  lua_State *L = newstate();
  pthread_t workers[2], finisher;
  unsigned identities[2] = {1, 2};
  unsigned phase = strcmp(kind, "parallel_ephemerons") == 0   ? TRACE_EPHEMERONS
                   : strcmp(kind, "parallel_weakvalues") == 0 ? TRACE_VALUES
                                                              : TRACE_MARK;
  unsigned rounds;
  l_mem marked;
  require(pthread_mutex_init(&scan.mutex, NULL) == 0);
  require(pthread_cond_init(&scan.changed, NULL) == 0);
  scan.workers[0] = lua_newthread(L);
  scan.workers[1] = lua_newthread(L);
  scan.finisher = lua_newthread(L);
  if (phase == TRACE_EPHEMERONS)
    require(luaL_loadstring(L, "local root,keys={},{}; "
                               "for i=1,257 do keys[i]={i} end; "
                               "for i=1,256 do root[i]=setmetatable("
                               "{[keys[i]]={keys[i+1],42}}, {__mode='k'}) "
                               "end; return root,keys[1]") == LUA_OK);
  else if (phase == TRACE_VALUES)
    require(luaL_loadstring(L, "local root={}; for i=1,256 do "
                               "root[i]=setmetatable({{i,42}}, {__mode='v'}) "
                               "end; return root") == LUA_OK);
  else
    require(luaL_loadstring(L, "local root,shared={},{}; for i=1,256 do "
                               "root[i]={i,{i+1,42},shared}; shared[i]=root[i] "
                               "end; return root") == LUA_OK);
  luaL_openlibs(L);
  require(lua_pcall(L, 0, LUA_MULTRET, 0) == LUA_OK);
  begin(L);
  lua_lock(L);
  memset(&G(L)->trace_plan, 0, sizeof(G(L)->trace_plan));
  G(L)->trace_plan.phase = TRACE_INIT;
  G(L)->trace_plan.finish = TRACE_MAJOR;
  luaC_advancetrace(L);
  for (rounds = 0; phase != TRACE_MARK && rounds < 10000; rounds++) {
    if (G(L)->trace_plan.phase == phase && G(L)->trace_plan.next != NULL)
      break;
    luaC_tracebatch(L, 64);
    if (!__atomic_load_n(&G(L)->trace_ready, __ATOMIC_ACQUIRE))
      luaC_advancetrace(L);
  }
  require(rounds < 10000 && luaC_tracing(L));
  lua_unlock(L);
  parallel_scanning = &scan;
  require(pthread_create(&workers[0], NULL, parallel_scanner, &identities[0]) ==
          0);
  require(pthread_mutex_lock(&scan.mutex) == 0);
  while (!scan.entered)
    require(pthread_cond_wait(&scan.changed, &scan.mutex) == 0);
  require(pthread_mutex_unlock(&scan.mutex) == 0);
  require(pthread_mutex_lock(&G(L)->world.mutex) == 0);
  marked = G(L)->GCmarked;
  require(G(L)->trace_busy == 1 && !luaW_stopped(&G(L)->world));
  require(pthread_mutex_unlock(&G(L)->world.mutex) == 0);
  require(pthread_create(&workers[1], NULL, parallel_scanner, &identities[1]) ==
          0);
  require(pthread_join(workers[1], NULL) == 0);
  require(pthread_mutex_lock(&scan.mutex) == 0);
  require(scan.progressed && !scan.release);
  require(pthread_mutex_unlock(&scan.mutex) == 0);
  require(pthread_mutex_lock(&G(L)->world.mutex) == 0);
  require(G(L)->trace_busy == 1 && G(L)->trace_plan.phase == phase &&
          !luaW_stopped(&G(L)->world));
  if (phase == TRACE_MARK)
    require(G(L)->GCmarked > marked);
  require(pthread_mutex_unlock(&G(L)->world.mutex) == 0);
  require(pthread_create(&finisher, NULL, parallel_finisher, &scan) == 0);
  while (!__atomic_load_n(&scan.waiting, __ATOMIC_ACQUIRE))
    sched_yield();
  /* An empty shared queue cannot finish a phase while the first scanner's
   * claimed payloads and newly discovered edges are still private. */
  require(pthread_mutex_lock(&scan.mutex) == 0);
  require(!scan.finished && luaC_tracing(L) && !luaW_stopped(&G(L)->world));
  scan.release = 1;
  require(pthread_cond_broadcast(&scan.changed) == 0);
  require(pthread_mutex_unlock(&scan.mutex) == 0);
  require(pthread_join(workers[0], NULL) == 0);
  require(pthread_join(finisher, NULL) == 0);
  require(scan.finished);
  parallel_scanning = NULL;
  lua_lock(L);
  luaC_waittrace(L);
  require(!luaC_tracing(L) && !G(L)->trace_busy);
  lua_unlock(L);
  lua_gc(L, LUA_GCCOLLECT);
  /* Check the graph after a subsequent complete collection, including the
   * conditional edges discovered by concurrently scanned ephemerons. */
  if (phase == TRACE_EPHEMERONS) {
    for (rounds = 1; rounds <= 256; rounds++) {
      lua_rawgeti(L, 4, rounds);
      lua_pushvalue(L, 5);
      require(lua_rawget(L, -2) == LUA_TTABLE);
      require(lua_rawgeti(L, -1, 2) == LUA_TNUMBER);
      require(lua_tointeger(L, -1) == 42);
      lua_pop(L, 1);
      require(lua_rawgeti(L, -1, 1) == LUA_TTABLE);
      lua_replace(L, 5);
      lua_settop(L, 5);
    }
  } else {
    for (rounds = 1; rounds <= 256; rounds++) {
      require(lua_rawgeti(L, 4, rounds) == LUA_TTABLE);
      require(lua_rawgeti(L, -1, 1) ==
              (phase == TRACE_VALUES ? LUA_TNIL : LUA_TNUMBER));
      if (phase == TRACE_MARK)
        require(lua_tointeger(L, -1) == (lua_Integer)rounds);
      lua_settop(L, 4);
    }
  }
  lua_close(L);
  require(pthread_cond_destroy(&scan.changed) == 0);
  require(pthread_mutex_destroy(&scan.mutex) == 0);
}

int main(int argc, char **argv) {
  require(argc == 2);
  if (strcmp(argv[1], "stack") == 0)
    stack_snapshot();
  else if (strcmp(argv[1], "stack_wait_snapshot") == 0)
    wait_for_stack(STACK_SNAPSHOT);
  else if (strcmp(argv[1], "stack_wait_release") == 0)
    wait_for_stack(STACK_RELEASE);
  else if (strcmp(argv[1], "stack_wait_offer") == 0)
    wait_for_stack(STACK_OFFER);
  else if (strcmp(argv[1], "stack_wait_reopen") == 0)
    wait_for_stack(STACK_REOPEN);
  else if (strcmp(argv[1], "stack_wait_collect") == 0)
    wait_for_stack(STACK_COLLECT);
  else if (strcmp(argv[1], "stack_wait_park") == 0)
    wait_for_stack(STACK_PARK);
  else if (strcmp(argv[1], "busy") == 0)
    busy_object();
  else if (strcmp(argv[1], "parser") == 0)
    parser_snapshot();
  else if (strcmp(argv[1], "closure_builder") == 0 ||
           strcmp(argv[1], "thread_builder") == 0)
    closure_construction(strcmp(argv[1], "thread_builder") == 0);
  else if (strcmp(argv[1], "thread_builder_error") == 0)
    thread_construction_error();
  else if (strcmp(argv[1], "resurrected_weak") == 0)
    resurrected_weak();
  else if (strcmp(argv[1], "intern") == 0 || strcmp(argv[1], "cache") == 0)
    string_lookup(strcmp(argv[1], "cache") == 0);
  else if (strncmp(argv[1], "concurrent", 10) == 0 ||
           strncmp(argv[1], "emergency", 9) == 0 ||
           strncmp(argv[1], "help", 4) == 0)
    concurrent_collection(argv[1]);
  else if (strcmp(argv[1], "ephemerons") == 0)
    ephemerons();
  else if (strcmp(argv[1], "weaken_array_after_read") == 0 ||
           strcmp(argv[1], "weaken_hash_after_read") == 0)
    read_before_weakening(strcmp(argv[1], "weaken_array_after_read") == 0);
  else if (strcmp(argv[1], "weak_retry") == 0)
    weak_retry();
  else if (strcmp(argv[1], "weak_stamp_wrap") == 0)
    weak_stamp_wrap();
  else if (strcmp(argv[1], "page_frontier") == 0)
    page_frontier();
  else if (strncmp(argv[1], "parallel_", 9) == 0)
    parallel_marking(argv[1]);
  else if (strncmp(argv[1], "weak", 4) == 0)
    weak_cutoff(argv[1]);
  else
    overwrite(argv[1]);
  return 0;
}
