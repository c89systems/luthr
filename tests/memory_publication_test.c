#include <laccess.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lgc.h>
#include <lmem.h>
#include <lstate.h>
#include <ltable.h>
#include <ltask.h>
#include <lualib.h>
#include <pthread.h>
#include <stdint.h>
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

struct handoff {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *first, *target;
  int paired;
  int tentative;
  int retiring, entered, retire, release;
};

struct allocator {
  size_t live;
  unsigned failures;
  unsigned calls, fail_at;
  int deny;
};

static struct handoff *handoff;
static LuaWorld *watched;
static __thread unsigned measured, world_locks, polls, entries;
static __thread int role;
static struct {
  lua_State *owner, *collector;
  void *array;
  unsigned collections, finalized;
  int armed;
} resize_probe;

int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
void __real_luaW_enter(lua_State *state);
void __real_luaW_leave(LuaWorld *world, LuaParticipation *participant);
void __real_luaX_poll(lua_State *state);
void __real_luaM_free_(lua_State *state, void *block, size_t size);

void __wrap_luaM_free_(lua_State *state, void *block, size_t size) {
  if (resize_probe.armed && state == resize_probe.owner &&
      block == resize_probe.array) {
    resize_probe.armed = 0;
    state->native_depth++;
    luaW_leave(&G(state)->world, state->admission);
    lua_gc(resize_probe.collector, LUA_GCCOLLECT);
    resize_probe.collections++;
    luaW_enter(state);
    state->native_depth--;
  }
  __real_luaM_free_(state, block, size);
}
void __real_luaQ_wait(lua_State *state, pthread_mutex_t *mutex,
                      pthread_cond_t *changed, LuaWaiter **waiters);

void __wrap_luaQ_wait(lua_State *state, pthread_mutex_t *mutex,
                      pthread_cond_t *changed, LuaWaiter **waiters) {
  if (role == 3 && mutex == &G(handoff->target)->world.mutex) {
    require(pthread_mutex_lock(&handoff->mutex) == 0);
    handoff->entered = 1;
    require(pthread_cond_broadcast(&handoff->changed) == 0);
    require(pthread_mutex_unlock(&handoff->mutex) == 0);
  }
  __real_luaQ_wait(state, mutex, changed, waiters);
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (measured && watched != NULL && mutex == &watched->mutex)
    world_locks++;
  return __real_pthread_mutex_lock(mutex);
}

void __wrap_luaW_enter(lua_State *state) {
  measured++;
  entries++;
  __real_luaW_enter(state);
  measured--;
  if (role == 2) {
    require(pthread_mutex_lock(&handoff->mutex) == 0);
    handoff->entered = 1;
    require(pthread_cond_broadcast(&handoff->changed) == 0);
    while (!handoff->release)
      require(pthread_cond_wait(&handoff->changed, &handoff->mutex) == 0);
    require(pthread_mutex_unlock(&handoff->mutex) == 0);
  }
}

void __wrap_luaW_leave(LuaWorld *world, LuaParticipation *participant) {
  if (role == 1) {
    require(pthread_mutex_lock(&handoff->mutex) == 0);
    handoff->retiring = 1;
    require(pthread_cond_broadcast(&handoff->changed) == 0);
    while (!handoff->retire)
      require(pthread_cond_wait(&handoff->changed, &handoff->mutex) == 0);
    require(pthread_mutex_unlock(&handoff->mutex) == 0);
  }
  measured++;
  __real_luaW_leave(world, participant);
  measured--;
}

void __wrap_luaX_poll(lua_State *state) {
  measured++;
  polls++;
  __real_luaX_poll(state);
  measured--;
}

