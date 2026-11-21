#include <lapi.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lgc.h>
#include <lmem.h>
#include <lmempage.h>
#include <lstate.h>
#include <ltask.h>
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

struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *main;
  void *held;
  size_t live, cap;
  unsigned denied, entered, release, stopped, help;
};
static struct fixture *active;
static __thread unsigned interrupt, handoff;
static __thread unsigned produce, polls;
static __thread unsigned measured, admissions;
static __thread unsigned stops;
static LuaExecution *rebind_execution;
static LuaAllocationCache *rebind_cache;
static CallInfo *rebind_frame;

void __real_luaW_stop(lua_State *);
void __wrap_luaW_stop(lua_State *L) {
  if (measured)
    stops++;
  __real_luaW_stop(L);
}

void __real_luaW_heapenter(lua_State *);
void __wrap_luaW_heapenter(lua_State *L) {
  if (measured)
    admissions++;
  __real_luaW_heapenter(L);
}

__typeof__(luaAL_enter) __real_luaAL_enter, __wrap_luaAL_enter;
int __wrap_luaAL_enter(lua_State *L) {
  if (rebind_execution != NULL) {
    LuaExecution *execution = rebind_execution;
    rebind_execution = NULL;
    __atomic_store_n(execution->allocation_cache->permission, NULL,
                     __ATOMIC_RELEASE);
    __atomic_store_n(rebind_cache->permission, execution, __ATOMIC_RELEASE);
    execution->allocation_cache = rebind_cache;
    rebind_frame = luaM_newframe(L);
    require(rebind_frame != NULL);
  }
  return __real_luaAL_enter(L);
}

void __real_luaQ_poll(lua_State *);
void __wrap_luaQ_poll(lua_State *L) {
  if (produce) {
    void *block;
    produce = 0;
    block = luaM_malloc_(L, 64, 0);
    memset(block, 0xa5, 64);
    luaM_free_(L, block, 64);
    produce = 1;
    require(++polls <=
            16); /* a page job must not chase later returns forever */
  }
  if ((interrupt || handoff) &&
      __atomic_load_n(&G(L)->allocation_pending, __ATOMIC_ACQUIRE) != NULL) {
    require(L->world_depth == 0 && luaW_scope(L).heap == 0);
    if (interrupt) {
      interrupt = 0;
      luaD_throw(L, LUA_ERRRUN);
    } else {
      LuaWorldScope scope = luaW_detach(L);
      handoff = 0;
      require(pthread_mutex_lock(&active->mutex) == 0);
      active->entered = 1;
      require(pthread_cond_broadcast(&active->changed) == 0);
      while (!active->release)
        require(pthread_cond_wait(&active->changed, &active->mutex) == 0);
      require(pthread_mutex_unlock(&active->mutex) == 0);
      luaW_attach(L, scope);
    }
  }
  __real_luaQ_poll(L);
}

static void *allocate(void *context, void *pointer, size_t old, size_t size) {
  struct fixture *f = context;
  void *replacement;
  if (pointer == NULL)
    old = 0;
  require(f->live >= old);
  if (size == 0) {
    if (pointer != NULL && pointer == f->held) {
      require(pthread_mutex_lock(&f->mutex) == 0);
      f->stopped = luaW_stopped(&G(f->main)->world);
      f->entered = 1;
      require(pthread_cond_broadcast(&f->changed) == 0);
      while (!f->release)
        require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
      f->held = NULL;
      require(pthread_mutex_unlock(&f->mutex) == 0);
    }
    free(pointer);
    f->live -= old;
    return NULL;
  }
  if (f->cap != 0 && size > f->cap - (f->live - old)) {
    f->denied++;
    return NULL;
  }
  replacement = realloc(pointer, size);
  if (replacement != NULL)
    f->live = f->live - old + size;
  return replacement;
}

static LuaAllocPage *pageof(void *block) {
  return luaMP_page((LuaAllocBlock *)block - 1);
}

