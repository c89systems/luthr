#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lmem.h>
#include <lmempage.h>
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

enum { BLOCKS = 5000, ACTORS = 4, BORROWED = 16, BYTES = 64 };
struct actor {
  lua_State *state;
  LuaAllocationCache cache;
  void *owner;
  unsigned id, admissions, initializations;
  int frame;
  unsigned char *blocks[BORROWED];
};
static __thread unsigned measured, admissions;
static __thread unsigned initializations;
static struct {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  int armed, entered, release;
} pause_init = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 0};
struct allocator {
  size_t live, cap;
  unsigned denied;
};
void __real_luaW_heapenter(lua_State *);
void __wrap_luaW_heapenter(lua_State *L) {
  if (measured)
    admissions++;
  __real_luaW_heapenter(L);
}

void __real_luaMP_initpage(lua_State *, LuaAllocPage *);
void __wrap_luaMP_initpage(lua_State *L, LuaAllocPage *page) {
  if (measured && !luaW_iscollector(L)) {
    initializations++;
    require(luaW_scope(L).heap == 0);
    require(pthread_mutex_lock(&pause_init.mutex) == 0);
    if (pause_init.armed) {
      pause_init.armed = 0;
      pause_init.entered = 1;
      require(pthread_cond_broadcast(&pause_init.changed) == 0);
      /* Keep participation while initialization is deliberately held. */
      while (!pause_init.release)
        require(pthread_cond_wait(&pause_init.changed, &pause_init.mutex) == 0);
    }
    require(pthread_mutex_unlock(&pause_init.mutex) == 0);
  }
  __real_luaMP_initpage(L, page);
}

static void *allocate(void *context, void *pointer, size_t old, size_t size) {
  struct allocator *allocator = context;
  void *replacement;
  if (pointer == NULL)
    old = 0;
  require(old <= allocator->live);
  if (size == 0) {
    allocator->live -= old;
    free(pointer);
    return NULL;
  }
  if (allocator->cap != 0 && size > allocator->cap - (allocator->live - old)) {
    allocator->denied++;
    return NULL;
  }
  replacement = realloc(pointer, size);
  if (replacement != NULL)
    allocator->live = allocator->live - old + size;
  return replacement;
}

static void *borrow(void *argument) {
  struct actor *actor = argument;
  LuaExecution execution = {0};
  NativeStackContext context = {0};
  NativeStackContext *previous = NULL;
  unsigned i;
  if (actor->cache.global != NULL) {
    void *expected = NULL;
    context.execution = &execution;
    execution.allocation_cache = &actor->cache;
    previous = luaE_setnativestackcontext(&context);
    require(__atomic_compare_exchange_n(&actor->owner, &expected, &execution, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));
  }
  lua_lock(actor->state);
  admissions = 0;
  initializations = 0;
  measured = 1;
  for (i = 0; i < (actor->frame ? 1U : BORROWED); i++) {
    actor->blocks[i] = actor->frame
                           ? (unsigned char *)luaM_newframe(actor->state)
                           : luaM_malloc_(actor->state, BYTES, 0);
    require(actor->blocks[i] != NULL);
    memset(actor->blocks[i], (int)(actor->id * BORROWED + i),
           actor->frame ? sizeof(CallInfo) : BYTES);
  }
  measured = 0;
  actor->admissions = admissions;
  actor->initializations = initializations;
  lua_unlock(actor->state);
  if (actor->cache.global != NULL) {
    __atomic_store_n(&actor->owner, NULL, __ATOMIC_RELEASE);
    luaE_setnativestackcontext(previous);
  }
  return NULL;
}

static void *collect(void *argument) {
  require(lua_gc(argument, LUA_GCCOLLECT) == 0);
  return NULL;
}

