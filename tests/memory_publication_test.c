#include <laccess.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lgc.h>
#include <lmem.h>
#include <lmempage.h>
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
  size_t cap, max_request;
  unsigned failures;
  unsigned calls, fail_at;
  size_t denied_request;
  unsigned denied_requests, small_fallback;
  int deny;
  lua_State *inspect_stack;
  StkId old_stack;
  unsigned prepared_stack, released_stack;
};

struct metadata_handoff {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_cond_t *first_condition;
  lua_State *states[2];
  unsigned waits[2], order[2], count;
  int grant, first_returned, release_first, second_arrived, collect, priority;
};
static struct metadata_handoff *metadata;
static lua_State *frame_refill_probe;

static struct handoff *handoff;
static LuaWorld *watched;
static pthread_mutex_t *watched_gate;
static __thread unsigned measured, world_locks, polls, entries;
static __thread unsigned stack_locks;
static __thread int role;
static struct {
  lua_State *owner, *collector;
  unsigned collections, finalized;
  int armed;
} resize_probe;

int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
void __real_luaW_enter(lua_State *state);
uintptr_t __real_luaX_valueenter(lua_State *state, int stackonly);
__typeof__(luaW_leave) __real_luaW_leave;
void __real_luaX_poll(lua_State *state);
void *__real_luaM_realloc_(lua_State *state, void *block, size_t old,
                           size_t size);
__typeof__(luaAL_enter) __real_luaAL_enter, __wrap_luaAL_enter;

int __wrap_luaAL_enter(lua_State *state) {
  if (state == frame_refill_probe) {
    CallInfo *frame;
    /* Populate the cache after the outer refill chose to seek admission.
     * This models another task using its slot while that admission parks. */
    frame_refill_probe = NULL;
    frame = luaM_allocframes(state, sizeof(CallInfo));
    require(frame != NULL);
    luaM_free_(state, frame, sizeof(*frame));
  }
  return __real_luaAL_enter(state);
}

void *__wrap_luaM_realloc_(lua_State *state, void *oldblock, size_t old,
                           size_t size) {
  void *block = __real_luaM_realloc_(state, oldblock, old, size);
  if (resize_probe.armed && state == resize_probe.owner && oldblock == NULL &&
      size == sizeof(Value) + 1 + sizeof(unsigned)) {
    resize_probe.armed = 0;
    state->native_depth++;
    require(pthread_mutex_lock(&G(state)->world.mutex) == 0);
    luaW_deactivate(state);
    require(pthread_mutex_unlock(&G(state)->world.mutex) == 0);
    lua_gc(resize_probe.collector, LUA_GCCOLLECT);
    resize_probe.collections++;
    require(pthread_mutex_lock(&G(state)->world.mutex) == 0);
    luaW_resume(state);
    require(pthread_mutex_unlock(&G(state)->world.mutex) == 0);
    luaW_enter(state);
    state->native_depth--;
  }
  return block;
}
void __real_luaQ_wait(lua_State *state, pthread_mutex_t *mutex,
                      pthread_cond_t *changed, LuaWaiter **waiters);

void __wrap_luaQ_wait(lua_State *state, pthread_mutex_t *mutex,
                      pthread_cond_t *changed, LuaWaiter **waiters) {
  struct metadata_handoff *fixture = metadata;
  int index = role - 4;
  if (fixture != NULL && index >= 0 && index < 2 &&
      mutex == &G(state)->world.mutex) {
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    fixture->waits[index]++;
    if (index == 0)
      fixture->first_condition = changed;
    else
      fixture->second_arrived = 1;
    require(pthread_cond_broadcast(&fixture->changed) == 0);
    require(pthread_mutex_unlock(&fixture->mutex) == 0);
  }
  if (role == 3 && mutex == &G(handoff->target)->world.mutex) {
    require(pthread_mutex_lock(&handoff->mutex) == 0);
    handoff->entered = 1;
    require(pthread_cond_broadcast(&handoff->changed) == 0);
    require(pthread_mutex_unlock(&handoff->mutex) == 0);
  }
  __real_luaQ_wait(state, mutex, changed, waiters);
  if (fixture != NULL && index == 0 && mutex == &G(state)->world.mutex) {
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    if (fixture->grant && !fixture->first_returned) {
      /* Delay the selected recipient before it tests ownership. A fresh
       * entrant must not take the grant while this recipient is delayed. */
      fixture->first_returned = 1;
      require(pthread_cond_broadcast(&fixture->changed) == 0);
      require(pthread_mutex_unlock(mutex) == 0);
      while (!fixture->release_first)
        require(pthread_cond_wait(&fixture->changed, &fixture->mutex) == 0);
      require(pthread_mutex_unlock(&fixture->mutex) == 0);
      require(pthread_mutex_lock(mutex) == 0);
    } else
      require(pthread_mutex_unlock(&fixture->mutex) == 0);
  }
}