static void throw_poll(LuaExecution *execution, lua_State *L) {
  require(execution->memory_cache == NULL);
  luaD_throw(L, LUA_ERRRUN);
}

static void poll_pages(lua_State *L, void *unused) {
  (void)unused;
  luaQ_poll(L);
}

static void retained(lua_State *L, struct fixture *f) {
  unsigned char *first, *second;
  LuaAllocPage *page;
  lua_lock(L);
  first = luaM_malloc_(L, 128, 0);
  memset(first, 0xa5, 128);
  page = pageof(first);
  require(page != NULL && page->owner != NULL);
  lua_unlock(L);
  lua_gc(L, LUA_GCCOLLECT);
  lua_lock(L);
  require(page->owner != NULL && page->owner->pages[page->pool] == page);
  f->cap = f->live;
  measured = 1;
  second = luaM_malloc_(L, 128, 0);
  measured = 0;
  require(pageof(second) == page && admissions == 0 && f->denied == 0);
  require(first[0] == 0xa5 && first[127] == 0xa5);
  f->cap = 0;
  luaM_free_(L, first, 128);
  luaM_free_(L, second, 128);
  lua_unlock(L);
}

static void pressure(lua_State *L, struct fixture *f) {
  lua_State *worker = lua_newthread(L);
  CallInfo *first, *second;
  LuaAllocPage *page;
  lua_gc(L, LUA_GCCOLLECT);
  lua_lock(L);
  first = luaM_newframe(L);
  require(first != NULL);
  page = pageof(first);
  lua_unlock(L);
  lua_gc(L, LUA_GCCOLLECT);
  lua_lock(L);
  luaM_reclaimpages(L); /* Leave a clean, nonempty owner cache. */
  require(page->owner != NULL && page->owner->pages[page->pool] == page);
  require(__atomic_load_n(&G(L)->allocation_dirty, __ATOMIC_ACQUIRE) == NULL);
  f->cap = f->live;
  lua_unlock(L);
  lua_lock(worker);
  second = luaM_newframe(worker);
  require(second != NULL && pageof(second) == page && f->denied != 0);
  lua_unlock(worker);
  lua_lock(L);
  f->cap = 0;
  luaM_free_(L, first, sizeof(*first));
  luaM_free_(L, second, sizeof(*second));
  lua_unlock(L);
}

static void task_cache(lua_State *L, int unwind) {
  LuaExecution execution = {0};
  LuaTaskRoot task = {0};
  NativeStackContext context = {0}, *previous;
  lua_State *first = lua_newthread(L), *second = lua_newthread(L);
  void *a, *b;
  LuaAllocPage *page;
  int status, results;
  require(luaL_loadstring(first, "return 17") == LUA_OK);
  require(luaTK_admit(first, &task, &execution) == LUA_OK);
  context.execution = &execution;
  previous = luaE_setnativestackcontext(&context);
  lua_lock(first);
  a = luaM_malloc_(first, 128, 0);
  page = pageof(a);
  require(page->owner == &task.allocation);
  if (unwind) {
    execution.poll = throw_poll;
    require(luaD_rawrunprotected(first, poll_pages, NULL) == LUA_ERRRUN);
    require(execution.memory_cache == &task.allocation);
    require(__atomic_load_n(&task.allocation.owner, __ATOMIC_ACQUIRE) ==
            &execution);
    execution.poll = NULL;
  }
  lua_unlock(first);
  lua_lock(second);
  b = luaM_malloc_(second, 128, 0);
  require(pageof(b) ==
          page); /* the execution owns capacity across coroutines */
  lua_unlock(second);
  status = luaTK_resume(&task, 0, &results);
  require(status == LUA_OK && results == 1);
  require(luaTK_finish(&task, &status, &results) == LUA_OK);
  require(luaTK_collect(L, &task, &status, &results) == LUA_OK);
  require(page->owner == NULL && execution.task == NULL);
  lua_lock(L);
  luaM_free_(L, a, 128);
  luaM_free_(L, b, 128);
  lua_unlock(L);
  luaE_setnativestackcontext(previous);
}

