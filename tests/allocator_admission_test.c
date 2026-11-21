#include <lapi.h>
#include <lauxlib.h>
#include <lmem.h>
#include <lstate.h>
#include <luthr/lua_runtime.h>
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

struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  lua_State *main;
  size_t live;
  unsigned active, armed, entered, release;
};
static __thread int forbid_metadata;
static __thread pthread_cond_t *forbid_notification;

struct builtin_worker {
  lua_State *state;
  pthread_barrier_t *allocating;
  size_t bytes;
};
static __thread struct builtin_worker *builtin_probe;

void *__real_realloc(void *, size_t);
void *__wrap_realloc(void *pointer, size_t size) {
  struct builtin_worker *worker = builtin_probe;
  if (worker != NULL) {
    int status;
    builtin_probe = NULL;
    require(__atomic_load_n(&G(worker->state)->allocator.owner,
                            __ATOMIC_ACQUIRE) == NULL);
    status = pthread_barrier_wait(worker->allocating);
    require(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
  }
  return __real_realloc(pointer, size);
}

void __real_luaQ_wake(pthread_cond_t *, LuaWaiter **);
void __wrap_luaQ_wake(pthread_cond_t *changed, LuaWaiter **waiters) {
  require(changed != forbid_notification);
  __real_luaQ_wake(changed, waiters);
}

void __real_luaW_heapenter(lua_State *);
void __wrap_luaW_heapenter(lua_State *state) {
  require(!forbid_metadata);
  __real_luaW_heapenter(state);
}

static void notify(struct fixture *f) {
  require(pthread_cond_broadcast(&f->changed) == 0);
}

static void hold(struct fixture *f) {
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->entered = 1;
  notify(f);
  while (!f->release)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
}

static void *allocate(void *context, void *old, size_t old_size, size_t size) {
  struct fixture *f = context;
  void *result;
  require(__atomic_fetch_add(&f->active, 1, __ATOMIC_ACQ_REL) == 0);
  if (old == NULL)
    old_size = 0;
  require(f->live >= old_size);
  if (size != 0 && __atomic_exchange_n(&f->armed, 0, __ATOMIC_ACQ_REL))
    hold(f);
  if (size == 0) {
    free(old);
    f->live -= old_size;
    result = NULL;
  } else {
    result = realloc(old, size);
    require(result != NULL);
    f->live = f->live - old_size + size;
  }
  require(__atomic_fetch_sub(&f->active, 1, __ATOMIC_RELEASE) == 1);
  return result;
}

static void *external_free(void *context, void *old, size_t old_size,
                           size_t size) {
  struct fixture *f = context;
  require(size == 0 && old != NULL);
  require(__atomic_load_n(&G(f->main)->allocator.owner, __ATOMIC_ACQUIRE) !=
          NULL);
  require(__atomic_fetch_add(&f->active, 1, __ATOMIC_ACQ_REL) == 0);
  hold(f);
  require(f->live >= old_size);
  f->live -= old_size;
  free(old);
  require(__atomic_fetch_sub(&f->active, 1, __ATOMIC_RELEASE) == 1);
  return NULL;
}

static void *collect(void *argument) {
  require(lua_gc(argument, LUA_GCCOLLECT) == 0);
  return NULL;
}

static void wait_entered(struct fixture *f) {
  require(pthread_mutex_lock(&f->mutex) == 0);
  while (!f->entered)
    require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
  require(pthread_mutex_unlock(&f->mutex) == 0);
}

static void release(struct fixture *f) {
  require(pthread_mutex_lock(&f->mutex) == 0);
  f->release = 1;
  notify(f);
  require(pthread_mutex_unlock(&f->mutex) == 0);
}

static void *hold_metadata(void *argument) {
  lua_State *state = argument;
  struct fixture *f;
  lua_getallocf(state, (void **)&f);
  lua_lock(state);
  luaW_heapenter(state);
  hold(f);
  luaW_heapleave(state);
  lua_unlock(state);
  return NULL;
}

static void native_memory(lua_State *state) {
  unsigned char *small, *large, *buffer;
  lua_lock(state);
  require(!luaW_trycollectenter(state));
  forbid_metadata = 1;
  small = luaM_malloc_(state, 128, 0);
  large = luaM_malloc_(state, 4096, 0);
  memset(small, 0x5a, 128);
  memset(large, 0xa5, 4096);
  large = luaM_saferealloc_(state, large, 4096, 8192);
  require(large[0] == 0xa5 && large[4095] == 0xa5);
  /* A large-to-small cutover copies and returns the old backing block. */
  large = luaM_saferealloc_(state, large, 8192, 128);
  require(large[0] == 0xa5 && large[127] == 0xa5);
  luaM_free_(state, small, 128);
  luaM_free_(state, large, 128);
  luaM_releasecache(state, &state->allocation);
  luaM_reclaimpages(state);
  forbid_metadata = 0;
  lua_unlock(state);
  forbid_metadata = 1;
  buffer = luaM_reallocbuffer(state, NULL, 0, 4096);
  memset(buffer, 0x3c, 4096);
  buffer = luaM_reallocbuffer(state, buffer, 4096, 8192);
  require(buffer[0] == 0x3c && buffer[4095] == 0x3c);
  require(luaM_reallocbuffer(state, buffer, 8192, 0) == NULL);
  forbid_metadata = 0;
}

static void *backing(void *argument) {
  lua_State *state = argument;
  unsigned char *block;
  lua_lock(state);
  block = luaM_malloc_(state, 4096, 0);
  memset(block, 0x6a, 4096);
  require(block[4095] == 0x6a);
  luaM_free_(state, block, 4096);
  lua_unlock(state);
  return NULL;
}

static void *builtin_backing(void *argument) {
  struct builtin_worker *worker = argument;
  unsigned char *bytes;
  builtin_probe = worker;
  bytes = lua_newuserdatauv(worker->state, worker->bytes, 0);
  require(builtin_probe == NULL && bytes != NULL);
  memset(bytes, 0x42, worker->bytes);
  require(bytes[worker->bytes - 1] == 0x42);
  lua_pop(worker->state, 1);
  return NULL;
}

static void builtin_overlap(size_t bytes, int facade) {
  luthr_lua_runtime *runtime;
  lua_State *main_state;
  pthread_barrier_t allocating;
  pthread_t carriers[2];
  struct builtin_worker workers[2];
  unsigned i;
  if (facade) {
    require(luthr_lua_runtime_new(&runtime) == LUTHR_LUA_RUNTIME_OK);
    main_state = luthr_lua_runtime_state(runtime);
  } else {
    main_state = luaL_newstate();
  }
  require(main_state != NULL && lua_getallocf(main_state, NULL) == luaL_alloc);
  lua_gc(main_state, LUA_GCSTOP);
  require(pthread_barrier_init(&allocating, NULL, 2) == 0);
  for (i = 0; i < 2; i++) {
    workers[i].state = lua_newthread(main_state);
    workers[i].allocating = &allocating;
    workers[i].bytes = bytes;
  }
  for (i = 0; i < 2; i++)
    require(pthread_create(&carriers[i], NULL, builtin_backing, &workers[i]) ==
            0);
  for (i = 0; i < 2; i++)
    require(pthread_join(carriers[i], NULL) == 0);
  require(pthread_barrier_destroy(&allocating) == 0);
  if (facade)
    luthr_lua_runtime_free(runtime);
  else
    lua_close(main_state);
}

int main(int argc, char **argv) {
  struct fixture f = {0};
  lua_State *first, *second;
  pthread_t owner, waiter;
  int builtin_external;
  require(argc == 2);
  if (strcmp(argv[1], "builtin_direct") == 0 ||
      strcmp(argv[1], "builtin_refill") == 0 ||
      strcmp(argv[1], "builtin_facade") == 0) {
    builtin_overlap(strcmp(argv[1], "builtin_refill") == 0 ? 128 : 4096,
                    strcmp(argv[1], "builtin_facade") == 0);
    return 0;
  }
  builtin_external = strcmp(argv[1], "builtin_external") == 0;
  require(pthread_mutex_init(&f.mutex, NULL) == 0);
  require(pthread_cond_init(&f.changed, NULL) == 0);
  f.main = builtin_external ? luaL_newstate() : lua_newstate(allocate, &f, 0);
  require(f.main != NULL);
  if (strstr(argv[1], "generational"))
    lua_gc(f.main, LUA_GCGEN);
  lua_gc(f.main, LUA_GCSTOP);
  first = lua_newthread(f.main);
  second = lua_newthread(f.main);
  if (strncmp(argv[1], "metadata_", 9) == 0) {
    require(pthread_create(&owner, NULL, hold_metadata, first) == 0);
    wait_entered(&f);
    native_memory(second); /* Must finish while another state owns metadata. */
    release(&f);
    require(pthread_join(owner, NULL) == 0);
  } else if (strncmp(argv[1], "notification_", 13) == 0) {
    int admitted;
    lua_lock(first);
    admitted = luaAL_enter(first);
    require(admitted);
    require(pthread_create(&waiter, NULL, backing, second) == 0);
    while (__atomic_load_n(&G(f.main)->allocator.waiting, __ATOMIC_ACQUIRE) ==
           0)
      sched_yield();
    /* Only callback waiters may be notified by callback admission release. */
    forbid_notification = &G(f.main)->world.changed;
    luaAL_leave(first, admitted);
    forbid_notification = NULL;
    lua_unlock(first);
    require(pthread_join(waiter, NULL) == 0);
  } else {
    if (builtin_external || strncmp(argv[1], "external_", 9) == 0) {
      char *bytes = allocate(&f, NULL, 0, 64);
      memset(bytes, 'x', 63);
      bytes[63] = '\0';
      lua_pushexternalstring(first, bytes, 63, external_free, &f);
      lua_pop(first, 1);
      require(pthread_create(&owner, NULL, collect, first) == 0);
    } else {
      require(strncmp(argv[1], "callback_", 9) == 0);
      __atomic_store_n(&f.armed, 1, __ATOMIC_RELEASE);
      require(pthread_create(&owner, NULL, backing, first) == 0);
    }
    wait_entered(&f);
    if (builtin_external) {
      backing(second); /* The external free is still waiting. */
    } else {
      require(pthread_create(&waiter, NULL, backing, second) == 0);
      while (__atomic_load_n(&G(f.main)->allocator.waiting, __ATOMIC_ACQUIRE) ==
             0)
        sched_yield();
      lua_lock(f.main);
      luaW_heapenter(f.main); /* A blocked callback cannot retain metadata. */
      require(luaW_trycollectenter(f.main));
      luaW_heapleave(f.main); /* recursive try preserves the original scope */
      require(__atomic_load_n(&f.active, __ATOMIC_ACQUIRE) == 1);
      luaW_heapleave(f.main);
      lua_unlock(f.main);
    }
    release(&f);
    require(pthread_join(owner, NULL) == 0);
    if (!builtin_external)
      require(pthread_join(waiter, NULL) == 0);
  }
  require(G(f.main)->allocator.owner == NULL);
  lua_close(f.main);
  require(f.live == 0 && f.active == 0);
  require(pthread_cond_destroy(&f.changed) == 0);
  require(pthread_mutex_destroy(&f.mutex) == 0);
  return 0;
}