static void execute(lua_State *state, const char *source) {
  int status = luaL_dostring(state, source);
  if (status != LUA_OK)
    fprintf(stderr, "publication: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
}

static void no_requests(lua_State *state) {
  LuaExecution execution = {0};
  LuaTaskRoot task = {0};
  NativeStackContext context = {0};
  NativeStackContext *previous;
  lua_State *thread;
  int i, status, results;
  lua_gc(state, LUA_GCSTOP);
  watched = &G(state)->world;
  entries = polls = world_locks = 0;
  for (i = 0; i < 40; i++) {
    lua_pushinteger(state, i);
    require(lua_gettop(state) == 1);
    lua_pop(state, 1);
  }
  thread = lua_newthread(state);
  require(luaL_loadstring(thread,
                          "local n=0; for i=1,2000 do n=n+i end; return n") ==
          LUA_OK);
  require(luaTK_admit(thread, &task, &execution) == LUA_OK);
  context.execution = &execution;
  previous = luaE_setnativestackcontext(&context);
  status = luaTK_resume(&task, 0, &results);
  require(status == LUA_OK && results == 1);
  require(luaTK_finish(&task, &status, &results) == LUA_OK && status == LUA_OK);
  luaE_setnativestackcontext(previous);
  require(luaTK_collect(state, &task, &status, &results) == LUA_OK);
  require(lua_tointeger(state, -1) == 2001000);
  require(entries > 40 && polls > 0 && world_locks == 0);
  printf(
      "request-free admission/polling: %u entries, %u polls, %u world locks\n",
      entries, polls, world_locks);
  watched = NULL;
}

static void intern_hits(lua_State *state) {
  static const char key[] = "independent-string-partition";
  static const char long_key[] =
      "a-long-cached-string-with-the-same-public-api-identity-on-each-hit";
  const char *short_value, *long_value;
  lua_State *other;
  unsigned i;
  lua_gc(state, LUA_GCSTOP);
  short_value = lua_pushlstring(state, key, sizeof(key) - 1);
  long_value = lua_pushstring(state, long_key);
  other = lua_newthread(state);
  require(lua_pushlstring(other, key, sizeof(key) - 1) == short_value);
  watched = &G(state)->world;
  world_locks = 0;
  measured++;
  for (i = 0; i < 40; i++) {
    require(lua_pushlstring(state, key, sizeof(key) - 1) == short_value);
    lua_pop(state, 1);
    require(lua_pushstring(state, long_key) == long_value);
    lua_pop(state, 1);
    require(lua_pushlstring(other, key, sizeof(key) - 1) == short_value);
    lua_pop(other, 1);
  }
  measured--;
  require(world_locks == 0);
  watched = NULL;
}

static void observe_warning(void *context, const char *message,
                            int continuing) {
  unsigned *count = context;
  require(strcmp(message, "sample") == 0 && continuing == 0);
  (*count)++;
}

static int unused_binding(lua_State *state) {
  (void)state;
  return 0;
}

static void ordinary_operations(lua_State *state) {
  unsigned warnings = 0, bin, i;
  lua_CFunction panic;
  void *blocks[4];
  lua_gc(state, LUA_GCSTOP);
  lua_newtable(state); /* selected metatable, rooted for the whole check */
  luaX_lockstate(state);
  for (bin = 0; bin < LUA_ALLOC_BINS; bin++) {
    size_t size = (size_t)16 << bin;
    for (i = 0; i < 4; i++)
      blocks[i] = luaM_malloc_(state, size, 0);
    for (i = 0; i < 4; i++)
      luaM_free_(state, blocks[i], size);
  }
  luaW_stop(state);
  luaE_setdebt(G(state),
               65536); /* measure accesses, independently of a GC request */
  luaW_start(state);
  luaX_unlockstate(state);
  watched = &G(state)->world;
  world_locks = 0;
  measured++;
  lua_newtable(state);
  lua_pushvalue(state, 1);
  lua_setmetatable(state, -2);
  require(lua_getmetatable(state, -1));
  lua_pop(state, 2);
  lua_newuserdatauv(state, 16, 0);
  lua_pop(state, 1);
  lua_pushinteger(state, 7);
  lua_pushcclosure(state, unused_binding, 1);
  lua_pop(state, 1);
  lua_copy(state, LUA_REGISTRYINDEX, LUA_REGISTRYINDEX);
  lua_pushinteger(state, 0);
  lua_pushvalue(state, 1);
  lua_setmetatable(state, -2);
  require(lua_getmetatable(state, -1));
  lua_pop(state, 1);
  lua_pushnil(state);
  lua_setmetatable(state, -2);
  lua_pop(state, 1);
  panic = lua_atpanic(state, unused_binding);
  lua_atpanic(state, panic);
  lua_setwarnf(state, observe_warning, &warnings);
  lua_warning(state, "sample", 0);
  lua_setwarnf(state, NULL, NULL);
  measured--;
  require(warnings == 1 && world_locks == 0);
  watched = NULL;
}

static void *old_owner(void *context) {
  struct handoff *fixture = context;
  role = 1;
  require(lua_gettop(fixture->target) == 0);
  return NULL;
}

static void *new_owner(void *context) {
  struct handoff *fixture = context;
  role = 2;
  if (fixture->paired)
    lua_xmove(fixture->first, fixture->target, 0);
  else
    require(lua_gettop(fixture->target) == 0);
  return NULL;
}

static unsigned active(lua_State *state) {
  LuaWorld *world = &G(state)->world;
  unsigned count;
  require(pthread_mutex_lock(&world->mutex) == 0);
  count = luaW_activecount(world);
  require(pthread_mutex_unlock(&world->mutex) == 0);
  return count;
}

/* Hold the departing entry after its physical gate release. The next owner
 * must enter without losing the departing reference or changing its identity
 * when a paired operation borrows another state's participation record. */
static void gate_handoff(lua_State *state, int paired) {
  struct handoff fixture;
  pthread_t old, next;
  lua_State *a = lua_newthread(state), *b = lua_newthread(state);
  memset(&fixture, 0, sizeof(fixture));
  fixture.first = (uintptr_t)a < (uintptr_t)b ? a : b;
  fixture.target = (uintptr_t)a < (uintptr_t)b ? b : a;
  fixture.paired = paired;
  require(pthread_mutex_init(&fixture.mutex, NULL) == 0);
  require(pthread_cond_init(&fixture.changed, NULL) == 0);
  handoff = &fixture;
  require(pthread_create(&old, NULL, old_owner, &fixture) == 0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.retiring)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(pthread_create(&next, NULL, new_owner, &fixture) == 0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.entered)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(active(state) == 2);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  fixture.retire = 1;
  require(pthread_cond_broadcast(&fixture.changed) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(pthread_join(old, NULL) == 0);
  require(active(state) == 1);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  fixture.release = 1;
  require(pthread_cond_broadcast(&fixture.changed) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(pthread_join(next, NULL) == 0);
  require(active(state) == 0);
  handoff = NULL;
  require(pthread_cond_destroy(&fixture.changed) == 0);
  require(pthread_mutex_destroy(&fixture.mutex) == 0);
}

static void *late_entry(void *context) {
  struct handoff *fixture = context;
  role = 3;
  if (fixture->tentative) {
    /* Pause the admission handshake after publishing its reference, before
     * inspecting the collection request or accessing any managed state. */
    LuaWorld *world = &G(fixture->target)->world;
    luaW_activate(fixture->target);
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    fixture->entered = 1;
    require(pthread_cond_broadcast(&fixture->changed) == 0);
    while (!fixture->release)
      require(pthread_cond_wait(&fixture->changed, &fixture->mutex) == 0);
    require(pthread_mutex_unlock(&fixture->mutex) == 0);
    luaW_leave(world, &fixture->target->participation);
  } else
    require(lua_gettop(fixture->target) == 0);
  return NULL;
}

static void late_admission(lua_State *state, int tentative) {
  struct handoff fixture;
  pthread_t thread;
  memset(&fixture, 0, sizeof(fixture));
  fixture.target = lua_newthread(state);
  fixture.tentative = tentative;
  require(pthread_mutex_init(&fixture.mutex, NULL) == 0);
  require(pthread_cond_init(&fixture.changed, NULL) == 0);
  handoff = &fixture;
  luaX_lockstate(state);
  luaW_stop(state);
  require(pthread_create(&thread, NULL, late_entry, &fixture) == 0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.entered)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(fixture.target->native_depth == 0);
  if (tentative)
    require(active(state) == 1);
  else {
    require(active(state) == 0);
    luaC_fullgc(state,
                0); /* the waiting entry cannot mutate the scanned roots */
  }
  luaW_start(state);
  luaX_unlockstate(state);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  fixture.release = 1;
  require(pthread_cond_broadcast(&fixture.changed) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(pthread_join(thread, NULL) == 0);
  require(active(state) == 0);
  handoff = NULL;
  require(pthread_cond_destroy(&fixture.changed) == 0);
  require(pthread_mutex_destroy(&fixture.mutex) == 0);
}

static void *allocate(void *context, void *pointer, size_t old, size_t size) {
  struct allocator *allocator = context;
  void *replacement;
  if (pointer == NULL)
    old = 0;
  if (size == 0) {
    allocator->live -= old;
    free(pointer);
    return NULL;
  }
  allocator->calls++;
  if (allocator->fail_at != 0 && allocator->calls == allocator->fail_at) {
    allocator->failures++;
    return NULL;
  }
  if (allocator->deny && size > old) {
    allocator->failures++;
    return NULL;
  }
  replacement = realloc(pointer, size);
  if (replacement != NULL)
    allocator->live = allocator->live - old + size;
  return replacement;
}

static size_t counted(lua_State *state) {
  size_t kib = (size_t)lua_gc(state, LUA_GCCOUNT);
  return kib * 1024 + (size_t)lua_gc(state, LUA_GCCOUNTB);
}

static int exhausted(lua_State *state) {
  lua_newuserdatauv(state, 4096, 0);
  return 1;
}

static void totals(void) {
  struct allocator allocator = {0};
  lua_State *state = lua_newstate(allocate, &allocator, 0);
  void *block;
  size_t before, populated;
  int i;
  require(state != NULL && lua_checkstack(state, 80));
  require(counted(state) <= allocator.live);
  lua_gc(state, LUA_GCSTOP);
  before = counted(state);
  luaX_lockstate(state);
  block = luaM_malloc_(state, 17, 0);
  luaX_unlockstate(state);
  require(counted(state) == before + 17);
  luaX_lockstate(state);
  block = luaM_realloc_(state, block, 17, 19);
  require(block != NULL);
  luaX_unlockstate(state);
  require(counted(state) == before + 19);
  luaX_lockstate(state);
  block = luaM_realloc_(state, block, 19, 100000);
  require(block != NULL);
  luaX_unlockstate(state);
  require(counted(state) == before + 100000);
  luaX_lockstate(state);
  luaM_free_(state, block, 100000);
  luaX_unlockstate(state);
  require(counted(state) == before);
  for (i = 0; i < 64; i++) {
    lua_createtable(state, 2, 0);
    lua_pushinteger(state, i);
    lua_rawseti(state, -2, 1);
  }
  populated = counted(state);
  require(populated > before && populated <= allocator.live);
  lua_settop(state, 0);
  lua_pushcfunction(state, exhausted);
  allocator.deny = 1;
  require(lua_pcall(state, 0, 1, 0) == LUA_ERRMEM && allocator.failures != 0);
  allocator.deny = 0;
  require(counted(state) <= allocator.live);
  lua_settop(state, 0);
  lua_gc(state, LUA_GCCOLLECT);
  require(counted(state) < populated && counted(state) <= allocator.live);
  lua_close(state);
  require(allocator.live == 0);
}

static void construction(void) {
  unsigned position, failed = 0;
  for (position = 1; position < 64; position++) {
    struct allocator allocator = {0};
    lua_State *state;
    allocator.fail_at = position;
    state = lua_newstate(allocate, &allocator, 0);
    if (state != NULL) {
      require(allocator.failures == 0);
      lua_close(state);
      require(allocator.live == 0 && failed > 1);
      return;
    }
    require(allocator.failures == 1 && allocator.live == 0);
    failed++;
  }
  require(0);
}

struct returned_block {
  lua_State *state;
  void *block;
};

static void *return_from_peer(void *context) {
  struct returned_block *returned = context;
  luaX_lockstate(returned->state);
  luaM_free_(returned->state, returned->block, 64);
  luaX_unlockstate(returned->state);
  return NULL;
}

static void allocation_cache(void) {
  struct allocator allocator = {0};
  struct returned_block returned;
  lua_State *state = lua_newstate(allocate, &allocator, 0);
  pthread_t peer;
  unsigned char *block;
  void *blocks[64];
  size_t peak;
  unsigned i, j;
  require(state != NULL);
  lua_gc(state, LUA_GCSTOP);
  returned.state = lua_newthread(state);
  luaX_lockstate(state);
  block = luaM_malloc_(state, 64, 0);
  luaM_free_(state, block, 64);
  block = luaM_malloc_(state, 96, 0);
  luaM_free_(state, block, 96);
  watched = &G(state)->world;
  world_locks = 0;
  measured++;
  for (i = 0; i < 64; i++) {
    block = luaM_malloc_(state, 64, 0);
    memset(block, (int)i, 64);
    block = luaM_realloc_(state, block, 64, 96);
    require(block != NULL);
    for (j = 0; j < 64; j++)
      require(block[j] == i);
    block = luaM_realloc_(state, block, 96, 32);
    require(block != NULL);
    for (j = 0; j < 32; j++)
      require(block[j] == i);
    luaM_free_(state, block, 32);
  }
  measured--;
  require(world_locks == 0);
  watched = NULL;
  returned.block = luaM_malloc_(state, 64, 0);
  luaX_unlockstate(state);
  require(pthread_create(&peer, NULL, return_from_peer, &returned) == 0);
  require(pthread_join(peer, NULL) == 0);
  luaX_lockstate(state);
  block = luaM_malloc_(state, 64, 0);
  memset(block, 0x5a, 64);
  allocator.deny = 1;
  require(luaM_realloc_(state, block, 64, 4096) == NULL);
  allocator.deny = 0;
  for (j = 0; j < 64; j++)
    require(block[j] == 0x5a);
  luaM_free_(state, block, 64);
  for (i = 0; i < 64; i++)
    blocks[i] = luaM_malloc_(state, 512, 0);
  peak = allocator.live;
  for (i = 0; i < 64; i++)
    luaM_free_(state, blocks[i], 512);
  luaX_unlockstate(state);
  lua_gc(state, LUA_GCCOLLECT);
  require(allocator.live < peak && counted(state) <= allocator.live);
  lua_close(state);
  require(allocator.live == 0);
}

static void graph(lua_State *state, int generational) {
  lua_State *thread;
  int results;
  luaL_openlibs(state);
  if (generational)
    lua_gc(state, LUA_GCGEN);
  lua_gc(state, LUA_GCSTOP);
  execute(state,
          "finalized=0; mt={__gc=function() finalized=finalized+1 end}; "
          "for n=1,130 do local t=setmetatable({},mt); setmetatable(t,mt) end; "
          "collectgarbage('collect'); assert(finalized==130); "
          "local t={}; setmetatable(t,{}); getmetatable(t).__gc=mt.__gc; "
          "t=nil; collectgarbage('collect'); assert(finalized==130)");
  thread = lua_newthread(state);
  require(luaL_loadstring(thread,
                          "local t=setmetatable({},mt); t.self=t; return 42") ==
          LUA_OK);
  require(lua_resume(thread, NULL, 0, &results) == LUA_OK);
  lua_pop(state,
          1); /* its local construction/registration work still matters */
  lua_gc(state, LUA_GCCOLLECT);
  execute(state, "assert(finalized==131)");
}

static int resize_finalizer(lua_State *state) {
  unsigned *value = lua_touserdata(state, 1);
  require(value != NULL && (*value == 1 || *value == 128));
  *value = 0;
  resize_probe.finalized++;
  return 0;
}

static void weak_resize(lua_State *state) {
  lua_State *owner;
  Table *table;
  unsigned i;
  lua_gc(state, LUA_GCSTOP);
  owner = lua_newthread(state);
  lua_createtable(owner, 128, 0);
  lua_newtable(owner);
  lua_pushliteral(owner, "v");
  lua_setfield(owner, -2, "__mode");
  lua_setmetatable(owner, -2);
  for (i = 1; i <= 128; i += 127) {
    unsigned *value = lua_newuserdatauv(owner, sizeof(*value), 0);
    *value = i;
    lua_newtable(owner);
    lua_pushcfunction(owner, resize_finalizer);
    lua_setfield(owner, -2, "__gc");
    lua_setmetatable(owner, -2);
    lua_rawseti(owner, 1, i);
  }
  luaX_lockstate(owner);
  table = hvalue(s2v(owner->top.p - 1));
  resize_probe.owner = owner;
  resize_probe.collector = state;
  resize_probe.array = table->array - table->asize;
  resize_probe.armed = 1;
  luaH_resize(owner, table, 1, 4);
  luaX_unlockstate(owner);
  require(!resize_probe.armed && resize_probe.collections == 1 &&
          resize_probe.finalized == 0);
  for (i = 1; i <= 128; i += 127) {
    unsigned *value;
    require(lua_rawgeti(owner, 1, i) == LUA_TUSERDATA);
    value = lua_touserdata(owner, -1);
    require(value != NULL && *value == i);
    lua_pop(owner, 1);
  }
  lua_settop(owner, 0);
  lua_gc(state, LUA_GCCOLLECT);
  require(resize_probe.finalized == 2);
}

int main(int argc, char **argv) {
  lua_State *state;
  require(argc == 2);
  if (strcmp(argv[1], "construction") == 0) {
    construction();
    return 0;
  }
  if (strcmp(argv[1], "totals") == 0 ||
      strcmp(argv[1], "allocation_cache") == 0) {
    if (strcmp(argv[1], "totals") == 0)
      totals();
    else
      allocation_cache();
    return 0;
  }
  state = luaL_newstate();
  require(state != NULL);
  if (strcmp(argv[1], "no_requests") == 0)
    no_requests(state);
  else if (strcmp(argv[1], "intern_hits") == 0)
    intern_hits(state);
  else if (strcmp(argv[1], "ordinary_operations") == 0)
    ordinary_operations(state);
  else if (strcmp(argv[1], "handoff") == 0 ||
           strcmp(argv[1], "paired_handoff") == 0)
    gate_handoff(state, strcmp(argv[1], "paired_handoff") == 0);
  else if (strcmp(argv[1], "late") == 0 || strcmp(argv[1], "waiting") == 0)
    late_admission(state, strcmp(argv[1], "late") == 0);
  else if (strcmp(argv[1], "weak_resize") == 0)
    weak_resize(state);
  else {
    require(strcmp(argv[1], "incremental") == 0 ||
            strcmp(argv[1], "generational") == 0);
    graph(state, strcmp(argv[1], "generational") == 0);
  }
  lua_close(state);
  return 0;
}