struct busy_cache {
  struct fixture *fixture;
  LuaExecution execution;
  LuaTaskRoot task;
  void *block;
  LuaAllocPage *page;
};

static void *hold_cache(void *argument) {
  struct busy_cache *test = argument;
  struct fixture *f = test->fixture;
  NativeStackContext context = {0}, *previous;
  lua_State *L = test->task.thread;
  void *replacement;
  context.execution = &test->execution;
  previous = luaE_setnativestackcontext(&context);
  lua_lock(L);
  test->block = luaM_malloc_(L, 128, 0);
  test->page = pageof(test->block);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->entered = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  while (!f->release)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  replacement = luaM_malloc_(L, 128, 0);
  require(pageof(replacement) == test->page);
  memset(replacement, 0xa5, 128);
  luaM_free_(L, replacement, 128);
  lua_unlock(L);
  luaE_setnativestackcontext(previous);
  return NULL;
}

static void busy_cache(lua_State *L, struct fixture *f) {
  struct busy_cache test = {0};
  NativeStackContext context = {0}, *previous;
  lua_State *worker = lua_newthread(L);
  pthread_t holder;
  int status, results;
  unsigned pool;
  require(luaL_loadstring(worker, "return 17") == LUA_OK);
  require(luaTK_admit(worker, &test.task, &test.execution) == LUA_OK);
  test.fixture = f;
  require(pthread_create(&holder, NULL, hold_cache, &test) == 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  lua_lock(L);
  pool = test.page->pool;
  luaM_free_(L, test.block, 128);
  measured = 1;
  luaM_reclaimpages(L);
  measured = 0;
  require(stops == 0 && test.page->owner == &test.task.allocation);
  lua_unlock(L);
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(pthread_join(holder, NULL) == 0);
  lua_lock(L);
  measured = 1;
  luaM_reclaimpages(L);
  measured = 0;
  require(stops == 0 && test.task.allocation.pages[pool] == NULL);
  lua_unlock(L);
  context.execution = &test.execution;
  previous = luaE_setnativestackcontext(&context);
  status = luaTK_resume(&test.task, 0, &results);
  require(status == LUA_OK && results == 1);
  require(luaTK_finish(&test.task, &status, &results) == LUA_OK);
  require(luaTK_collect(L, &test.task, &status, &results) == LUA_OK);
  luaE_setnativestackcontext(previous);
}

enum { RETURN_WORKERS = 3, RETURN_BLOCKS = 30000 };
struct remote_returns {
  lua_State *workers[RETURN_WORKERS];
  void **blocks;
  unsigned ready, start, done;
};
struct return_worker {
  struct remote_returns *test;
  unsigned index;
};

static void *return_blocks(void *argument) {
  struct return_worker *worker = argument;
  struct remote_returns *test = worker->test;
  lua_State *L = test->workers[worker->index];
  unsigned i;
  lua_lock(L);
  __atomic_fetch_add(&test->ready, 1, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&test->start, __ATOMIC_ACQUIRE))
    luaX_pollrequests(L);
  for (i = worker->index; i < RETURN_BLOCKS; i += RETURN_WORKERS) {
    luaM_free_(L, test->blocks[i], 64);
    if (i % 127 == 0)
      luaX_pollrequests(L);
  }
  lua_unlock(L);
  __atomic_fetch_add(&test->done, 1, __ATOMIC_RELEASE);
  return NULL;
}

