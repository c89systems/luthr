#include <laccess.h>
#include <lapi.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lgc.h>
#include <lmempage.h>
#include <lstate.h>
#include <lstring.h>
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

static lua_State *observed, *first_publisher, *second_publisher;
static unsigned published, unrelated;
static unsigned trim_calls;
static int count_trims;
static __thread int reject_string_locks;
static global_State *notice_vm;
static unsigned notices;

void __real_luaSC_notifygc(global_State *);
void __wrap_luaSC_notifygc(global_State *g) {
  if (g == notice_vm)
    ++notices;
  __real_luaSC_notifygc(g);
}

void __real_luaA_beginlock(lua_State *, LuaObjectLock *, LuaObjectScope *);
void __wrap_luaA_beginlock(lua_State *L, LuaObjectLock *lock,
                           LuaObjectScope *scope) {
  require(!reject_string_locks ||
          (uintptr_t)lock - (uintptr_t)G(L)->strt >= sizeof(G(L)->strt));
  __real_luaA_beginlock(L, lock, scope);
}

void __real_luaM_flushdebt(lua_State *);
void __wrap_luaM_flushdebt(lua_State *owner) {
  if (observed != NULL && luaW_iscollector(observed)) {
    if (owner == first_publisher)
      published |= 1;
    else if (owner == second_publisher)
      published |= 2;
    else
      unrelated++;
  }
  __real_luaM_flushdebt(owner);
}

void __real_luaD_shrinkstack(lua_State *L, lua_State *owner);
void __wrap_luaD_shrinkstack(lua_State *L, lua_State *owner) {
  if (count_trims)
    trim_calls++;
  __real_luaD_shrinkstack(L, owner);
}

static void publish_batches(lua_State *L, int generational) {
  Table *table;
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_createtable(L, 1024, 0);
  for (int i = 1; i <= 1024; i++) {
    lua_newthread(L);
    lua_rawseti(L, 1, i);
  }
  first_publisher = lua_newthread(L);
  second_publisher = lua_newthread(L);
  lua_createtable(second_publisher, 32, 0);
  lua_pushinteger(second_publisher, 43);
  lua_rawseti(second_publisher, 1, 1);
  table = (Table *)lua_topointer(second_publisher, 1);
  /* Exercise a published storage batch by making the table visible to
   * another coroutine before the resize. */
  lua_pushvalue(second_publisher, 1);
  lua_xmove(second_publisher, L, 1);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT);

  lua_newtable(first_publisher);
  lua_pushinteger(first_publisher, 42);
  lua_rawseti(first_publisher, 1, 1);
  lua_lock(second_publisher);
  require(first_publisher->new_tables != NULL);
  luaH_resize(second_publisher, table, 64, 0);
  require(second_publisher->new_objects == NULL &&
          second_publisher->storage_retired != NULL);
  observed = second_publisher;
  luaW_stop(second_publisher);
  require(published == 3 && unrelated == 0);
  require(first_publisher->new_tables == NULL &&
          second_publisher->storage_retired == NULL);
  observed = NULL;
  luaW_start(second_publisher);
  lua_unlock(second_publisher);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_rawgeti(first_publisher, 1, 1) == LUA_TNUMBER &&
          lua_tointeger(first_publisher, -1) == 42);
  require(lua_rawgeti(second_publisher, 1, 1) == LUA_TNUMBER &&
          lua_tointeger(second_publisher, -1) == 43);
  lua_close(L);
}

struct startup_allocator {
  size_t requests, fail_at, live;
  int deny_cache, denied_cache;
};

static void *startup_alloc(void *argument, void *old, size_t old_size,
                           size_t size) {
  struct startup_allocator *test = argument;
  void *memory;
  if (size == 0) {
    test->live -= old_size;
    free(old);
    return NULL;
  }
  if (test->deny_cache &&
      size == sizeof(LuaStringCache) + sizeof(LuaAllocBlock)) {
    test->denied_cache++;
    return NULL;
  }
  if (++test->requests >= test->fail_at)
    return NULL;
  if (old == NULL)
    old_size = 0;
  memory = realloc(old, size);
  if (memory != NULL)
    test->live = test->live - old_size + size;
  return memory;
}

