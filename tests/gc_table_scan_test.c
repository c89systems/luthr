#include <laccess.h>
#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lstate.h>
#include <ltable.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define require(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                  \
      abort();                                                                 \
    }                                                                          \
  } while (0)

static struct {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *marker, *writer;
  Table *table;
  GCObject *child;
  int index, armed, entered, release, written, relocate, pause, pause_release,
      remember, first_write;
  uint64_t initial_layout;
  unsigned chunks;
} probe = {.mutex = PTHREAD_MUTEX_INITIALIZER,
           .changed = PTHREAD_COND_INITIALIZER};
static __thread int scanning, hash_chunk, interrupted;
static __thread unsigned first_chunk;

static void hold(void) {
  require(pthread_mutex_lock(&probe.mutex) == 0);
  if (probe.armed && !probe.entered) {
    /* The fixture drives this collection. Keep the writer's allocation from
     * requesting another phase boundary while this real payload is held. */
    G(probe.marker)->gcstp = GCSTPUSR;
    G(probe.marker)->pacer.hard = MAX_LMEM;
    probe.entered = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    while (!probe.release)
      require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
  }
  require(pthread_mutex_unlock(&probe.mutex) == 0);
}

int __real_luaC_scanstrong(lua_State *, LuaTraceWork *, Table *)
    __attribute__((weak));
int __wrap_luaC_scanstrong(lua_State *L, LuaTraceWork *work, Table *table) {
  int result;
  scanning = L == probe.marker && table == probe.table;
  hash_chunk = 0;
  interrupted = 0;
  result = __real_luaC_scanstrong(L, work, table);
  if (interrupted)
    require(result == 0);
  scanning = 0;
  return result;
}
unsigned __real_luaH_tracechunk(global_State *, const Table *, unsigned,
                                unsigned, int, GCObject **)
    __attribute__((weak));
unsigned __wrap_luaH_tracechunk(global_State *g, const Table *view,
                                unsigned first, unsigned count, int hash,
                                GCObject **edges) {
  unsigned found;
  if (scanning && probe.first_write && !probe.entered) {
    /* Hold an unflagged scan after it chose its validation path and before
     * it loads a value whose type the first concurrent writer will change. */
    require(g == NULL && first == 0 && !hash);
    probe.initial_layout =
        __atomic_load_n(&probe.table->sequence, __ATOMIC_ACQUIRE);
    interrupted = 1;
    hold();
  }
  found = __real_luaH_tracechunk(g, view, first, count, hash, edges);
  hash_chunk = hash;
  first_chunk = first;
  if (scanning)
    probe.chunks++;
  if (scanning && !probe.relocate && !probe.remember && !probe.first_write)
    hold();
  return found;
}
void __real_luaC_traceedges(LuaTraceWork *, GCObject **, unsigned)
    __attribute__((weak));
void __wrap_luaC_traceedges(LuaTraceWork *work, GCObject **edges, unsigned n) {
  __real_luaC_traceedges(work, edges, n);
  if (scanning && probe.relocate && hash_chunk && first_chunk == 0)
    hold();
}
void __real_luaC_finishstrong(LuaTraceWork *, GCObject *) __attribute__((weak));
void __wrap_luaC_finishstrong(LuaTraceWork *work, GCObject *object) {
  __real_luaC_finishstrong(work, object);
  /* The remembered owner now belongs to a private result list. A fresh write
   * must preserve its link until that list is published, and cover two minors.
   */
  if (scanning && probe.remember)
    hold();
}
int __real_luaA_tryscan(lua_State *, GCObject *);
int __wrap_luaA_tryscan(lua_State *L, GCObject *object) {
  int result = __real_luaA_tryscan(L, object);
  /* The same test, linked to the previous runtime, holds its ownership scan. */
  if (result && L == probe.marker && object == (GCObject *)probe.table)
    hold();
  return result;
}

static void *mark(void *unused) {
  unsigned i;
  (void)unused;
  lua_lock(probe.marker);
  for (i = 0; i < 10000; i++) {
    int entered;
    if (luaC_tracing(probe.marker))
      luaC_tracebatch(probe.marker, 64);
    else
      luaC_step(probe.marker);
    require(pthread_mutex_lock(&probe.mutex) == 0);
    entered = probe.entered;
    require(pthread_mutex_unlock(&probe.mutex) == 0);
    if (entered)
      break;
  }
  require(i < 10000);
  lua_unlock(probe.marker);
  return NULL;
}

static void *write_values(void *unused) {
  lua_State *L = probe.writer;
  unsigned i;
  (void)unused;
  if (probe.pause) {
    lua_lock(L);
    luaW_stop(L);
    require(pthread_mutex_lock(&probe.mutex) == 0);
    probe.written = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    while (!probe.pause_release)
      require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
    require(pthread_mutex_unlock(&probe.mutex) == 0);
    luaW_start(L);
    lua_unlock(L);
    return NULL;
  } else if (probe.first_write) {
    lua_pushinteger(L, LUA_MININTEGER);
    lua_rawseti(L, probe.index, 1);
    require(__atomic_load_n(&probe.table->sequence, __ATOMIC_ACQUIRE) ==
            (probe.initial_layout | LUA_TABLE_WRITER_FLAGS));
  } else if (probe.relocate) {
    lua_pushvalue(L, 2);
    lua_pushinteger(L, 123);
    lua_rawset(L, probe.index);
    require(gcvalue(gval(gnode(probe.table, 10))) == probe.child);
  } else {
    /* Change GC tags to full-width scalar bits and force a replacement image
     * while the marker retains the old one. The old image must remain usable.
     */
    for (i = 1; i <= 256; i++) {
      lua_pushinteger(L, i & 1 ? LUA_MAXINTEGER : LUA_MININTEGER);
      lua_rawseti(L, probe.index, i);
    }
    for (i = 0; i < 128; i++) {
      lua_pushfstring(L, "new-%d", (int)i);
      lua_pushinteger(L, i);
      lua_rawset(L, probe.index);
    }
    lua_newtable(L);
    lua_pushinteger(L, 42);
    lua_rawseti(L, -2, 1);
    lua_rawseti(L, probe.index, 1);
  }
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.written = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  return NULL;
}