static void remote_returns(lua_State *L) {
  struct remote_returns test = {0};
  struct return_worker workers[RETURN_WORKERS];
  pthread_t threads[RETURN_WORKERS];
  unsigned i;
  test.blocks = calloc(RETURN_BLOCKS, sizeof(*test.blocks));
  require(test.blocks != NULL);
  for (i = 0; i < RETURN_WORKERS; i++)
    test.workers[i] = lua_newthread(L);
  lua_lock(L);
  for (i = 0; i < RETURN_BLOCKS; i++) {
    test.blocks[i] = luaM_malloc_(L, 64, 0);
    memset(test.blocks[i], (int)i, 64);
  }
  lua_unlock(L);
  for (i = 0; i < RETURN_WORKERS; i++) {
    workers[i].test = &test;
    workers[i].index = i;
    require(pthread_create(&threads[i], NULL, return_blocks, &workers[i]) == 0);
  }
  while (__atomic_load_n(&test.ready, __ATOMIC_ACQUIRE) != RETURN_WORKERS) {
  }
  lua_lock(L);
  __atomic_store_n(&test.start, 1, __ATOMIC_RELEASE);
  measured = 1;
  do {
    luaM_reclaimpages(L);
  } while (__atomic_load_n(&test.done, __ATOMIC_ACQUIRE) != RETURN_WORKERS);
  measured = 0;
  require(stops == 0);
  lua_unlock(L);
  for (i = 0; i < RETURN_WORKERS; i++)
    require(pthread_join(threads[i], NULL) == 0);
  lua_lock(L);
  luaM_reclaimpages(L);
  require(G(L)->allocation_dirty == NULL && G(L)->allocation_pending == NULL &&
          G(L)->allocation_retired == NULL &&
          G(L)->allocation_returning == NULL);
  lua_unlock(L);
  free(test.blocks);
}

static void growth_handoff(lua_State *L, int collect) {
  LuaAllocationCache cache = {0};
  LuaExecution execution = {0};
  NativeStackContext context = {0}, *previous;
  void **blocks = calloc(5000, sizeof(*blocks)), *replacement;
  size_t bytes, cold = 0;
  unsigned count = 0, i;
  require(blocks != NULL);
  cache.global = G(L);
  execution.allocation_cache = &cache;
  context.execution = &execution;
  previous = luaE_setnativestackcontext(&context);
  lua_lock(L);
  do {
    require(count < 5000);
    blocks[count] = luaM_malloc_(L, 128, 0);
    bytes = pageof(blocks[count++])->bytes;
    if (cold == 0)
      cold = bytes;
  } while (bytes < 65536);
  luaM_releasecache(L, &cache); /* admission can return to another task/slot */
  replacement = luaM_malloc_(L, 128, 0);
  require(pageof(replacement)->bytes >= bytes);
  luaM_free_(L, replacement, 128);
  for (i = 0; i < count; i++)
    luaM_free_(L, blocks[i], 128);
  if (collect) {
    lua_unlock(L);
    lua_gc(L, LUA_GCCOLLECT);
    lua_lock(L);
    replacement = luaM_malloc_(L, 128, 0);
    require(pageof(replacement)->bytes == cold);
    luaM_free_(L, replacement, 128);
  }
  luaM_releasecache(L, &cache);
  lua_unlock(L);
  luaE_setnativestackcontext(previous);
  free(blocks);
}

static void assist_rebind(lua_State *L) {
  LuaAllocationCache caches[2] = {{0}};
  LuaExecution execution = {0};
  NativeStackContext context = {0}, *previous;
  void *owners[2] = {&execution, NULL}, *empty;
  CallInfo *frames[64], *kept[64], *result;
  LuaAllocPage *last = NULL;
  unsigned i, count = 0;
  for (i = 0; i < 2; i++) {
    caches[i].global = G(L);
    caches[i].permission = &owners[i];
  }
  execution.allocation_cache = &caches[0];
  context.execution = &execution;
  previous = luaE_setnativestackcontext(&context);
  lua_lock(L);
  for (i = 0; i < 64; i++) {
    frames[i] = luaM_newframe(L);
    require(frames[i] != NULL);
  }
  for (i = 0; i < 64; i++) {
    LuaAllocPage *page = pageof(frames[i]);
    if (page != last)
      kept[count++] = frames[i];
    else
      luaM_free_(L, frames[i], sizeof(*frames[i]));
    last = page;
  }
  require(count >= 3);
  empty = luaM_malloc_(L, 512, 0);
  luaM_free_(L, empty, 512);
  luaM_releasecache(L, &caches[0]);
  /* The assist offers partial frame pages, then returns the empty page's
   * backing. Its admission lends the first slot and resumes on a warm second
   * slot. Another offered page remains, so a stale empty-cache assumption is
   * falsifiable rather than silently taking the later backing slow path. */
  rebind_cache = &caches[1];
  rebind_execution = &execution;
  result = luaM_newframe(L);
  require(rebind_execution == NULL && result != NULL && result != rebind_frame);
  require(pageof(result) == pageof(rebind_frame));
  for (i = 0; i < count; i++)
    luaM_free_(L, kept[i], sizeof(*kept[i]));
  luaM_free_(L, result, sizeof(*result));
  luaM_free_(L, rebind_frame, sizeof(*rebind_frame));
  luaM_releasecache(L, &caches[1]);
  lua_unlock(L);
  luaE_setnativestackcontext(previous);
}