static void check_cached_spans(lua_State *L, lua_State *owner) {
  char source[] = {'k', 'e', 'y', '\0', 'x', '?'};
  char copy[sizeof(source)];
  const size_t width = sizeof(source) - 1;
  size_t len;
  const char *first = lua_pushlstring(owner, source, width);
  require(first[width] == '\0' && memcmp(first, source, width) == 0);
  reject_string_locks = 1;
  require(lua_pushlstring(owner, source, width) == first);
  lua_pop(owner, 1);
  reject_string_locks = 0;

  memcpy(copy, source, sizeof(copy));
  require(lua_pushlstring(owner, copy, width) == first);
  lua_pop(owner, 1);
  /* A zero-terminated lookup must stop at the embedded zero. */
  require(strcmp(lua_pushstring(owner, source), "key") == 0);
  lua_tolstring(owner, -1, &len);
  require(len == 3);
  lua_pop(owner, 1);

  source[4] = 'y';
  require(memcmp(lua_pushlstring(owner, source, width), source, width) == 0);
  require(!lua_rawequal(owner, -1, -2));
  lua_pop(owner, 2);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCCOLLECT);
  require(memcmp(lua_pushlstring(owner, copy, width), copy, width) == 0);
  lua_pop(owner, 1);
}

static void check_cached_results(lua_State *L, lua_State *owner) {
  static const char sources[][8] = {"0aQ\0xd1", "0aR\0xd1", "2aQ\0xd3"};
  static const char reversed[] = "dx\0Qa";
  luaL_requiref(owner, LUA_STRLIBNAME, luaopen_string, 0);
  lua_getfield(owner, 1, "match");
  lua_getfield(owner, 1, "reverse");
  for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++)
    lua_pushexternalstring(owner, sources[i], sizeof(sources[i]) - 1, NULL,
                           NULL);
  lua_pushliteral(owner, "^.(.....).$");
  lua_pushexternalstring(owner, reversed, sizeof(reversed) - 1, NULL, NULL);

  for (int cycle = 0; cycle < 3; cycle++) {
    for (int i = 0; i < 3; i++) {
      size_t len;
      const char *value;
      lua_pushvalue(owner, 2);
      lua_pushvalue(owner, 4 + i);
      lua_pushvalue(owner, 7);
      /* The middle source collides with the first. The last must still hit. */
      reject_string_locks = i == 2;
      lua_call(owner, 2, 1);
      reject_string_locks = 0;
      value = lua_tolstring(owner, -1, &len);
      require(value != NULL && len == 5 &&
              memcmp(value, sources[i] + 1, 5) == 0);
      if (i == 0) {
        /* Repeat the result so it is eligible for the interned cache. */
        lua_pushvalue(owner, 2);
        lua_pushvalue(owner, 4);
        lua_pushvalue(owner, 7);
        lua_call(owner, 2, 1);
        require(lua_rawequal(owner, -1, -2));
        lua_pop(owner, 1);
      }
    }
    require(lua_rawequal(owner, 9, 11) && !lua_rawequal(owner, 9, 10));
    lua_pushvalue(owner, 3);
    lua_pushvalue(owner, 8);
    reject_string_locks = 1;
    lua_call(owner, 1, 1);
    reject_string_locks = 0;
    require(lua_rawequal(owner, 9, -1));

    lua_settop(owner, 8);
    lua_gc(L, LUA_GCCOLLECT);
    lua_gc(L, LUA_GCCOLLECT);
  }
  lua_settop(owner, 0);
}

