#include <lapi.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lgc.h>
#include <lstate.h>
#include <lualib.h>
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
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *main, *collector, *worker;
  void *target;
  size_t live;
  unsigned allocator_active;
  int entered, release, detached, freed, done;
  int handshake, waiting, observed;
  int assist, assisted, trace, trace_assisted, unwind, interrupt;
};
static struct fixture *active;

void __real_luaQ_poll(lua_State *);
void __wrap_luaQ_poll(lua_State *L) {
  if (active != NULL && active->interrupt && L == active->worker &&
      L->reclaiming) {
    active->interrupt = 0;
    luaD_throw(L, LUA_ERRRUN);
  }
  __real_luaQ_poll(L);
}

static void interrupted_reclaim(lua_State *L, void *unused) {
  (void)unused;
  luaC_assistreclaim(L);
  require(0);
}

void __real_luaQ_wait(lua_State *, pthread_mutex_t *, pthread_cond_t *,
                      LuaWaiter **);
void __wrap_luaQ_wait(lua_State *L, pthread_mutex_t *mutex,
                      pthread_cond_t *changed, LuaWaiter **waiters) {
  struct fixture *fixture = active;
  if (fixture != NULL && fixture->handshake && L == fixture->worker &&
      mutex == &G(L)->world.mutex && G(L)->world.stopped &&
      luaQ_isowner(&G(L)->world.collector)) {
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    fixture->waiting = 1;
    require(pthread_cond_broadcast(&fixture->changed) == 0);
    require(pthread_mutex_unlock(&fixture->mutex) == 0);
  }
  __real_luaQ_wait(L, mutex, changed, waiters);
}

void __real_luaC_publish(lua_State *);
void __wrap_luaC_publish(lua_State *L) {
  struct fixture *fixture = active;
  if (fixture != NULL && fixture->handshake && L == fixture->worker &&
      !fixture->observed) {
    /* A real collector reached quiescence before the other execution finished
     * its private destruction loop, without test-induced native detachment. */
    require(fixture->collector->reclaiming &&
            fixture->collector->retired != NULL);
    fixture->observed = 1;
  }
  __real_luaC_publish(L);
}

static void *allocate(void *context, void *pointer, size_t old, size_t size) {
  struct fixture *fixture = context;
  void *replacement;
  require(__atomic_fetch_add(&fixture->allocator_active, 1, __ATOMIC_ACQUIRE) ==
          0);
  if (pointer == NULL)
    old = 0;
  require(fixture->live >= old);
  if (size == 0) {
    fixture->live -= old;
    free(pointer);
    replacement = NULL;
  } else {
    replacement = realloc(pointer, size);
    if (replacement != NULL)
      fixture->live = fixture->live - old + size;
  }
  require(__atomic_fetch_sub(&fixture->allocator_active, 1, __ATOMIC_RELEASE) ==
          1);
  return replacement;
}

void __real_luaM_free_(lua_State *, void *, size_t);
void __wrap_luaM_free_(lua_State *L, void *block, size_t size) {
  struct fixture *fixture = active;
  LuaWorldScope scope = {0};
  int watched = fixture != NULL && block != NULL &&
                block == __atomic_load_n(&fixture->target, __ATOMIC_ACQUIRE);
  if (fixture != NULL && fixture->assist && block != NULL &&
      L == fixture->worker && L->reclaiming) {
    fixture->assisted++;
    if (luaC_tracing(L))
      fixture->trace_assisted++;
  }
  if (watched) {
    require(L == fixture->collector && L->reclaiming);
    require(L->world_depth == 0 && luaW_scope(L).heap == 0);
    require(!__atomic_load_n(&G(L)->world.stopped, __ATOMIC_ACQUIRE));
    require(L->retired != NULL); /* Other private garbage is still pending. */
    if (fixture->detached)
      scope = luaW_detach(L); /* Model an actual allocator ownership wait. */
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    fixture->entered = 1;
    require(pthread_cond_broadcast(&fixture->changed) == 0);
    while (!fixture->release)
      require(pthread_cond_wait(&fixture->changed, &fixture->mutex) == 0);
    require(pthread_mutex_unlock(&fixture->mutex) == 0);
    if (fixture->detached)
      luaW_attach(L, scope);
  }
  __real_luaM_free_(L, block, size);
  if (watched) {
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    fixture->freed++;
    __atomic_store_n(&fixture->target, NULL, __ATOMIC_RELEASE);
    require(pthread_mutex_unlock(&fixture->mutex) == 0);
  }
}