static void single_boundary(lua_State *L) {
  void *block;
  lua_lock(L);
  luaM_reclaimpages(L);
  block = luaM_malloc_(L, 128, 0);
  luaM_free_(L, block, 128);
  measured = 1;
  luaC_finishreclaim(L);
  measured = 0;
  require(stops ==
          0); /* page lifetime handoff never stops application workers */
  lua_unlock(L);
}

static void physical_credit(lua_State *L) {
  void *block;
  l_mem charged, pending, debt;
  lua_lock(L);
  block = luaM_malloc_(L, 128, 0);
  luaW_stop(L);
  charged = gettotalbytes(G(L));
  pending = G(L)->GCretired;
  debt = luaE_getdebt(G(L));
  luaM_retire_(L, block, 128);
  require(gettotalbytes(G(L)) == charged && G(L)->GCretired == pending + 128);
  require(luaE_getdebt(G(L)) == debt + 128);
  luaW_start(L);
  measured = 1;
  luaM_free_(L, block, 128);
  luaM_flushdebt(L);
  measured = 0;
  require(stops == 0 && gettotalbytes(G(L)) == charged - 128);
  require(G(L)->GCretired == pending && luaE_getdebt(G(L)) == debt + 128);
  luaM_reclaimpages(L);
  measured = 1;
  luaC_finishreclaim(L);
  measured = 0;
  require(stops == 0); /* physical accounting needs no pacing rendezvous */
  lua_unlock(L);
}

static void garbage(lua_State *L, struct fixture *f, int hold) {
  enum { BLOCKS = 100000, BYTES = 64 };
  void **blocks = calloc(BLOCKS, sizeof(*blocks));
  unsigned i;
  require(blocks != NULL);
  lua_lock(L);
  for (i = 0; i < BLOCKS; i++) {
    blocks[i] = luaM_malloc_(L, BYTES, 0);
    memset(blocks[i], (unsigned char)i, BYTES);
  }
  if (hold)
    f->held = pageof(blocks[BLOCKS / 2]);
  for (i = 0; i < BLOCKS; i++)
    luaM_free_(L, blocks[i], BYTES);
  lua_unlock(L);
  free(blocks);
}

static void pages(lua_State *L, void *unused) {
  (void)unused;
  luaM_reclaimpages(L);
}

static void *collect(void *context) {
  struct fixture *f = context;
  if (f->help) {
    lua_lock(f->main);
    handoff = 1;
    luaM_reclaimpages(f->main);
    require(handoff == 0);
    lua_unlock(f->main);
  } else
    require(lua_gc(f->main, LUA_GCCOLLECT) == 0);
  return NULL;
}