static void invalidate_string_cache(int generational) {
  struct startup_allocator test = {.fail_at = SIZE_MAX};
  lua_State *L = lua_newstate(startup_alloc, &test, 42);
  lua_State *owner;
  char source[4097];
  lu_mem initial_size;
  require(L != NULL);
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  owner = lua_newthread(L);
  require(owner->strcache == NULL);
  initial_size = luaE_threadsize(owner);
  memset(source, 'a', sizeof(source) - 1);
  source[sizeof(source) - 1] = '\0';
  test.deny_cache = 1;
  require(strcmp(lua_pushstring(owner, source), source) == 0);
  require(test.denied_cache == 1 && owner->strcache == NULL);
  require(luaE_threadsize(owner) == initial_size);
  lua_pop(owner, 1);
  test.deny_cache = 0;
  for (int i = 0; i < 3; i++) {
    uint64_t epoch;
    const char *value = lua_pushstring(owner, source);
    require(strcmp(value, source) == 0);
    require(owner->strcache != NULL &&
            luaE_threadsize(owner) == initial_size + sizeof(LuaStringCache));
    size_t len;
    require(lua_tolstring(owner, -1, &len) == value &&
            len == sizeof(source) - 1);
    epoch = owner->strcache->epoch;
    require(epoch == G(L)->strcache_epoch);
    require(lua_pushstring(owner, source) == value);
    lua_pop(owner, 2);
    lua_gc(L, LUA_GCCOLLECT);
    require(owner->strcache->epoch == epoch && G(L)->strcache_epoch != epoch);
    source[0]++;
  }
  lua_lock(L);
  luaW_stop(L);
  /* Reuse of epoch one must also invalidate owners that last used it. */
  owner->strcache->epoch = 1;
  G(L)->strcache_epoch = UINT64_MAX;
  luaS_clearcache(G(L));
  require(G(L)->strcache_epoch == 1 && owner->strcache->epoch == 0);
  luaW_start(L);
  lua_unlock(L);
  require(strcmp(lua_pushstring(owner, source), source) == 0);
  lua_pop(owner, 1);
  check_cached_spans(L, owner);
  check_cached_results(L, owner);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT);
  lua_close(L);
  require(test.live == 0);
}

static void byte_strings(lua_State *L, int generational) {
  char source[UCHAR_MAX + 1][2];
  const char *identity[UCHAR_MAX + 1], *empty;
  lua_State *owner;
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_newtable(L);
  owner = lua_newthread(L);
  reject_string_locks = 1;
  empty = lua_pushlstring(L, NULL, 0);
  lua_pop(L, 1);
  for (int i = 0; i <= UCHAR_MAX; i++) {
    source[i][0] = (char)i;
    source[i][1] = '\0';
    identity[i] = lua_pushlstring(L, source[i], 1);
    lua_pop(L, 1);
    lua_pushexternalstring(L, source[i], 1, NULL, NULL);
    lua_pushinteger(L, i);
    lua_rawset(L, 1);
  }
  reject_string_locks = 0;
  lua_gc(L, LUA_GCCOLLECT);
  reject_string_locks = 1;
  require(lua_pushstring(owner, "") == empty);
  for (int i = 0; i <= UCHAR_MAX; i++) {
    require(lua_pushlstring(owner, source[i], 1) == identity[i]);
    lua_pop(owner, 1);
    if (i != 0) {
      require(lua_pushstring(owner, source[i]) == identity[i]);
      lua_pop(owner, 1);
    }
    require(lua_pushlstring(L, source[i], 1) == identity[i]);
    require(lua_rawget(L, 1) == LUA_TNUMBER && lua_tointeger(L, -1) == i);
    lua_pop(L, 1);
  }
  reject_string_locks = 0;
  lua_close(L);
}

static void startup_strings(void) {
  struct startup_allocator test = {.fail_at = SIZE_MAX};
  lua_State *state = lua_newstate(startup_alloc, &test, 42);
  size_t requests;
  require(state != NULL);
  requests = test.requests;
  lua_close(state);
  require(test.live == 0);
  for (size_t fail = 1; fail <= requests; fail++) {
    test = (struct startup_allocator){.fail_at = fail};
    state = lua_newstate(startup_alloc, &test, 42);
    if (state != NULL)
      lua_close(state);
    require(test.live == 0);
  }
}