static void *collect(void *argument) {
  struct fixture *fixture = argument;
  require(lua_gc(fixture->collector, LUA_GCCOLLECT) == 0);
  /* Another collector can trim this now-idle coroutine. Inspect its private
   * cleanup state under the same gate used by those runtime borrowers. */
  lua_lock(fixture->collector);
  require(fixture->collector->retired == NULL &&
          fixture->collector->retired_tail == NULL &&
          !fixture->collector->reclaiming &&
          !fixture->collector->reclaim_pages);
  lua_unlock(fixture->collector);
  require(pthread_mutex_lock(&fixture->mutex) == 0);
  fixture->done = 1;
  require(pthread_cond_broadcast(&fixture->changed) == 0);
  require(pthread_mutex_unlock(&fixture->mutex) == 0);
  return NULL;
}

static void *collect_other(void *argument) {
  struct fixture *fixture = argument;
  require(lua_gc(fixture->worker, LUA_GCCOLLECT) == 0);
  return NULL;
}

static const char garbage[] =
    "for i=1,100 do "
    "local co=coroutine.create(function() local v={i}; "
    "coroutine.yield(function() return v end) end); "
    "assert(coroutine.resume(co)); "
    "local s=('retired-%d'):format(i); "
    "local t={s,{i}} "
    "end";

static const char progress[] =
    "local sum=0; for i=1,2000 do "
    "local v={i,i+1,tag=('retired-%d'):format(i%100+1)}; "
    "sum=sum+v[2]; assert(v.tag==('retired-%d'):format(i%100+1)) "
    "end; assert(sum==2003000)";