int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex) {
  if (measured && mutex == watched_gate)
    stack_locks++;
  if (measured && watched != NULL && mutex == &watched->mutex)
    world_locks++;
  return __real_pthread_mutex_lock(mutex);
}

static void hold_handoff(void) {
  require(pthread_mutex_lock(&handoff->mutex) == 0);
  handoff->entered = 1;
  require(pthread_cond_broadcast(&handoff->changed) == 0);
  while (!handoff->release)
    require(pthread_cond_wait(&handoff->changed, &handoff->mutex) == 0);
  require(pthread_mutex_unlock(&handoff->mutex) == 0);
}

void __wrap_luaW_enter(lua_State *state) {
  measured++;
  entries++;
  __real_luaW_enter(state);
  measured--;
  if (role == 2 && handoff->paired)
    hold_handoff();
}

uintptr_t __wrap_luaX_valueenter(lua_State *state, int stackonly) {
  uintptr_t owner = __real_luaX_valueenter(state, stackonly);
  if (role == 2 && !handoff->paired)
    hold_handoff();
  return owner;
}

void __wrap_luaW_leave(global_State *g, uintptr_t stack, unsigned flags,
                       uint64_t control) {
  if (role == 1) {
    require(pthread_mutex_lock(&handoff->mutex) == 0);
    handoff->retiring = 1;
    require(pthread_cond_broadcast(&handoff->changed) == 0);
    while (!handoff->retire)
      require(pthread_cond_wait(&handoff->changed, &handoff->mutex) == 0);
    require(pthread_mutex_unlock(&handoff->mutex) == 0);
  }
  measured++;
  __real_luaW_leave(g, stack, flags, control);
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
  require(entries == 0 && world_locks == 0);
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
  require(entries > 0 && polls > 0 && world_locks == 0);
  printf(
      "request-free admission/polling: %u entries, %u polls, %u world locks\n",
      entries, polls, world_locks);
  watched = NULL;
}

static void stack_admission(lua_State *state) {
  unsigned i;
  lua_gc(state, LUA_GCSTOP);
  watched_gate = &G(state)->world.mutex;
  stack_locks = 0;
  measured++;
  for (i = 0; i < 100; i++) {
    lua_pushinteger(state, i);
    require(lua_gettop(state) == 1 && lua_tointeger(state, -1) == i);
    lua_pop(state, 1);
  }
  measured--;
  fprintf(stderr, "uncontended stack admission: %u OS mutex locks\n",
          stack_locks);
  require(stack_locks == 0);
  watched_gate = NULL;
}

static void allocation_admission(lua_State *state) {
  unsigned admissions;
  lua_gc(state, LUA_GCSTOP);
  require(luaL_loadstring(state,
                          "local last; for i=1,2000 do local t={i}; "
                          "local function f() return t[1] end; "
                          "local s='value'..i; last=f()+#s end; return last") ==
          LUA_OK);
  entries = 0;
  require(lua_pcall(state, 0, 1, 0) == LUA_OK);
  admissions = entries;
  require(lua_tointeger(state, -1) == 2009);
  fprintf(stderr, "allocation-only stack admissions: %u\n", admissions);
  require(admissions == 1); /* Allocation is not a new coroutine entry. */
}

static void *stack_increment(void *context) {
  lua_State *state = context;
  unsigned i;
  for (i = 0; i < 1000; i++) {
    luaX_lockstate(state);
    setivalue(s2v(state->top.p - 1), ivalue(s2v(state->top.p - 1)) + 1);
    luaX_unlockstate(state);
  }
  return NULL;
}