static void batch(int capped, int collect_during_init) {
  struct allocator allocator = {0};
  lua_State *L = lua_newstate(allocate, &allocator, 0);
  struct actor actors[ACTORS] = {0};
  unsigned i, j, total = 0, initialized = 0;
  require(L != NULL);
  for (i = 0; i < ACTORS; i++) {
    actors[i].state = lua_newthread(L);
    actors[i].id = i + 1;
    actors[i].frame = 1;
    actors[i].cache.global = G(L);
    actors[i].cache.permission = &actors[i].owner;
    require(actors[i].state->nci == 0);
  }
  lua_gc(L, LUA_GCSTOP);
  lua_lock(L);
  luaW_stop(L);
  for (i = 0; i < ACTORS; i++) {
    actors[i].cache.next = G(L)->slot_caches;
    G(L)->slot_caches = &actors[i].cache;
  }
  luaW_start(L);
  lua_unlock(L);
  lua_gc(L,
         LUA_GCCOLLECT); /* Retire unused frames and start with a cold pool. */
  if (capped) {
    CallInfo *probe;
    size_t capacity;
    lua_lock(L);
    probe = luaM_newframe(L);
    require(probe != NULL);
    capacity = luaMP_page((LuaAllocBlock *)probe - 1)->bytes;
    luaM_free_(L, probe, sizeof(*probe));
    lua_unlock(L);
    lua_gc(L, LUA_GCCOLLECT);
    allocator.cap = allocator.live + capacity; /* exactly one observed page */
  }
  if (collect_during_init) {
    pthread_t first, second, collector;
    size_t base = allocator.live;
    pause_init.armed = 1;
    require(pthread_create(&first, NULL, borrow, &actors[0]) == 0);
    require(pthread_mutex_lock(&pause_init.mutex) == 0);
    while (!pause_init.entered)
      require(pthread_cond_wait(&pause_init.changed, &pause_init.mutex) == 0);
    require(pthread_mutex_unlock(&pause_init.mutex) == 0);
    /* A second backing request can finish while the first batch is private.
     * It must not reserve a second batch for this same allocation pool. */
    require(pthread_create(&second, NULL, borrow, &actors[1]) == 0);
    require(pthread_join(second, NULL) == 0);
    require(allocator.live <= base + 5 * 4096);
    require(pthread_create(&collector, NULL, collect, L) == 0);
    while (!luaW_stopped(&G(L)->world))
      sched_yield();
    require(pthread_mutex_lock(&pause_init.mutex) == 0);
    pause_init.release = 1;
    require(pthread_cond_broadcast(&pause_init.changed) == 0);
    require(pthread_mutex_unlock(&pause_init.mutex) == 0);
    require(pthread_join(first, NULL) == 0);
    require(pthread_join(collector, NULL) == 0);
  }
  for (i = 0; i < ACTORS; i++) {
    if (!collect_during_init || i >= 2)
      borrow(&actors[i]);
    total += actors[i].admissions;
    initialized += actors[i].initializations;
  }
  require(!capped ||
          (allocator.denied != 0 && allocator.live <= allocator.cap));
  allocator.cap = 0;
  lua_lock(L);
  for (i = 0; i < ACTORS; i++) {
    for (j = 0; j < sizeof(CallInfo); j++)
      require(actors[i].blocks[0][j] == actors[i].id * BORROWED);
    luaM_free_(L, actors[i].blocks[0], sizeof(CallInfo));
  }
  lua_unlock(L);
  lua_close(L);
  require(allocator.live == 0);
  require(initialized != 0);
  fprintf(stderr, "cold-pool metadata admissions: %u\n", total);
  require(capped || collect_during_init || total == 0);
}

