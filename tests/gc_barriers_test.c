#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lstate.h>
#include <pthread.h>
#include <sched.h>
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

static __thread unsigned stops;
static __thread int measured;
void __real_luaW_stop(lua_State *);
void __wrap_luaW_stop(lua_State *L) {
  if (measured)
    stops++;
  __real_luaW_stop(L);
}

static GCObject *object(lua_State *L, int index) {
  return gcvalue(s2v(L->stack.p + index));
}

static void prepare(lua_State *L) {
  unsigned batches = 0;
  lua_gc(L, LUA_GCCOLLECT);
  lua_lock(L);
  luaW_stop(L);
  if (G(L)->gckind == KGC_INC)
    luaC_runtilstate(L, GCSenteratomic, 0);
  luaW_start(L);
  /* Major tracing now runs with workers admitted. Bring these retained
   * owners to black before testing writes; minor collections still use the
   * remembered-owner protocol between their marking epochs. */
  while (luaC_tracing(L) && (G(L)->gray != NULL || G(L)->barrier_ready != 0 ||
                             luaC_pagetracepending(G(L)))) {
    require(batches++ < 10000);
    luaC_tracebatch(L, 64);
  }
  lua_unlock(L);
}

static int recorded(lua_State *L, int index) {
  return luaC_tracing(L) ? isblack(G(L), object(L, index))
                         : isgray(object(L, index));
}

static void drain(lua_State *L) {
  lua_lock(L);
  luaW_stop(L);
  luaW_start(L);
  lua_unlock(L);
}

static void repeated(lua_State *L) {
  unsigned i;
  lua_newtable(L);
  prepare(L);
  require(isblack(G(L), object(L, 1)));
  lua_newtable(L);
  measured = 1;
  for (i = 0; i < 256; i++) {
    lua_pushvalue(L, 2);
    lua_rawseti(L, 1, 1);
  }
  measured = 0;
  require(stops == 0); /* One dirty owner must not fill an edge buffer. */
  drain(L);
  require(recorded(L, 1));
  lua_settop(L, 1);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_rawgeti(L, 1, 1) == LUA_TTABLE);
}

static int closure(lua_State *L) {
  (void)L;
  return 0;
}

struct link_handoff {
  lua_State *writer;
  GCObject *owner;
  GCObject *young;
};

static void *remember_after_scan(void *context) {
  struct link_handoff *handoff = context;
  lua_lock(handoff->writer);
  /* Synchronization must come from claiming the link after this relaxed
   * observation. A test-side signal would hide the missing handoff. */
  while (isgray(handoff->owner))
    sched_yield();
  luaC_syncbarrier(handoff->writer, handoff->owner, handoff->young);
  lua_unlock(handoff->writer);
  return NULL;
}

static void link_handoff(lua_State *L, int sweeping) {
  struct link_handoff handoff;
  LuaTraceWork work = {0};
  pthread_t worker;
  lua_pushnil(L);
  lua_pushcclosure(L, closure, 1);
  lua_gc(L, LUA_GCCOLLECT);
  handoff.owner = object(L, 1);
  handoff.writer = lua_newthread(L);
  lua_newtable(L);
  handoff.young = object(L, 3);
  work.global = G(L);
  require(G(L)->gckind == KGC_GENMINOR && !luaC_tracing(L));
  require(isold(handoff.owner) && !isold(handoff.young) &&
          !isgray(handoff.owner));
  lua_lock(L);
  setage(handoff.owner, sweeping ? G_OLD : G_TOUCHED2);
  *luaC_graylink(handoff.owner) = NULL;
  l_setbit(handoff.owner->marked, QUEUEDBIT);
  require(pthread_create(&worker, NULL, remember_after_scan, &handoff) == 0);
  require(*luaC_graylink(handoff.owner) == NULL);
  if (sweeping)
    require(!luaC_sweepgray(handoff.owner));
  else
    luaC_finishstrong(&work, handoff.owner, 0);
  require(pthread_join(worker, NULL) == 0);
  require(isgray(handoff.owner) && getage(handoff.owner) == G_TOUCHED1);
  luaW_stop(L);
  require(G(L)->grayagain == handoff.owner);
  luaW_start(L);
  lua_unlock(L);
  lua_gc(L, LUA_GCCOLLECT);
}