static void stack_contention(lua_State *state) {
  pthread_t peers[4];
  unsigned i;
  lua_pushinteger(state, 0);
  for (i = 0; i < 4; i++)
    require(pthread_create(&peers[i], NULL, stack_increment, state) == 0);
  for (i = 0; i < 4; i++)
    require(pthread_join(peers[i], NULL) == 0);
  require(lua_gettop(state) == 1 && lua_tointeger(state, -1) == 4000);
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

/* Hold the departing notification after its combined gate/admission release.
 * The next owner must retain its single admission and stable identity
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
  /* A root check may remain pending after the gate itself is gone. */
  __atomic_fetch_or(&G(state)->world.control, LUA_WORLD_STACK_ROOTS,
                    __ATOMIC_SEQ_CST);
  require(pthread_create(&old, NULL, old_owner, &fixture) == 0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.retiring)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  __atomic_fetch_and(&G(state)->world.control, ~(uint64_t)LUA_WORLD_STACK_ROOTS,
                     __ATOMIC_SEQ_CST);
  require(pthread_create(&next, NULL, new_owner, &fixture) == 0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.entered)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(active(state) == 1);
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
    require(luaX_tryreserve(fixture->target) == 1);
    luaW_activate(fixture->target);
    require(pthread_mutex_lock(&fixture->mutex) == 0);
    fixture->entered = 1;
    require(pthread_cond_broadcast(&fixture->changed) == 0);
    while (!fixture->release)
      require(pthread_cond_wait(&fixture->changed, &fixture->mutex) == 0);
    require(pthread_mutex_unlock(&fixture->mutex) == 0);
    luaX_endreserve(fixture->target, 1);
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
  if (allocator->inspect_stack != NULL) {
    lua_State *L = allocator->inspect_stack;
    require(L->top.p == L->stack.p + 11);
    require(L->ci->func.p == L->stack.p);
    require(ttistable(s2v(L->stack.p + 10)));
    if (size != 0) {
      require(pointer == NULL && L->stack.p == allocator->old_stack);
      allocator->prepared_stack++;
    } else {
      require(L->stack.p != allocator->old_stack);
      allocator->released_stack++;
    }
  }
  if (pointer == NULL)
    old = 0;
  require(old <= allocator->live);
  if (size == 0) {
    allocator->live -= old;
    free(pointer);
    return NULL;
  }
  allocator->calls++;
  if (size > allocator->max_request)
    allocator->max_request = size;
  if (allocator->cap != 0 && size > allocator->cap - (allocator->live - old)) {
    allocator->failures++;
    if (size > allocator->denied_request)
      allocator->denied_request = size;
    allocator->denied_requests++;
    return NULL;
  }
  if (allocator->fail_at != 0 && allocator->calls == allocator->fail_at) {
    allocator->failures++;
    return NULL;
  }
  if (allocator->deny && size > old) {
    allocator->failures++;
    return NULL;
  }
  replacement = realloc(pointer, size);
  if (replacement != NULL) {
    allocator->live = allocator->live - old + size;
    if (allocator->denied_request != 0 && size < allocator->denied_request)
      allocator->small_fallback++;
  }
  return replacement;
}

static void page_growth(int limited) {
  struct allocator allocator = {0};
  lua_State *state = lua_newstate(allocate, &allocator, 0);
  unsigned char **blocks = calloc(1500, sizeof(*blocks));
  size_t base;
  unsigned count, i, j;
  require(state != NULL && blocks != NULL);
  lua_gc(state, LUA_GCSTOP);
  base = allocator.live;
  allocator.calls = 0;
  allocator.max_request = 0;
  if (limited)
    allocator.cap = base + 4096;
  luaX_lockstate(state);
  for (count = 0; count < 1500; count++) {
    blocks[count] = luaM_realloc_(state, NULL, 0, 64);
    if (blocks[count] == NULL)
      break;
    memset(blocks[count], (int)count, 64);
  }
  if (limited) {
    require(count < 1500 && allocator.live <= allocator.cap);
    require(allocator.denied_requests != 0 && allocator.small_fallback != 0);
  } else {
    fprintf(stderr, "pooled backing allocations: %u calls, largest %zu\n",
            allocator.calls, allocator.max_request);
    require(count == 1500 && allocator.calls < 20);
    require(allocator.max_request > 4096 && allocator.max_request <= 65536);
    require(allocator.live <= base + 262144);
  }
  for (i = 0; i < count; i++) {
    for (j = 0; j < 64; j++)
      require(blocks[i][j] == (unsigned char)i);
    luaM_free_(state, blocks[i], 64);
  }
  luaX_unlockstate(state);
  allocator.cap = 0;
  lua_close(state);
  require(allocator.live == 0);
  free(blocks);
}