static void control_lifetime(void) {
  enum { CONTROLS = 32, CONTROL_BYTES = 128 };
  struct allocator allocator = {0};
  unsigned char *controls[CONTROLS], *blocks[BLOCKS];
  lua_State *L = lua_newstate(allocate, &allocator, 0);
  size_t baseline;
  unsigned i, j;
  require(L != NULL);
  lua_gc(L, LUA_GCSTOP);
  lua_gc(L, LUA_GCCOLLECT);
  baseline = allocator.live;
  for (i = 0; i < CONTROLS; i++) {
    lua_lock(L);
    /* Make the ordinary size class hot before retaining a control record.
     * A shared page would retain an entire hot slab for this one record. */
    for (j = 0; j < BLOCKS; j++) {
      blocks[j] = luaM_malloc_(L, CONTROL_BYTES, 0);
      require(blocks[j] != NULL);
    }
    controls[i] = luaM_alloccontrol(L, CONTROL_BYTES);
    require(controls[i] != NULL);
    memset(controls[i], (int)i, CONTROL_BYTES);
    for (j = 0; j < BLOCKS; j++)
      luaM_free_(L, blocks[j], CONTROL_BYTES);
    lua_unlock(L);
    lua_gc(L, LUA_GCCOLLECT);
  }
  require(allocator.live <= baseline + 65536);
  lua_lock(L);
  allocator.cap = allocator.live;
  G(L)->gcstopem = 1;
  require(luaM_alloccontrol(L, CONTROL_BYTES) == NULL && allocator.denied != 0);
  G(L)->gcstopem = 0;
  allocator.cap = 0;
  for (i = 0; i < CONTROLS; i++) {
    for (j = 0; j < CONTROL_BYTES; j++)
      require(controls[i][j] == i);
    luaM_free_(L, controls[i], CONTROL_BYTES);
  }
  lua_unlock(L);
  lua_close(L);
  require(allocator.live == 0);
}

struct return_wave {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *state;
  unsigned char *blocks[BORROWED];
  unsigned ready, returned;
};

static void *return_remote(void *context) {
  struct return_wave *wave = context;
  unsigned round, i, j;
  for (round = 1; round <= 512; round++) {
    require(pthread_mutex_lock(&wave->mutex) == 0);
    while (wave->ready != round)
      require(pthread_cond_wait(&wave->changed, &wave->mutex) == 0);
    require(pthread_mutex_unlock(&wave->mutex) == 0);
    lua_lock(wave->state);
    for (i = 0; i < BORROWED; i++) {
      for (j = 0; j < BYTES; j++)
        require(wave->blocks[i][j] == (unsigned char)round);
      luaM_free_(wave->state, wave->blocks[i], BYTES);
    }
    lua_unlock(wave->state);
    require(pthread_mutex_lock(&wave->mutex) == 0);
    wave->returned = round;
    require(pthread_cond_broadcast(&wave->changed) == 0);
    require(pthread_mutex_unlock(&wave->mutex) == 0);
  }
  return NULL;
}

static void remote_returns(void) {
  struct allocator allocator = {0};
  struct return_wave wave = {0};
  lua_State *L = lua_newstate(allocate, &allocator, 0);
  unsigned char *warm[64], *owned[BORROWED];
  pthread_t freer;
  unsigned round, i;
  require(L != NULL);
  wave.state = lua_newthread(L);
  lua_gc(L, LUA_GCSTOP);
  require(pthread_mutex_init(&wave.mutex, NULL) == 0);
  require(pthread_cond_init(&wave.changed, NULL) == 0);
  lua_lock(L);
  /* Grow this owner's size class once, then deny any further backing memory.
   * Remote returns must sustain both simultaneous waves without collecting. */
  for (i = 0; i < 64; i++) {
    warm[i] = luaM_malloc_(L, BYTES, 0);
    require(warm[i] != NULL);
  }
  for (i = 0; i < 64; i++)
    luaM_free_(L, warm[i], BYTES);
  for (i = 0; i < BORROWED; i++) {
    owned[i] = luaM_malloc_(L, BYTES, 0);
    require(owned[i] != NULL);
    memset(owned[i], 1, BYTES);
  }
  allocator.cap = allocator.live;
  G(L)->gcstopem = 1;
  lua_unlock(L);
  require(pthread_create(&freer, NULL, return_remote, &wave) == 0);
  measured = 1;
  admissions = 0;
  for (round = 1; round <= 512; round++) {
    require(pthread_mutex_lock(&wave.mutex) == 0);
    memcpy(wave.blocks, owned, sizeof(owned));
    wave.ready = round;
    require(pthread_cond_broadcast(&wave.changed) == 0);
    require(pthread_mutex_unlock(&wave.mutex) == 0);
    lua_lock(L);
    for (i = 0; i < BORROWED; i++) {
      owned[i] = luaM_malloc_(L, BYTES, 0);
      require(owned[i] != NULL);
      memset(owned[i], (unsigned char)(round + 1), BYTES);
    }
    lua_unlock(L);
    require(pthread_mutex_lock(&wave.mutex) == 0);
    while (wave.returned != round)
      require(pthread_cond_wait(&wave.changed, &wave.mutex) == 0);
    require(pthread_mutex_unlock(&wave.mutex) == 0);
  }
  measured = 0;
  require(pthread_join(freer, NULL) == 0);
  require(admissions == 0 && allocator.denied == 0);
  lua_lock(L);
  allocator.cap = 0;
  G(L)->gcstopem = 0;
  for (i = 0; i < BORROWED; i++)
    luaM_free_(L, owned[i], BYTES);
  lua_unlock(L);
  lua_close(L);
  require(allocator.live == 0);
  require(pthread_cond_destroy(&wave.changed) == 0);
  require(pthread_mutex_destroy(&wave.mutex) == 0);
}