static void key(lua_State *L, unsigned bucket) {
  static unsigned serial;
  do {
    lua_pushfstring(L, "slot-%d", (int)serial++);
    if ((tsvalue(s2v(L->top.p - 1))->hash & 63) == bucket)
      return;
    lua_pop(L, 1);
  } while (1);
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  pthread_t marker, writer;
  struct timespec deadline;
  unsigned i;
  require((argc == 2 || argc == 3) && L != NULL);
  lua_gc(L, strstr(argv[1], "generational") ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCSTOP);
  probe.relocate = strncmp(argv[1], "relocate", 8) == 0;
  probe.pause = strncmp(argv[1], "pause", 5) == 0;
  probe.remember = strncmp(argv[1], "remember", 8) == 0;
  probe.first_write = strncmp(argv[1], "first_write", 11) == 0;
  probe.marker = lua_newthread(L);
  probe.writer = lua_newthread(L);
  lua_createtable(L, probe.relocate ? 0 : 128, probe.relocate ? 64 : 0);
  probe.table = (Table *)lua_topointer(L, -1);
  if (probe.relocate) {
    require(sizenode(probe.table) == 64);
    key(L, 0);
    lua_pushinteger(L, 1);
    lua_rawset(L, 3);
    key(L, 0);
    lua_newtable(L);
    probe.child = (GCObject *)lua_topointer(L, -1);
    lua_pushinteger(L, 42);
    lua_rawseti(L, -2, 1);
    lua_rawset(L, 3);
    require(gcvalue(gval(gnode(probe.table, 63))) == probe.child);
    for (i = 11; i <= 62; i++) {
      key(L, i);
      lua_pushinteger(L, i);
      lua_rawset(L, 3);
    }
  } else {
    for (i = 1; i <= 128; i++) {
      lua_newtable(L);
      lua_pushinteger(L, 42);
      lua_rawseti(L, -2, 1);
      lua_rawseti(L, 3, i);
    }
    if (strncmp(argv[1], "metatable", 9) == 0) {
      lua_newtable(L);
      lua_setmetatable(L, 3);
    }
  }
  lua_pushvalue(L, 3);
  probe.index = argc == 3 ? 3 : 1;
  if (argc == 3) {
    probe.writer = L;
    lua_pop(L, 1);
  } else
    lua_xmove(L, probe.writer, 1);
  if (probe.relocate) {
    key(L, 63);
    lua_xmove(L, probe.writer, 1);
  }
  if (probe.remember) {
    lua_gc(L, LUA_GCCOLLECT);
    lua_newtable(probe.writer);
    lua_rawseti(probe.writer, 1, 1); /* remember an old owner before tracing */
  }
  lua_lock(L);
  luaW_stop(L);
  if (G(L)->gckind == KGC_GENMINOR) {
    G(L)->gcstp = 0;
    luaE_setdebt(G(L), 0);
  } else {
    luaC_runtilstate(L, GCSpropagate, 1);
    require(luaC_tracing(L));
  }
  G(L)->pacer.hard = MAX_LMEM;
  luaW_start(L);
  lua_unlock(L);
  require(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += 10;
  probe.armed = 1;
  require(pthread_create(&marker, NULL, mark, NULL) == 0);
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.entered)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_create(&writer, NULL, write_values, NULL) == 0);
  if (probe.pause) {
    while (!luaW_stopped(&G(L)->world))
      sched_yield();
    require(pthread_mutex_lock(&probe.mutex) == 0);
    probe.release = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    require(pthread_mutex_unlock(&probe.mutex) == 0);
  }
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.written)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  if (probe.relocate)
    require(!iswhite(G(L), probe.child));
  if (probe.pause) {
    require(probe.chunks == 1);
    probe.pause_release = 1;
  }
  probe.release = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_join(writer, NULL) == 0);
  require(pthread_join(marker, NULL) == 0);
  if (probe.remember) {
    lua_gc(L, LUA_GCCOUNT); /* join this minor, without starting another */
    for (i = 0; i < 2; i++) {
      require(G(L)->gckind == KGC_GENMINOR);
      lua_gc(L, LUA_GCSTEP, (size_t)0);
      lua_gc(L, LUA_GCCOUNT);
    }
    require(G(L)->gckind == KGC_GENMINOR);
  } else {
    lua_gc(L, LUA_GCCOLLECT);
    lua_gc(L, LUA_GCCOLLECT);
  }
  if (probe.first_write) {
    require(lua_rawgeti(probe.writer, probe.index, 1) == LUA_TNUMBER);
    require(lua_tointeger(probe.writer, -1) == LUA_MININTEGER);
  } else if (!probe.relocate) {
    require(lua_rawgeti(probe.writer, probe.index, 1) == LUA_TTABLE);
    lua_rawgeti(probe.writer, -1, 1);
    require(lua_tointeger(probe.writer, -1) == 42);
  } else
    require(gcvalue(gval(gnode(probe.table, 10))) == probe.child);
  lua_close(L);
  require(pthread_cond_destroy(&probe.changed) == 0);
  require(pthread_mutex_destroy(&probe.mutex) == 0);
  return 0;
}