static void trim_idle_stacks(lua_State *L, int generational) {
  lua_State *large, *frames, *packed;
  size_t frame_capacity;
  int packed_size;
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_createtable(L, 128, 0);
  for (int i = 1; i <= 128; i++) {
    lua_newthread(L);
    lua_rawseti(L, 1, i);
  }

  large = lua_newthread(L);
  lua_pushinteger(large, 42);
  lua_lock(large);
  require(luaD_reallocstack(large, 1024, 1));
  lua_unlock(large);

  frames = lua_newthread(L);
  lua_lock(frames);
  for (int i = 0; i < 5; i++)
    require(luaE_extendCI(frames, 1) != NULL);
  lua_unlock(frames);
  frame_capacity = frames->call_bytes;
  require(frame_capacity > 0 && stacksize(frames) == BASIC_STACK_SIZE);

  packed = lua_newthread(L);
  require(lua_checkstack(packed, 80));
  for (int i = 1; i <= 80; i++)
    lua_pushinteger(packed, i);
  packed_size = stacksize(packed);

  count_trims = 1;
  require(lua_gc(L, LUA_GCCOLLECT) == 0);
  count_trims = 0;
  require(trim_calls == 2);
  require(stacksize(large) < 1024 && lua_tointeger(large, -1) == 42);
  require(frames->call_bytes < frame_capacity &&
          stacksize(frames) == BASIC_STACK_SIZE);
  require(stacksize(packed) == packed_size && lua_tointeger(packed, -1) == 80);
  lua_close(L);
}

enum { ACTORS = LUA_BARRIER_SHARDS * 2, ROWS = 32, OBJECTS = ACTORS * ROWS };