int main(int argc, char **argv) {
  struct allocator allocator = {0};
  lua_State *L;
  require(argc == 2);
  if (strcmp(argv[1], "remote_returns") == 0) {
    remote_returns();
    return 0;
  }
  if (strcmp(argv[1], "control_lifetime") == 0) {
    control_lifetime();
    return 0;
  }
  if (strcmp(argv[1], "batch") == 0 || strcmp(argv[1], "cap") == 0 ||
      strcmp(argv[1], "init_gc") == 0) {
    batch(strcmp(argv[1], "cap") == 0, strcmp(argv[1], "init_gc") == 0);
    return 0;
  }
  L = lua_newstate(allocate, &allocator, 0);
  unsigned char **blocks = calloc(BLOCKS, sizeof(*blocks));
  struct actor actors[ACTORS] = {0};
  pthread_t workers[ACTORS];
  unsigned count, i, j, k, total = 0;
  require(L != NULL && blocks != NULL && argc == 2);
  require(strcmp(argv[1], "serial") == 0 || strcmp(argv[1], "parallel") == 0);
  count = strcmp(argv[1], "parallel") == 0 ? ACTORS : 1;
  for (i = 0; i < count; i++) {
    actors[i].state = lua_newthread(L);
    actors[i].id = i + 1;
  }
  lua_gc(L, LUA_GCSTOP);
  lua_lock(L);
  for (i = 0; i < BLOCKS; i++) {
    blocks[i] = luaM_malloc_(L, BYTES, 0);
    require(blocks[i] != NULL);
    memset(blocks[i], 0xa5, BYTES);
  }
  for (i = 1; i < BLOCKS; i += 2) {
    luaM_free_(L, blocks[i], BYTES);
    blocks[i] = NULL;
  }
  lua_unlock(L);
  /* Keep raw allocations live while GC retires every cached page. The free
   * blocks on those pages can satisfy all consumers without backing work. */
  lua_gc(L, LUA_GCCOLLECT);
  for (i = 0; i < count; i++)
    require(pthread_create(&workers[i], NULL, borrow, &actors[i]) == 0);
  for (i = 0; i < count; i++) {
    require(pthread_join(workers[i], NULL) == 0);
    total += actors[i].admissions;
  }
  lua_lock(L);
  for (i = 0; i < BLOCKS; i += 2) {
    for (j = 0; j < BYTES; j++)
      require(blocks[i][j] == 0xa5);
    luaM_free_(L, blocks[i], BYTES);
  }
  for (i = 0; i < count; i++)
    for (j = 0; j < BORROWED; j++) {
      for (k = 0; k < BYTES; k++)
        require(actors[i].blocks[j][k] == actors[i].id * BORROWED + j);
      luaM_free_(L, actors[i].blocks[j], BYTES);
    }
  lua_unlock(L);
  lua_close(L);
  free(blocks);
  require(allocator.live == 0);
  fprintf(stderr, "reclaimed-page metadata admissions: %u\n", total);
  require(total == 0);
  return 0;
}