static void joined(lua_State *L, struct fixture *f, int help) {
  lua_State *worker = lua_newthread(L);
  pthread_t collector;
  require(luaL_loadstring(worker,
                          "return function() local sum=0; for i=1,200000 do "
                          "sum=sum+i end; return sum end") == LUA_OK);
  require(lua_pcall(worker, 0, 1, 0) == LUA_OK);
  lua_pushvalue(worker, -1);
  require(lua_pcall(worker, 0, 1, 0) == LUA_OK);
  lua_pop(worker,
          1); /* warm stack/frame storage before holding the allocator */
  garbage(L, f, !help);
  f->help = (unsigned)help;
  require(pthread_create(&collector, NULL, collect, f) == 0);
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  if (help) {
    lua_lock(worker);
    luaM_reclaimpages(worker);
    require(__atomic_load_n(&G(worker)->allocation_pending, __ATOMIC_ACQUIRE) ==
            NULL);
    lua_unlock(worker);
  } else {
    require(f->stopped == 0);
    lua_pushvalue(worker, -1);
    require(lua_pcall(worker, 0, 1, 0) == LUA_OK);
    require(lua_tointeger(worker, -1) == (lua_Integer)20000100000LL);
  }
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->release = 1;
  require(pthread_cond_broadcast(&f->changed) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
  require(pthread_join(collector, NULL) == 0);
}

int main(int argc, char **argv) {
  struct fixture f = {0};
  lua_State *L;
  require(argc == 2);
  require(pthread_mutex_init(&f.mutex, NULL) == 0);
  require(pthread_cond_init(&f.changed, NULL) == 0);
  L = lua_newstate(allocate, &f, 0);
  require(L != NULL);
  f.main = L;
  active = &f;
  lua_gc(L, LUA_GCSTOP);
  lua_gc(L, LUA_GCCOLLECT);
  if (strcmp(argv[1], "retained") == 0)
    retained(L, &f);
  else if (strcmp(argv[1], "pressure") == 0)
    pressure(L, &f);
  else if (strcmp(argv[1], "task") == 0 || strcmp(argv[1], "lease_unwind") == 0)
    task_cache(L, strcmp(argv[1], "lease_unwind") == 0);
  else if (strcmp(argv[1], "busy_cache") == 0)
    busy_cache(L, &f);
  else if (strcmp(argv[1], "remote_returns") == 0)
    remote_returns(L);
  else if (strcmp(argv[1], "growth_handoff") == 0 ||
           strcmp(argv[1], "growth_epoch") == 0)
    growth_handoff(L, strcmp(argv[1], "growth_epoch") == 0);
  else if (strcmp(argv[1], "assist_rebind") == 0)
    assist_rebind(L);
  else if (strcmp(argv[1], "boundary") == 0)
    single_boundary(L);
  else if (strcmp(argv[1], "credit") == 0)
    physical_credit(L);
  else if (strcmp(argv[1], "overlap") == 0 || strcmp(argv[1], "helpers") == 0)
    joined(L, &f, strcmp(argv[1], "helpers") == 0);
  else {
    int finite = strcmp(argv[1], "finite") == 0;
    require(finite || strcmp(argv[1], "unwind") == 0);
    garbage(L, &f, 0);
    lua_lock(L);
    if (finite) {
      produce = 1;
      luaM_reclaimpages(L);
      produce = 0;
      require(polls >= 2 && __atomic_load_n(&G(L)->allocation_dirty,
                                            __ATOMIC_ACQUIRE) != NULL);
    } else {
      interrupt = 1;
      require(luaD_rawrunprotected(L, pages, NULL) == LUA_ERRRUN);
      require(interrupt == 0 && L->world_depth == 0 && luaW_scope(L).heap == 0);
      require(__atomic_load_n(&G(L)->allocation_pending, __ATOMIC_ACQUIRE) !=
              NULL);
    }
    luaM_reclaimpages(L);
    require(__atomic_load_n(&G(L)->allocation_pending, __ATOMIC_ACQUIRE) ==
            NULL);
    lua_unlock(L);
  }
  lua_close(L);
  require(f.live == 0);
  require(pthread_cond_destroy(&f.changed) == 0);
  require(pthread_mutex_destroy(&f.mutex) == 0);
  return 0;
}