static void page_isolation(void) {
  struct allocator allocator = {0};
  lua_State *state = lua_newstate(allocate, &allocator, 0);
  void **objects = calloc(1500, sizeof(*objects));
  unsigned char *frame = NULL;
  size_t base, frame_size = sizeof(CallInfo);
  unsigned i;
  require(state != NULL && objects != NULL);
  lua_gc(state, LUA_GCSTOP);
  base = allocator.live;
  luaX_lockstate(state);
  for (i = 0; i < 1500; i++) {
    objects[i] = luaM_malloc_(state, frame_size, LUA_TTABLE);
    if (i == 1000) {
      frame = (unsigned char *)luaM_allocframes(state, sizeof(CallInfo));
      require(frame != NULL);
      memset(frame, 0x5a, frame_size);
    }
  }
  for (i = 0; i < 1500; i++)
    luaM_free_(state, objects[i], frame_size);
  luaX_unlockstate(state);
  lua_gc(state, LUA_GCCOLLECT);
  fprintf(stderr, "retained pool bytes with one raw frame: %zu\n",
          allocator.live > base ? allocator.live - base : 0);
  require(allocator.live <= base + 16384);
  luaX_lockstate(state);
  for (i = 0; i < frame_size; i++)
    require(frame[i] == 0x5a);
  luaM_free_(state, frame, frame_size);
  luaX_unlockstate(state);
  lua_close(state);
  require(allocator.live == 0);
  free(objects);
}

