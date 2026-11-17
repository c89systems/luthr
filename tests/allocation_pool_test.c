#include <lapi.h>
#include <lauxlib.h>
#include <lmem.h>
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

enum { BLOCKS = 5000, ACTORS = 4, BORROWED = 16, BYTES = 64 };
struct actor {
  lua_State *state;
  LuaAllocationCache cache;
  unsigned id, admissions;
  int frame;
  unsigned char *blocks[BORROWED];
};
static __thread unsigned measured, admissions;
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
    context.execution = &execution;
    execution.allocation_cache = &actor->cache;
    previous = luaE_setnativestackcontext(&context);
  }
  lua_lock(actor->state);
  admissions = 0;
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
  lua_unlock(actor->state);
  if (actor->cache.global != NULL)
    luaE_setnativestackcontext(previous);
  return NULL;
}

static void batch(int capped) {
  struct allocator allocator = {0};
  lua_State *L = lua_newstate(allocate, &allocator, 0);
  struct actor actors[ACTORS] = {0};
  unsigned i, j, total = 0;
  require(L != NULL);
  for (i = 0; i < ACTORS; i++) {
    actors[i].state = lua_newthread(L);
    actors[i].id = i + 1;
    actors[i].frame = 1;
    actors[i].cache.global = G(L);
    actors[i].cache.parallelism = ACTORS;
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
  if (capped)
    allocator.cap = allocator.live + 4096;
  for (i = 0; i < ACTORS; i++) {
    borrow(&actors[i]);
    total += actors[i].admissions;
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
  fprintf(stderr, "cold-pool metadata admissions: %u\n", total);
  require(capped || total == 1);
}

int main(int argc, char **argv) {
  struct allocator allocator = {0};
  lua_State *L;
  require(argc == 2);
  if (strcmp(argv[1], "batch") == 0 || strcmp(argv[1], "cap") == 0) {
    batch(strcmp(argv[1], "cap") == 0);
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