static void forward(lua_State *L) {
  unsigned i;
  lua_pushnil(L);
  lua_pushcclosure(L, closure, 1);
  prepare(L);
  require(isblack(G(L), object(L, 1)));
  measured = 1;
  for (i = 0; i < 256; i++) {
    lua_newtable(L);
    require(lua_setupvalue(L, 1, 1) != NULL);
  }
  measured = 0;
  require(stops == 0); /* Publication must not stop other workers. */
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_getupvalue(L, 1, 1) != NULL && lua_istable(L, -1));
}

static void mixed(lua_State *L) {
  lua_newtable(L);
  lua_pushnil(L);
  lua_pushcclosure(L, closure, 1);
  prepare(L);
  require(isblack(G(L), object(L, 1)) && isblack(G(L), object(L, 2)));
  lua_newtable(L);
  lua_newtable(L);
  /* Sharing child 3 must not suppress either owner's publication or the
   * table's independent second edge. */
  lua_pushvalue(L, 3);
  require(lua_setupvalue(L, 2, 1) != NULL);
  lua_pushvalue(L, 3);
  lua_rawseti(L, 1, 1);
  lua_pushvalue(L, 4);
  lua_rawseti(L, 1, 2);
  drain(L);
  require(recorded(L, 1) && recorded(L, 2));
  lua_settop(L, 2);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_rawgeti(L, 1, 2) == LUA_TTABLE);
  lua_pop(L, 1);
  require(lua_rawgeti(L, 1, 1) == LUA_TTABLE);
  require(lua_getupvalue(L, 2, 1) != NULL && lua_rawequal(L, -1, -2));
}

enum { OWNERS = 160, ACTORS = 4, WRITES = 256 };

static void collisions(lua_State *L, int reset) {
  unsigned round, i;
  /* More distinct owners than the former buffer capacity. */
  for (i = 1; i <= OWNERS; i++)
    lua_newtable(L);
  for (round = 0; round < (reset ? 3U : 1U); round++) {
    prepare(L);
    lua_newtable(L);
    measured = 1;
    for (i = 1; i <= OWNERS; i++) {
      unsigned index = round & 1 ? OWNERS + 1 - i : i;
      require(isblack(G(L), object(L, (int)index)));
      lua_pushvalue(L, OWNERS + 1);
      lua_rawseti(L, (int)index, (lua_Integer)round + 1);
      lua_pushvalue(L, OWNERS + 1);
      lua_rawseti(L, (int)index, (lua_Integer)round + 1);
    }
    measured = 0;
    require(stops == 0);
    drain(L);
    for (i = 1; i <= OWNERS; i++)
      require(recorded(L, (int)i));
    lua_settop(L, OWNERS);
  }
  lua_gc(L, LUA_GCCOLLECT);
  for (i = 1; i <= OWNERS; i++) {
    require(lua_rawgeti(L, (int)i, 1) == LUA_TTABLE);
    lua_pop(L, 1);
  }
}

struct actor {
  lua_State *state;
  unsigned id, stops;
  pthread_barrier_t *start;
};

