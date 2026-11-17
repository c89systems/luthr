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
  unsigned id, admissions;
  unsigned char *blocks[BORROWED];
};
static __thread unsigned measured, admissions;
void __real_luaW_heapenter(lua_State *);
void __wrap_luaW_heapenter(lua_State *L) {
  if (measured)
    admissions++;
  __real_luaW_heapenter(L);
}

static void *allocate(void *context, void *pointer, size_t old, size_t size) {
  size_t *live = context;
  void *replacement;
  if (pointer == NULL)
    old = 0;
  require(old <= *live);
  if (size == 0) {
    *live -= old;
    free(pointer);
    return NULL;
  }
  replacement = realloc(pointer, size);
  if (replacement != NULL)
    *live = *live - old + size;
  return replacement;
}

static void *borrow(void *argument) {
  struct actor *actor = argument;
  unsigned i;
  lua_lock(actor->state);
  admissions = 0;
  measured = 1;
  for (i = 0; i < BORROWED; i++) {
    actor->blocks[i] = luaM_malloc_(actor->state, BYTES, 0);
    require(actor->blocks[i] != NULL);
    memset(actor->blocks[i], (int)(actor->id * BORROWED + i), BYTES);
  }
  measured = 0;
  actor->admissions = admissions;
  lua_unlock(actor->state);
  return NULL;
}

int main(int argc, char **argv) {
  size_t live = 0;
  lua_State *L = lua_newstate(allocate, &live, 0);
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
  require(live == 0);
  fprintf(stderr, "reclaimed-page metadata admissions: %u\n", total);
  require(total == 0);
  return 0;
}