static void refill_recheck(void) {
  struct allocator allocator = {0};
  lua_State *state = lua_newstate(allocate, &allocator, 0);
  CallInfo *frame;
  require(state != NULL && state->call_bytes == 0);
  lua_gc(state, LUA_GCCOLLECT); /* no retained frames: the frame pool is cold */
  allocator.cap = allocator.live + 4096;
  allocator.failures = 0;
  luaX_lockstate(state);
  frame_refill_probe = state;
  frame = luaM_allocframes(state, sizeof(CallInfo));
  require(frame != NULL && frame_refill_probe == NULL);
  require(allocator.failures == 0);
  luaM_free_(state, frame, sizeof(*frame));
  luaX_unlockstate(state);
  allocator.cap = 0;
  lua_close(state);
  require(allocator.live == 0);
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
      lua_close(state);
      require(allocator.live == 0 && allocator.failures <= 1);
      if (allocator.failures == 0) {
        require(failed > 1);
        return;
      }
      /* Optional page growth can fail while the minimum refill succeeds.
       * Continue injecting later call sites instead of stopping here. */
      continue;
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

static void *stale_step(void *context) {
  lua_State *state = context;
  luaX_lockstate(state);
  luaC_step(state);
  luaX_unlockstate(state);
  return NULL;
}

static void coalesced_gc(lua_State *state) {
  lua_State *waiter;
  void *block;
  LuaWorld *world = &G(state)->world;
  pthread_t peer;
  execute(state, "keep={}; for i=1,4000 do keep[i]={i} end");
  lua_gc(state, LUA_GCINC);
  lua_gc(state, LUA_GCCOLLECT);
  waiter = lua_newthread(state);
  luaX_lockstate(state);
  luaW_heapenter(state);
  luaE_setdebt(G(state), 0);
  require(pthread_create(&peer, NULL, stale_step, waiter) == 0);
  require(pthread_mutex_lock(&world->mutex) == 0);
  while (waiter->heap_waiting == 0)
    luaQ_wait(state, &world->mutex, &world->changed, &world->waiters);
  require(pthread_mutex_unlock(&world->mutex) == 0);
  /* Service the request while the other mutator waits for metadata access. */
  luaC_fullgc(state, 0);
  require(G(state)->gcstate == GCSpause && luaE_getdebt(G(state)) > 64);
  luaW_heapleave(state);
  luaX_unlockstate(state);
  require(pthread_join(peer, NULL) == 0);
  luaX_lockstate(state);
  fprintf(stderr, "collector state after a stale request: %u\n",
          G(state)->gcstate);
  require(G(state)->gcstate == GCSpause);
  luaX_unlockstate(state);
  /* Explicit zero-byte steps still force work, even with positive debt. */
  lua_gc(state, LUA_GCSTEP, (size_t)0);
  luaX_lockstate(state);
  require(G(state)->gcstate != GCSpause);
  luaX_unlockstate(state);
  lua_gc(state, LUA_GCCOLLECT);
  lua_gc(state, LUA_GCSTEP, (size_t)64);
  luaX_lockstate(state);
  require(G(state)->gcstate == GCSpause);
  luaE_setdebt(G(state), 128);
  block = luaM_malloc_(state, 256, 0);
  require(luaE_getdebt(G(state)) > 0 &&
          luaE_getdebt(G(state)) <= -state->allocation_debt);
  /* Unpublished local allocations still require an automatic step. */
  luaC_checkGC(state);
  require(G(state)->gcstate != GCSpause);
  luaM_free_(state, block, 256);
  luaX_unlockstate(state);
}

static void marking_cache(lua_State *state) {
  void *block;
  lua_gc(state, LUA_GCSTOP);
  execute(state, "keep={}; for i=1,400 do keep[i]={i} end");
  lua_gc(state, LUA_GCCOLLECT);
  lua_gc(state, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  lua_gc(state, LUA_GCPARAM, LUA_GCPSTEPMUL, 100);
  luaX_lockstate(state);
  block = luaM_malloc_(state, 64, 0);
  luaM_free_(state, block, 64);
  luaX_unlockstate(state);
  luaX_lockstate(state);
  luaW_stop(state);
  luaC_runtilstate(state, GCSpropagate, 0);
  luaW_start(state);
  require(luaC_tracing(state));
  watched = &G(state)->world;
  world_locks = 0;
  measured++;
  block = luaM_malloc_(state, 64, 0);
  luaM_free_(state, block, 64);
  measured--;
  fprintf(stderr, "allocation after marking: %u world locks\n", world_locks);
  require(world_locks == 0);
  watched = NULL;
  luaX_unlockstate(state);
  lua_gc(state, LUA_GCCOLLECT);
}

/* A full cache must immediately reuse its owner's returned block, including
 * during marking. Retiring an empty cache must still return its backing bytes.
 */
static void local_returns(void) {
  struct allocator allocator = {0};
  lua_State *state = lua_newstate(allocate, &allocator, 0);
  void **blocks = calloc(2048, sizeof(*blocks));
  LuaAllocPage *page;
  void *returned, *again;
  size_t baseline, count = 0;
  unsigned i;
  require(state != NULL && blocks != NULL);
  lua_gc(state, LUA_GCSTOP);
  lua_gc(state, LUA_GCCOLLECT);
  baseline = allocator.live;
  luaX_lockstate(state);
  blocks[count++] = luaM_allocframes(state, sizeof(CallInfo));
  require(blocks[0] != NULL);
  page = luaMP_page((LuaAllocBlock *)blocks[0] - 1);
  require(page != NULL);
  while (page->local != NULL) {
    require(count < 2048);
    blocks[count++] = luaM_allocframes(state, sizeof(CallInfo));
    require(blocks[count - 1] != NULL);
  }
  require(count >= 2);
  returned = blocks[--count];
  luaM_free_(state, returned, sizeof(CallInfo));
  luaW_stop(state);
  luaC_runtilstate(state, GCSpropagate, 0);
  luaW_start(state);
  require(luaC_tracing(state));
  allocator.cap = allocator.live;
  watched = &G(state)->world;
  world_locks = 0;
  measured++;
  for (i = 0; i < 1000; i++) {
    again = luaM_allocframes(state, sizeof(CallInfo));
    require(again == returned);
    memset(again, 0x5a, sizeof(CallInfo));
    luaM_free_(state, again, sizeof(CallInfo));
  }
  measured--;
  require(world_locks == 0);
  require(allocator.failures == 0);
  watched = NULL;
  allocator.cap = 0;
  while (count != 0)
    luaM_free_(state, blocks[--count], sizeof(CallInfo));
  luaX_unlockstate(state);
  lua_gc(state, LUA_GCCOLLECT);
  require(allocator.live <= baseline);
  lua_close(state);
  require(allocator.live == 0);
  free(blocks);
}

static void grow_stack(lua_State *state, void *context) {
  require(luaD_growstack(state, *(int *)context, 1));
}

static void stack_retry(void) {
  struct allocator allocator = {0};
  lua_State *state = lua_newstate(allocate, &allocator, 0);
  int requested = 512, status, i;
  require(state != NULL);
  lua_gc(state, LUA_GCSTOP);
  for (i = 1; i <= 10; i++) {
    lua_newtable(state);
    lua_pushinteger(state, i);
    lua_rawseti(state, -2, 1);
  }
  lua_gc(state, LUA_GCCOLLECT);
  lua_newuserdatauv(state, 65536, 0);
  lua_pop(state, 1); /* recoverable garbage fills the physical heap budget */
  allocator.cap = allocator.live;
  allocator.failures = 0;
  luaX_lockstate(state);
  status = luaD_rawrunprotected(state, grow_stack, &requested);
  luaX_unlockstate(state);
  require(status == LUA_OK && allocator.failures != 0);
  require(lua_gettop(state) == 10);
  for (i = 1; i <= 10; i++) {
    lua_rawgeti(state, i, 1);
    require(lua_tointeger(state, -1) == i);
    lua_pop(state, 1);
  }
  allocator.cap = allocator.live;
  allocator.failures = 0;
  require(!lua_checkstack(state, 32768));
  require(allocator.failures == 1 && lua_gettop(state) == 10);
  allocator.failures = 0;
  requested = 32768;
  luaX_lockstate(state);
  status = luaD_rawrunprotected(state, grow_stack, &requested);
  require(!G(state)->gcstopem && !G(state)->gcemergency);
  luaX_unlockstate(state);
  require(status == LUA_ERRMEM && allocator.failures == 2);
  require(lua_gettop(state) == 10);
  allocator.cap = 0;
  allocator.inspect_stack = state;
  allocator.old_stack = state->stack.p;
  require(lua_checkstack(state, 1024));
  allocator.inspect_stack = NULL;
  require(allocator.prepared_stack == 1 && allocator.released_stack == 1);
  lua_close(state);
  require(allocator.live == 0);
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

static void weak_resize(lua_State *state, int metatable) {
  lua_State *owner;
  Table *table;
  unsigned i;
  lua_gc(state, LUA_GCSTOP);
  owner = lua_newthread(state);
  lua_createtable(owner, 128, 0);
  lua_newtable(owner);
  if (!metatable) {
    lua_pushliteral(owner, "v");
    lua_setfield(owner, -2, "__mode");
  }
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
  if (metatable)
    table = table->metatable;
  resize_probe.owner = owner;
  resize_probe.collector = state;
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

static void metadata_order(struct metadata_handoff *fixture, unsigned index) {
  require(pthread_mutex_lock(&fixture->mutex) == 0);
  require(fixture->count < 2);
  fixture->order[fixture->count++] = index;
  if (index == 1)
    fixture->second_arrived = 1;
  require(pthread_cond_broadcast(&fixture->changed) == 0);
  require(pthread_mutex_unlock(&fixture->mutex) == 0);
}

static void *metadata_worker(void *context) {
  unsigned index = (unsigned)(uintptr_t)context;
  struct metadata_handoff *fixture = metadata;
  lua_State *state = fixture->states[index];
  role = (int)index + 4;
  if (index == 1 && fixture->collect) {
    require(lua_gc(state, LUA_GCCOLLECT) == 0);
    metadata_order(fixture, index);
  } else {
    luaX_lockstate(state);
    if (index == 1 && fixture->priority)
      luaW_collectenter(state);
    else
      luaW_heapenter(state);
    metadata_order(fixture, index);
    luaW_heapleave(state);
    luaX_unlockstate(state);
  }
  return NULL;
}

static void metadata_handoff(lua_State *state, int spurious, int collect,
                             int priority) {
  struct metadata_handoff fixture = {0};
  pthread_t first, second;
  int bypassed;
  require(pthread_mutex_init(&fixture.mutex, NULL) == 0);
  require(pthread_cond_init(&fixture.changed, NULL) == 0);
  fixture.states[0] = lua_newthread(state);
  fixture.states[1] = lua_newthread(state);
  fixture.collect = collect;
  fixture.priority = priority;
  metadata = &fixture;
  luaX_lockstate(state);
  luaW_heapenter(state);
  require(pthread_create(&first, NULL, metadata_worker, (void *)(uintptr_t)0) ==
          0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (fixture.waits[0] == 0)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  if (priority) {
    /* Queue the collector behind an ordinary waiter before granting either.
     * Its admission must precede the refill, without a timing assumption. */
    require(pthread_create(&second, NULL, metadata_worker,
                           (void *)(uintptr_t)1) == 0);
    require(pthread_mutex_lock(&fixture.mutex) == 0);
    while (!fixture.second_arrived)
      require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
    require(pthread_mutex_unlock(&fixture.mutex) == 0);
    luaW_heapleave(state);
    luaX_unlockstate(state);
    require(pthread_join(first, NULL) == 0 && pthread_join(second, NULL) == 0);
    require(fixture.count == 2 && fixture.order[0] == 1 &&
            fixture.order[1] == 0);
    metadata = NULL;
    require(pthread_cond_destroy(&fixture.changed) == 0);
    require(pthread_mutex_destroy(&fixture.mutex) == 0);
    return;
  }
  if (spurious) {
    require(pthread_mutex_lock(&G(state)->world.mutex) == 0);
    require(pthread_cond_broadcast(fixture.first_condition) == 0);
    require(pthread_mutex_unlock(&G(state)->world.mutex) == 0);
    require(pthread_mutex_lock(&fixture.mutex) == 0);
    while (fixture.waits[0] < 2)
      require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
    require(pthread_mutex_unlock(&fixture.mutex) == 0);
  }
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  fixture.grant = 1;
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  luaW_heapleave(state);
  luaX_unlockstate(state);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.first_returned)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(pthread_create(&second, NULL, metadata_worker,
                         (void *)(uintptr_t)1) == 0);
  require(pthread_mutex_lock(&fixture.mutex) == 0);
  while (!fixture.second_arrived)
    require(pthread_cond_wait(&fixture.changed, &fixture.mutex) == 0);
  bypassed = fixture.count != 0;
  fixture.release_first = 1;
  require(pthread_cond_broadcast(&fixture.changed) == 0);
  require(pthread_mutex_unlock(&fixture.mutex) == 0);
  require(pthread_join(first, NULL) == 0 && pthread_join(second, NULL) == 0);
  metadata = NULL;
  require(pthread_cond_destroy(&fixture.changed) == 0);
  require(pthread_mutex_destroy(&fixture.mutex) == 0);
  require(!bypassed && fixture.count == 2 && fixture.order[0] == 0 &&
          fixture.order[1] == 1);
}

struct metadata_counter {
  lua_State *state;
  unsigned *value;
};
static void *metadata_increment(void *context) {
  struct metadata_counter *counter = context;
  for (unsigned i = 0; i < 1000; i++) {
    luaX_lockstate(counter->state);
    luaW_heapenter(counter->state);
    ++*counter->value;
    luaW_heapleave(counter->state);
    luaX_unlockstate(counter->state);
  }
  return NULL;
}
static void metadata_contention(lua_State *state) {
  struct metadata_counter counters[4];
  pthread_t peers[4];
  unsigned value = 0, i;
  for (i = 0; i < 4; i++) {
    counters[i].state = lua_newthread(state);
    counters[i].value = &value;
  }
  for (i = 0; i < 4; i++)
    require(pthread_create(&peers[i], NULL, metadata_increment, &counters[i]) ==
            0);
  for (i = 0; i < 4; i++)
    require(pthread_join(peers[i], NULL) == 0);
  require(value == 4000);
}

static void stopped_gc(lua_State *state) {
  execute(state, "keep={}; for i=1,4000 do keep[i]={i} end");
  lua_gc(state, LUA_GCCOLLECT);
  lua_gc(state, LUA_GCSTOP);
  lua_gc(state, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  lua_gc(state, LUA_GCPARAM, LUA_GCPSTEPMUL, 100);
  luaX_lockstate(state);
  luaE_setdebt(G(state), 0);
  watched = &G(state)->world;
  world_locks = 0;
  measured++;
  luaC_checkGC(state);
  measured--;
  require(world_locks == 0 && G(state)->gcstate == GCSpause);
  watched = NULL;
  luaX_unlockstate(state);
  lua_gc(state, LUA_GCSTEP, (size_t)0);
  require(lua_gc(state, LUA_GCISRUNNING) == 0);
  luaX_lockstate(state);
  require(G(state)->gcstate != GCSpause);
  luaX_unlockstate(state);
}

int main(int argc, char **argv) {
  lua_State *state;
  require(argc == 2);
  if (strcmp(argv[1], "construction") == 0) {
    construction();
    return 0;
  }
  if (strcmp(argv[1], "page_isolation") == 0) {
    page_isolation();
    return 0;
  }
  if (strcmp(argv[1], "refill_recheck") == 0) {
    refill_recheck();
    return 0;
  }
  if (strcmp(argv[1], "totals") == 0 ||
      strcmp(argv[1], "allocation_cache") == 0 ||
      strcmp(argv[1], "local_returns") == 0 ||
      strcmp(argv[1], "stack_retry") == 0 ||
      strcmp(argv[1], "page_growth") == 0 || strcmp(argv[1], "page_cap") == 0) {
    if (strcmp(argv[1], "totals") == 0)
      totals();
    else if (strcmp(argv[1], "allocation_cache") == 0)
      allocation_cache();
    else if (strcmp(argv[1], "local_returns") == 0)
      local_returns();
    else if (strcmp(argv[1], "stack_retry") == 0)
      stack_retry();
    else
      page_growth(strcmp(argv[1], "page_cap") == 0);
    return 0;
  }
  state = luaL_newstate();
  require(state != NULL);
  if (strcmp(argv[1], "no_requests") == 0)
    no_requests(state);
  else if (strcmp(argv[1], "stack_admission") == 0)
    stack_admission(state);
  else if (strcmp(argv[1], "allocation_admission") == 0)
    allocation_admission(state);
  else if (strcmp(argv[1], "stack_contention") == 0)
    stack_contention(state);
  else if (strcmp(argv[1], "metadata_contention") == 0)
    metadata_contention(state);
  else if (strcmp(argv[1], "metadata_handoff") == 0 ||
           strcmp(argv[1], "metadata_spurious") == 0 ||
           strcmp(argv[1], "metadata_priority") == 0 ||
           strcmp(argv[1], "metadata_collect") == 0)
    metadata_handoff(state, strcmp(argv[1], "metadata_spurious") == 0,
                     strcmp(argv[1], "metadata_collect") == 0,
                     strcmp(argv[1], "metadata_priority") == 0);
  else if (strcmp(argv[1], "stopped_gc") == 0)
    stopped_gc(state);
  else if (strcmp(argv[1], "intern_hits") == 0)
    intern_hits(state);
  else if (strcmp(argv[1], "ordinary_operations") == 0)
    ordinary_operations(state);
  else if (strcmp(argv[1], "marking_cache") == 0)
    marking_cache(state);
  else if (strcmp(argv[1], "coalesced_gc") == 0)
    coalesced_gc(state);
  else if (strcmp(argv[1], "handoff") == 0 ||
           strcmp(argv[1], "paired_handoff") == 0)
    gate_handoff(state, strcmp(argv[1], "paired_handoff") == 0);
  else if (strcmp(argv[1], "late") == 0 || strcmp(argv[1], "waiting") == 0)
    late_admission(state, strcmp(argv[1], "late") == 0);
  else if (strcmp(argv[1], "weak_resize") == 0 ||
           strcmp(argv[1], "metatable_resize") == 0)
    weak_resize(state, strcmp(argv[1], "metatable_resize") == 0);
  else {
    require(strcmp(argv[1], "incremental") == 0 ||
            strcmp(argv[1], "generational") == 0);
    graph(state, strcmp(argv[1], "generational") == 0);
  }
  lua_close(state);
  return 0;
}