static void *publish(void *argument) {
  struct actor *actor = argument;
  lua_State *L = actor->state;
  unsigned i;
  int status = pthread_barrier_wait(actor->start);
  require(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
  measured = 1;
  for (i = 0; i < WRITES; i++) {
    /* Distinct closures claim one shared referent. Distinct keys dirty one
     * shared owner. Both claims race across independent stack owners. */
    lua_pushvalue(L, 2);
    require(lua_setupvalue(L, 1, 1) != NULL);
    lua_pushvalue(L, 2);
    lua_rawseti(L, 3, (lua_Integer)(actor->id * WRITES + i + 1));
  }
  actor->stops = stops;
  measured = 0;
  return NULL;
}

static void parallel(lua_State *L, int collect) {
  struct actor actors[ACTORS];
  pthread_t workers[ACTORS];
  pthread_barrier_t start;
  unsigned i;
  require(pthread_barrier_init(&start, NULL, ACTORS) == 0);
  lua_newtable(L); /* shared referent */
  lua_newtable(L); /* shared owner */
  for (i = 0; i < ACTORS; i++) {
    lua_State *thread = lua_newthread(L);
    actors[i].state = thread;
    actors[i].id = i;
    actors[i].stops = 0;
    actors[i].start = &start;
    lua_pushnil(thread);
    lua_pushcclosure(thread, closure, 1);
    lua_pushvalue(L, 1);
    lua_xmove(L, thread, 1);
    lua_pushvalue(L, 2);
    lua_xmove(L, thread, 1);
  }
  prepare(L);
  lua_newtable(L); /* black during a major trace, white between minor cycles */
  for (i = 0; i < ACTORS; i++) {
    lua_pushvalue(L, -1);
    lua_xmove(L, actors[i].state, 1);
    lua_replace(actors[i].state, 2);
    require(pthread_create(&workers[i], NULL, publish, &actors[i]) == 0);
  }
  if (collect)
    for (i = 0; i < 10; i++)
      lua_gc(L, LUA_GCCOLLECT);
  for (i = 0; i < ACTORS; i++) {
    require(pthread_join(workers[i], NULL) == 0);
    require(actors[i].stops == 0);
  }
  drain(L);
  lua_gc(L, LUA_GCCOLLECT);
  for (i = 0; i < ACTORS * WRITES; i++) {
    require(lua_rawgeti(L, 2, (lua_Integer)i + 1) == LUA_TTABLE);
    require(lua_rawequal(L, -1, -2));
    lua_pop(L, 1);
  }
  require(pthread_barrier_destroy(&start) == 0);
}

static void targets(lua_State *L) {
  const char *chunk = "return function() return {} end";
  unsigned i;
  lua_pushnil(L);
  lua_pushcclosure(L, closure, 1);
  prepare(L);
  measured = 1;
  for (i = 0; i < 135; i++) {
    switch (i % 9) {
    case 0:
      lua_pushfstring(L, "short-%d", (int)i);
      break;
    case 1: {
      char value[128];
      memset(value, (int)i, sizeof(value));
      lua_pushlstring(L, value, sizeof(value));
      break;
    }
    case 2:
      lua_newtable(L);
      break;
    case 3:
      lua_newuserdatauv(L, 8, 0);
      break;
    case 4:
      lua_newuserdatauv(L, 8, 2);
      break;
    case 5:
      lua_newthread(L); /* GC header follows LX extra space. */
      break;
    case 6:
      require(luaL_loadstring(L, chunk) == LUA_OK);
      break;
    case 7:
      lua_pushthread(L); /* The embedded main thread is already marked. */
      break;
    default:
      lua_pushnil(L);
      lua_pushcclosure(L, closure, 1);
      break;
    }
    require(lua_setupvalue(L, 1, 1) != NULL);
  }
  measured = 0;
  require(stops == 0);
  drain(L);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_getupvalue(L, 1, 1) != NULL && lua_iscfunction(L, -1));
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  require(L != NULL && argc == 3 && lua_checkstack(L, OWNERS + 10));
  if (strcmp(argv[2], "generational") == 0)
    lua_gc(L, LUA_GCGEN);
  else
    require(strcmp(argv[2], "incremental") == 0);
  lua_gc(L, LUA_GCSTOP);
  if (strcmp(argv[1], "repeated") == 0)
    repeated(L);
  else if (strcmp(argv[1], "forward") == 0)
    forward(L);
  else if (strcmp(argv[1], "mixed") == 0)
    mixed(L);
  else if (strcmp(argv[1], "parallel") == 0 ||
           strcmp(argv[1], "parallel_gc") == 0)
    parallel(L, strcmp(argv[1], "parallel_gc") == 0);
  else if (strcmp(argv[1], "targets") == 0)
    targets(L);
  else if (strcmp(argv[1], "trace_handoff") == 0 ||
           strcmp(argv[1], "sweep_handoff") == 0)
    link_handoff(L, strcmp(argv[1], "sweep_handoff") == 0);
  else {
    require(strcmp(argv[1], "collisions") == 0 ||
            strcmp(argv[1], "reset") == 0);
    collisions(L, strcmp(argv[1], "reset") == 0);
  }
  lua_close(L);
  return 0;
}
