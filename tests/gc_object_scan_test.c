#include <laccess.h>
#include <lapi.h>
#include <lauxlib.h>
#include <lfunc.h>
#include <lgc.h>
#include <lstate.h>
#include <pthread.h>
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
  GCObject *target;
  int index, kind, armed, entered, release, written;
} probe = {.mutex = PTHREAD_MUTEX_INITIALIZER,
           .changed = PTHREAD_COND_INITIALIZER};
static __thread int marking;

static void hold(void) {
  require(pthread_mutex_lock(&probe.mutex) == 0);
  if (probe.armed && !probe.entered) {
    G(probe.marker)->gcstp = GCSTPUSR;
    G(probe.marker)->pacer.debt = MAX_LMEM;
    probe.entered = 1;
    require(pthread_cond_broadcast(&probe.changed) == 0);
    while (!probe.release)
      require(pthread_cond_wait(&probe.changed, &probe.mutex) == 0);
  }
  require(pthread_mutex_unlock(&probe.mutex) == 0);
}

unsigned __real_luaC_objectchunk(GCObject *, unsigned, unsigned, GCObject **)
    __attribute__((weak));
unsigned __wrap_luaC_objectchunk(GCObject *object, unsigned first,
                                 unsigned count, GCObject **edges) {
  unsigned n = __real_luaC_objectchunk(object, first, count, edges);
  if (marking && object == probe.target)
    hold();
  return n;
}
int __real_luaA_tryscan(lua_State *, GCObject *);
int __wrap_luaA_tryscan(lua_State *L, GCObject *object) {
  int result = __real_luaA_tryscan(L, object);
  if (result && L == probe.marker && object == probe.target)
    hold();
  return result;
}
int __real_pthread_mutex_trylock(pthread_mutex_t *);
int __wrap_pthread_mutex_trylock(pthread_mutex_t *mutex) {
  int result = __real_pthread_mutex_trylock(mutex);
  if (result == 0 && marking && probe.kind == 3 &&
      mutex == &gco2upv(probe.target)->mutex)
    hold();
  return result;
}

static void *mark(void *unused) {
  unsigned i;
  (void)unused;
  lua_lock(probe.marker);
  marking = 1;
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
  marking = 0;
  lua_unlock(probe.marker);
  return NULL;
}

static void *write_values(void *unused) {
  lua_State *L = probe.writer;
  unsigned i;
  (void)unused;
  for (i = 0; i < 128; i++) {
    if (probe.kind == 4) {
      lua_createtable(L, 1, 0);
      lua_pushinteger(L, i);
      lua_rawseti(L, -2, 1);
      require(lua_setmetatable(L, probe.index));
    } else if (probe.kind == 2)
      lua_upvaluejoin(L, probe.index, 1, probe.index + 1 + (i & 1), 1);
    else {
      lua_pushinteger(L, i & 1 ? LUA_MAXINTEGER : LUA_MININTEGER);
      if (probe.kind == 0)
        require(lua_setiuservalue(L, probe.index, 1));
      else
        require(lua_setupvalue(L, probe.index, 1) != NULL);
    }
  }
  if (probe.kind == 2)
    lua_upvaluejoin(L, probe.index, 1, probe.index + 1, 1);
  else {
    lua_createtable(L, 1, 0);
    lua_pushinteger(L, 42);
    lua_rawseti(L, -2, 1);
    if (probe.kind == 4)
      require(lua_setmetatable(L, probe.index));
    else if (probe.kind == 0)
      require(lua_setiuservalue(L, probe.index, 1));
    else
      require(lua_setupvalue(L, probe.index, 1) != NULL);
  }
  require(pthread_mutex_lock(&probe.mutex) == 0);
  probe.written = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  return NULL;
}

static int noop(lua_State *L) {
  (void)L;
  return 0;
}
static void closure(lua_State *L) {
  require(luaL_loadstring(L, "local v={42}; return function() return v end") ==
          LUA_OK);
  require(lua_pcall(L, 0, 1, 0) == LUA_OK);
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate(), *other;
  pthread_t marker, writer;
  struct timespec deadline;
  unsigned i;
  int shared;
  require(argc == 2 && L != NULL);
  lua_gc(L, strstr(argv[1], "generational") ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCSTOP);
  shared = strstr(argv[1], "shared") != NULL;
  probe.marker = lua_newthread(L);
  other = lua_newthread(L);
  if (strncmp(argv[1], "userdata0", 9) == 0) {
    probe.kind = 4;
    memset(lua_newuserdatauv(L, 48, 0), 0x5a, 48);
    lua_newtable(L);
    require(lua_setmetatable(L, 3));
  } else if (strncmp(argv[1], "userdata", 8) == 0) {
    probe.kind = 0;
    lua_newuserdatauv(L, 1, 1);
    lua_newtable(L);
    require(lua_setiuservalue(L, 3, 1));
  } else if (strncmp(argv[1], "cclosure", 8) == 0) {
    probe.kind = 1;
    lua_newtable(L);
    lua_pushcclosure(L, noop, 1);
  } else {
    probe.kind = strncmp(argv[1], "lclosure", 8) == 0 ? 2 : 3;
    closure(L);
  }
  probe.target = gcvalue(s2v(L->stack.p + 3));
  if (probe.kind == 3)
    probe.target = obj2gco(gco2lcl(probe.target)->upvals[0]);
  if (probe.kind == 2) {
    closure(L);
    closure(L);
  }
  probe.writer = shared ? other : L;
  probe.index = shared ? 1 : 3;
  if (shared)
    lua_xmove(L, other, probe.kind == 2 ? 3 : 1);
  lua_lock(L);
  luaW_stop(L);
  if (G(L)->gckind == KGC_GENMINOR) {
    G(L)->gcstp = 0;
    luaE_setdebt(G(L), 0);
  } else {
    luaC_runtilstate(L, GCSpropagate, 1);
    require(luaC_tracing(L));
  }
  G(L)->pacer.debt = MAX_LMEM;
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
  require(pthread_mutex_lock(&probe.mutex) == 0);
  while (!probe.written)
    require(pthread_cond_timedwait(&probe.changed, &probe.mutex, &deadline) ==
            0);
  probe.release = 1;
  require(pthread_cond_broadcast(&probe.changed) == 0);
  require(pthread_mutex_unlock(&probe.mutex) == 0);
  require(pthread_join(writer, NULL) == 0);
  require(pthread_join(marker, NULL) == 0);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCCOLLECT);
  if (probe.kind == 4) {
    const unsigned char *payload = lua_touserdata(probe.writer, probe.index);
    for (i = 0; i < 48; i++)
      require(payload[i] == 0x5a);
    require(lua_getmetatable(probe.writer, probe.index));
  } else if (probe.kind == 0)
    require(lua_getiuservalue(probe.writer, probe.index, 1) == LUA_TTABLE);
  else
    require(lua_getupvalue(probe.writer, probe.index, 1) != NULL);
  require(lua_rawgeti(probe.writer, -1, 1) == LUA_TNUMBER);
  require(lua_tointeger(probe.writer, -1) == 42);
  lua_close(L);
  require(pthread_cond_destroy(&probe.changed) == 0);
  require(pthread_mutex_destroy(&probe.mutex) == 0);
  return 0;
}
