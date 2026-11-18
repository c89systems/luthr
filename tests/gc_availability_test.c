#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lstate.h>
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

static __thread unsigned batches, phase_entries;
static __thread int pause_scan;
static struct {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  int entered, release;
} scanner = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};
void __real_luaC_scantrace(lua_State *, LuaTraceWork *, unsigned);
void __wrap_luaC_scantrace(lua_State *L, LuaTraceWork *work, unsigned limit) {
  if (pause_scan) {
    pause_scan = 0;
    require(pthread_mutex_lock(&scanner.mutex) == 0);
    scanner.entered = 1;
    require(pthread_cond_broadcast(&scanner.changed) == 0);
    while (!scanner.release)
      require(pthread_cond_wait(&scanner.changed, &scanner.mutex) == 0);
    require(pthread_mutex_unlock(&scanner.mutex) == 0);
  }
  __real_luaC_scantrace(L, work, limit);
}
lu_mem __real_luaC_tracebatch(lua_State *, unsigned);
lu_mem __wrap_luaC_tracebatch(lua_State *L, unsigned limit) {
  batches++;
  return __real_luaC_tracebatch(L, limit);
}
int __real_luaW_trycollectenter(lua_State *);
int __wrap_luaW_trycollectenter(lua_State *L) {
  phase_entries++;
  return __real_luaW_trycollectenter(L);
}

struct request {
  lua_State *state;
  GCObject *published;
  unsigned batches, phase_entries;
  lu_mem debt;
  int pressure, resume, pause;
};

static void *assist(void *argument) {
  struct request *r = argument;
  unsigned i;
  pause_scan = r->pause;
  lua_lock(r->state);
  r->state->assist_epoch = G(r->state)->world.work_epoch;
  if (!r->resume)
    r->state->trace_debt = r->pressure ? 0 : LUA_GC_ASSIST_BYTES;
  if (r->published != NULL)
    luaC_traceobject(r->state, r->published);
  for (i = 0; i < (r->resume || r->pressure ? 1u : 1000u); i++) {
    luaC_checkGC(r->state);
    luaC_polltrace(r->state);
  }
  r->debt = r->state->trace_debt;
  r->batches = batches;
  r->phase_entries = phase_entries;
  lua_unlock(r->state);
  return NULL;
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  struct request r = {0};
  struct request other = {0};
  pthread_t worker;
  unsigned length = 0, slices = 0;
  GCObject *late;
  int publish, remote, busy;
  require(argc == 2 && L != NULL);
  remote = strncmp(argv[1], "remote_", 7) == 0;
  busy = strncmp(argv[1], "busy_", 5) == 0;
  publish = busy || remote || strncmp(argv[1], "publish_", 8) == 0;
  r.pressure = strncmp(argv[1], "pressure_", 9) == 0;
  lua_gc(L, strstr(argv[1], "generational") != NULL ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  require(luaL_loadstring(L, "local h; for i=1,3000 do h={h} end; return h") ==
          LUA_OK);
  require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  r.state = lua_newthread(L);
  if (busy)
    other.state = lua_newthread(L);
  lua_newtable(L);
  late = (GCObject *)lua_topointer(L, -1);
  lua_pop(L, 1); /* White until a barrier publishes it during this trace. */
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  lua_gc(L, LUA_GCRESTART);
  lua_lock(L);
  G(L)->pacer.goal = MAX_LMEM;
  luaE_setdebt(G(L), 0);
  luaC_step(L);
  require(luaC_tracing(L));
  luaW_heapenter(L);
  while (__atomic_load_n(&G(L)->trace_ready, __ATOMIC_ACQUIRE)) {
    luaC_tracebatch(L, LUA_TRACE_BATCH);
    require(++slices < 10000);
  }
  require(G(L)->trace_busy == 0 && iswhite(G(L), late));
  /* Assistance is due, but ordinary work is empty and this host retains the
   * phase permit. Repeating checks cannot create more useful collector work. */
  G(L)->pacer.goal = G(L)->pacer.start;
  G(L)->pacer.expected = G(L)->pacer.work + LUA_GC_ASSIST_BYTES;
  if (r.pressure)
    __atomic_store_n(&G(L)->pacer.debt, -MAX_LMEM, __ATOMIC_RELAXED);
  if (remote)
    luaC_traceobject(L, late);
  else if (publish)
    r.published = late;
  r.pause = busy;
  require(pthread_create(&worker, NULL, assist, &r) == 0);
  if (busy) {
    pthread_t observer;
    require(pthread_mutex_lock(&scanner.mutex) == 0);
    while (!scanner.entered)
      require(pthread_cond_wait(&scanner.changed, &scanner.mutex) == 0);
    require(pthread_mutex_unlock(&scanner.mutex) == 0);
    require(G(L)->trace_busy == 1);
    luaW_heapleave(L);
    /* This batch is owned by a scanner, not available to another helper. The
     * phase permit is free; the observer still owes no empty batch or pause. */
    require(pthread_create(&observer, NULL, assist, &other) == 0);
    require(pthread_join(observer, NULL) == 0);
    require(other.batches == 0 && other.phase_entries == 0 &&
            other.debt == LUA_GC_ASSIST_BYTES);
    luaW_heapenter(L);
    require(pthread_mutex_lock(&scanner.mutex) == 0);
    scanner.release = 1;
    require(pthread_cond_broadcast(&scanner.changed) == 0);
    require(pthread_mutex_unlock(&scanner.mutex) == 0);
  }
  if (r.pressure) {
    require(pthread_mutex_lock(&G(L)->world.mutex) == 0);
    while (!r.state->heap_waiting)
      luaQ_wait(L, &G(L)->world.mutex, &G(L)->world.changed,
                &G(L)->world.waiters);
    require(pthread_mutex_unlock(&G(L)->world.mutex) == 0);
  } else {
    require(pthread_join(worker, NULL) == 0);
    require(r.debt != 0 && r.phase_entries < 8);
    if (publish)
      require(r.batches > 0 && r.batches < 8 && isblack(G(L), late));
    else
      require(r.batches == 0 && r.phase_entries == 0 &&
              r.debt == LUA_GC_ASSIST_BYTES);
  }
  luaW_heapleave(L);
  lua_unlock(L);
  if (r.pressure) {
    require(pthread_join(worker, NULL) == 0);
    require(!luaC_tracing(L));
  } else {
    r.resume = 1;
    r.pause = 0;
    r.published = NULL;
    require(pthread_create(&worker, NULL, assist, &r) == 0);
    require(pthread_join(worker, NULL) == 0);
    require(!luaC_tracing(L) && r.phase_entries != 0);
  }
  lua_gc(L, LUA_GCCOLLECT);
  lua_pushvalue(L, 1);
  while (lua_istable(L, -1)) {
    lua_rawgeti(L, -1, 1);
    lua_remove(L, -2);
    length++;
  }
  require(length == 3000);
  lua_close(L);
  require(pthread_cond_destroy(&scanner.changed) == 0);
  require(pthread_mutex_destroy(&scanner.mutex) == 0);
  return 0;
}