int main(int argc, char **argv) {
  struct fixture fixture = {0};
  pthread_t collector, other;
  int generational;
  require(argc == 3);
  generational = strcmp(argv[2], "generational") == 0;
  require(generational || strcmp(argv[2], "incremental") == 0);
  fixture.detached = strcmp(argv[1], "collect_again") == 0;
  fixture.handshake = strcmp(argv[1], "handshake") == 0;
  fixture.trace = strcmp(argv[1], "assist_trace") == 0;
  fixture.unwind = strcmp(argv[1], "assist_unwind") == 0;
  fixture.assist =
      fixture.trace || fixture.unwind || strcmp(argv[1], "assist") == 0;
  /* Automatic tracing can now overlap older destruction too. Model the held
   * allocator wait as withdrawn participation, just like collect_again. */
  if (fixture.assist)
    fixture.detached = 1;
  require(fixture.detached || fixture.handshake || fixture.assist ||
          strcmp(argv[1], "overlap") == 0);
  require(pthread_mutex_init(&fixture.mutex, NULL) == 0);
  require(pthread_cond_init(&fixture.changed, NULL) == 0);
  fixture.main = lua_newstate(allocate, &fixture, 0);
  require(fixture.main != NULL);
  luaL_openlibs(fixture.main);
  fixture.collector = lua_newthread(fixture.main);
  fixture.worker = lua_newthread(fixture.main);
  lua_gc(fixture.main, LUA_GCSTOP);
  if (generational)
    lua_gc(fixture.main, LUA_GCGEN);
  lua_gc(fixture.main, LUA_GCCOLLECT);
  require(luaL_dostring(fixture.main, garbage) == LUA_OK);
  if (fixture.assist) {
    unsigned i;
    for (i = 0; i < 9; i++)
      require(luaL_dostring(fixture.main, garbage) == LUA_OK);
  }
  if (fixture.trace)
    require(luaL_dostring(fixture.main,
                          "local head; for i=1,50000 do head={head} end; "
                          "retained_chain=head") == LUA_OK);
  /* Preserve traversal order: this newest object enters the first private
   * batch, with coroutine, capture, prototype and string garbage still queued.
   */
  lua_newtable(fixture.main);
  fixture.target = (void *)lua_topointer(fixture.main, -1);
  lua_pop(fixture.main, 1);
  if (fixture.assist)
    lua_gc(fixture.main, LUA_GCRESTART);
  active = &fixture;
  require(pthread_create(&collector, NULL, collect, &fixture) == 0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.entered)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(!fixture.done && !fixture.freed);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(__atomic_load_n(&G(fixture.main)->GCretired, __ATOMIC_RELAXED) > 0);
  if (fixture.assist) {
    if (fixture.trace)
      lua_gc(fixture.worker, LUA_GCINC);
    lua_lock(fixture.worker);
    if (fixture.trace) {
      luaW_stop(fixture.worker);
      luaC_runtilstate(fixture.worker, GCSpropagate, 1);
      G(fixture.worker)->GCmarkdebt = MAX_LMEM;
      luaW_start(fixture.worker);
      require(luaC_tracing(fixture.worker));
    }
    fixture.worker->assist_debt =
        fixture.trace ? LUA_GC_ASSIST_BYTES : MAX_LMEM;
    fixture.worker->assist_epoch = G(fixture.worker)->world.work_epoch;
    if (fixture.unwind) {
      unsigned before;
      fixture.interrupt = 1;
      require(luaD_rawrunprotected(fixture.worker, interrupted_reclaim, NULL) ==
              LUA_ERRRUN);
      require(!fixture.worker->reclaiming && fixture.worker->native_depth == 0);
      before = (unsigned)fixture.assisted;
      fixture.worker->assist_debt = MAX_LMEM;
      luaC_assistreclaim(fixture.worker);
      require((unsigned)fixture.assisted > before);
    }
    if (fixture.trace) {
      fixture.worker->trace_debt = LUA_GC_ASSIST_BYTES;
      luaC_assisttrace(fixture.worker);
      require(fixture.trace_assisted != 0 && luaC_tracing(fixture.worker));
    } else
      luaC_assistreclaim(fixture.worker);
    lua_unlock(fixture.worker);
    /* All shared work is gone; only the deliberately held claim remains.
     * Assistance must return and allow ordinary allocation to continue. */
    if (!fixture.trace)
      require(fixture.collector->retired == NULL &&
              fixture.collector->reclaim_workers == 1);
  }
  /* This must finish while destruction is held, including backing allocation,
   * shared string interning and ordinary Lua execution on another carrier. */
  require(luaL_dostring(fixture.worker, progress) == LUA_OK);
  if (fixture.assist)
    require(fixture.assisted != 0);
  if (fixture.detached) {
    require(lua_gc(fixture.worker, LUA_GCCOLLECT) == 0);
    require(luaL_dostring(fixture.worker, progress) == LUA_OK);
    require(lua_gc(fixture.worker, LUA_GCCOLLECT) == 0);
  }
  if (fixture.handshake) {
    require(pthread_create(&other, NULL, collect_other, &fixture) == 0);
    require(pthread_mutex_lock(&fixture.mutex) == 0);
    while (!fixture.waiting)
      require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
    require(pthread_mutex_unlock(&fixture.mutex) == 0);
  }
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  require(!fixture.done && !fixture.freed);
  fixture.release = 1;
  require(pthread_cond_broadcast(&fixture.changed) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  if (fixture.handshake) {
    require(pthread_join(other, NULL) == 0);
    require(fixture.observed);
  }
  require(pthread_join(collector, NULL) == 0);
  require(fixture.done && fixture.freed == 1);
  active = NULL;
  lua_gc(fixture.main, LUA_GCCOLLECT);
  require(G(fixture.main)->GCretired == 0);
  lua_close(fixture.main);
  require(fixture.live == 0 && fixture.allocator_active == 0);
  require(pthread_cond_destroy(&fixture.changed) == 0);
  require(pthread_mutex_destroy(&fixture.mutex) == 0);
  return 0;
}