static void barrier_notices(lua_State *L) {
  global_State *g = G(L);
  GCObject *objects[3];
  lua_gc(L, LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_createtable(L, 3, 0);
  for (unsigned i = 0; i < 3; ++i) {
    lua_newtable(L);
    objects[i] = (GCObject *)lua_topointer(L, -1);
    lua_rawseti(L, 1, i + 1);
  }
  lua_lock(L);
  luaW_stop(L);
  luaC_runtilstate(L, GCSpropagate, 1);
  luaW_start(L);
  notice_vm = g;
  require(iswhite(g, objects[0]) && iswhite(g, objects[1]) &&
          iswhite(g, objects[2]));
  luaC_traceobject(L, objects[0]);
  require(luaC_barriernotice(g));
  luaC_notifybarriers(g);
  require(notices == 1 && !luaC_barriernotice(g) && g->barrier_ready != 0);
  luaC_traceobject(L, objects[1]);
  luaC_notifybarriers(g);
  require(notices == 1 && isgray(objects[0]) && isgray(objects[1]));
  luaC_drainbarriers(L);
  require(g->barrier_ready == 0);

  luaC_traceobject(L, objects[2]);
  require(luaC_barriernotice(g));
  lua_unlock(L);
  require(notices == 2 && !luaC_barriernotice(g) && g->barrier_ready != 0);
  notice_vm = NULL;
  lua_gc(L, LUA_GCCOLLECT);
  for (unsigned i = 1; i <= 3; ++i) {
    require(lua_rawgeti(L, 1, i) == LUA_TTABLE);
    lua_pop(L, 1);
  }
  lua_close(L);
}
struct producer {
  lua_State *state;
  GCObject **objects;
  Table *child;
  pthread_barrier_t *start;
  unsigned *done;
};

static void *publish(void *argument) {
  struct producer *producer = argument;
  lua_State *L = producer->state;
  unsigned i;
  int status = pthread_barrier_wait(producer->start);
  require(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
  lua_lock(L);
  for (i = 0; i < ROWS; i++) {
    GCObject *object = producer->objects[i];
    if (producer->child != NULL) {
      TValue value;
      sethvalue(L, &value, producer->child);
      luaH_setint(L, gco2t(object), 2, &value);
    } else {
      require(iswhite(G(L), object));
      luaC_traceobject(L, object);
    }
  }
  lua_unlock(L);
  __atomic_fetch_add(producer->done, 1, __ATOMIC_RELEASE);
  return NULL;
}

int main(int argc, char **argv) {
  lua_State *L = luaL_newstate();
  struct producer producers[ACTORS];
  pthread_t workers[ACTORS];
  pthread_barrier_t start;
  GCObject **objects = calloc(OBJECTS, sizeof(*objects));
  unsigned i, done = 0;
  Table *child = NULL;
  int back;
  require(argc == 2 && L != NULL && objects != NULL);
  if (strcmp(argv[1], "notice") == 0) {
    free(objects);
    barrier_notices(L);
    return 0;
  }
  if (strncmp(argv[1], "publish_", 8) == 0) {
    free(objects);
    publish_batches(L, strcmp(argv[1], "publish_generational") == 0);
    return 0;
  }
  if (strncmp(argv[1], "string_cache_", 13) == 0) {
    free(objects);
    lua_close(L);
    invalidate_string_cache(strcmp(argv[1], "string_cache_generational") == 0);
    return 0;
  }
  if (strncmp(argv[1], "byte_strings_", 13) == 0) {
    free(objects);
    byte_strings(L, strcmp(argv[1], "byte_strings_generational") == 0);
    return 0;
  }
  if (strcmp(argv[1], "string_startup") == 0) {
    free(objects);
    lua_close(L);
    startup_strings();
    return 0;
  }
  if (strncmp(argv[1], "trim_", 5) == 0) {
    free(objects);
    trim_idle_stacks(L, strcmp(argv[1], "trim_generational") == 0);
    return 0;
  }
  back = strcmp(argv[1], "remembered") == 0;
  lua_gc(L, back ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_createtable(L, OBJECTS, 0);
  for (i = 0; i < OBJECTS; i++) {
    lua_createtable(L, 2, 0);
    objects[i] = (GCObject *)lua_topointer(L, -1);
    lua_pushinteger(L, i + 1);
    lua_rawseti(L, -2, 1);
    lua_rawseti(L, 1, i + 1);
  }
  if (back) {
    lua_gc(L, LUA_GCCOLLECT);
    lua_newtable(L);
    lua_pushinteger(L, 42);
    lua_rawseti(L, -2, 1);
    child = (Table *)lua_topointer(L, -1);
  }
  require(lua_checkstack(L, ACTORS + 1));
  require(pthread_barrier_init(&start, NULL, ACTORS) == 0);
  for (i = 0; i < ACTORS; i++) {
    producers[i].state = lua_newthread(L);
    producers[i].objects = objects + i * ROWS;
    producers[i].child = child;
    producers[i].start = &start;
    producers[i].done = &done;
  }
  lua_lock(L);
  if (!back) {
    luaW_stop(L);
    luaC_runtilstate(L, GCSpropagate, 1);
    require(luaC_tracing(L));
    luaW_start(L);
    for (i = 0; i < OBJECTS; i++)
      require(iswhite(G(L), objects[i]));
  }
  for (i = 0; i < ACTORS; i++)
    require(pthread_create(&workers[i], NULL, publish, &producers[i]) == 0);
  /* More producers than shards guarantee collisions. Consume concurrently,
   * including empty-directory passes and producer publication/clear races. */
  while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) != ACTORS) {
    require(pthread_mutex_lock(&G(L)->world.mutex) == 0);
    luaC_drainbarriers(L);
    require(pthread_mutex_unlock(&G(L)->world.mutex) == 0);
    sched_yield();
  }
  for (i = 0; i < ACTORS; i++)
    require(pthread_join(workers[i], NULL) == 0);
  require(pthread_mutex_lock(&G(L)->world.mutex) == 0);
  luaC_drainbarriers(L);
  require(G(L)->barrier_ready == 0);
  for (i = 0; i < LUA_BARRIER_SHARDS; i++)
    require(G(L)->barriers[i].forward == NULL &&
            G(L)->barriers[i].back == NULL);
  for (i = 0; i < OBJECTS; i++)
    require(isgray(objects[i]));
  require(pthread_mutex_unlock(&G(L)->world.mutex) == 0);
  lua_unlock(L);
  /* Retire every producer root before finishing collection. Published work
   * belongs to the VM, and its target graph must survive producer retirement.
   */
  lua_settop(L, 1);
  lua_gc(L, LUA_GCCOLLECT);
  for (i = 0; i < OBJECTS; i++) {
    require(lua_rawgeti(L, 1, i + 1) == LUA_TTABLE);
    lua_rawgeti(L, -1, 1);
    require(lua_tointeger(L, -1) == (lua_Integer)i + 1);
    lua_pop(L, 1);
    if (back) {
      require(lua_rawgeti(L, -1, 2) == LUA_TTABLE);
      lua_rawgeti(L, -1, 1);
      require(lua_tointeger(L, -1) == 42);
      lua_pop(L, 2);
    }
    lua_pop(L, 1);
  }
  lua_close(L);
  free(objects);
  require(pthread_barrier_destroy(&start) == 0);
  return 0;
}
