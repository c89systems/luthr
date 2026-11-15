#define _GNU_SOURCE
#include <lauxlib.h>
#include <lstate.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture;
struct context {
  struct fixture *fixture;
  unsigned kind;
  unsigned calls;
};
struct actor {
  struct fixture *fixture;
  lua_State *state;
  int polled;
  int done;
  int status;
  int results;
};
struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_barrier_t managed;
  struct context allocators[2];
  struct context warnings[2];
  struct actor actors[2];
  size_t live_bytes;
  int blocked;
  int entered;
  int release;
};

static void check(int status) {
  if (status != 0)
    abort();
}

static void *allocate(void *user, void *pointer, size_t old_size, size_t size,
                      unsigned expected) {
  struct context *context = user;
  struct fixture *fixture = context->fixture;
  void *replacement;
  if (context->kind != expected)
    abort();
  context->calls++; /* legacy callback state is serialized by heap ownership */
  if (pointer == NULL)
    old_size = 0;
  if (size == 0) {
    fixture->live_bytes -= old_size;
    free(pointer);
    return NULL;
  }
  replacement = realloc(pointer, size);
  if (replacement != NULL)
    fixture->live_bytes = fixture->live_bytes - old_size + size;
  return replacement;
}

static void *allocate_a(void *user, void *pointer, size_t old_size,
                        size_t size) {
  return allocate(user, pointer, old_size, size, 0);
}

static void *allocate_b(void *user, void *pointer, size_t old_size,
                        size_t size) {
  return allocate(user, pointer, old_size, size, 1);
}

static void warn(void *user, const char *message, int continuation,
                 unsigned expected) {
  struct context *context = user;
  struct fixture *fixture = context->fixture;
  if (context->kind != expected || continuation != 0 ||
      strcmp(message, "paired configuration") != 0)
    abort();
  check(pthread_mutex_lock(&fixture->mutex));
  context->calls++;
  if (fixture->blocked && expected == 0) {
    fixture->entered = 1;
    check(pthread_cond_broadcast(&fixture->changed));
    while (!fixture->release)
      check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  }
  check(pthread_mutex_unlock(&fixture->mutex));
}

static void warn_a(void *user, const char *message, int continuation) {
  warn(user, message, continuation, 0);
}

static void warn_b(void *user, const char *message, int continuation) {
  warn(user, message, continuation, 1);
}

static int panic_a(lua_State *state) {
  (void)state;
  abort();
}

static int panic_b(lua_State *state) {
  (void)state;
  abort();
}

static void check_allocator(lua_State *state, struct fixture *fixture) {
  void *user;
  lua_Alloc current = lua_getallocf(state, &user);
  if (!((current == allocate_a && user == &fixture->allocators[0]) ||
        (current == allocate_b && user == &fixture->allocators[1])))
    abort();
}

static int work(lua_State *state) {
  struct fixture *fixture = lua_touserdata(state, lua_upvalueindex(1));
  int role = (int)lua_tointeger(state, 1);
  unsigned i;
  if (fixture->blocked) {
    if (role == 0) {
      lua_warning(state, "paired configuration", 0);
      lua_warning(state, "paired configuration", 0);
    } else {
      check(pthread_mutex_lock(&fixture->mutex));
      while (!fixture->entered)
        check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
      check(pthread_mutex_unlock(&fixture->mutex));
      lua_setwarnf(state, warn_b, &fixture->warnings[1]);
      lua_setallocf(state, allocate_b, &fixture->allocators[1]);
      for (i = 0; i < 300; ++i) {
        (void)lua_newuserdatauv(state, 48, 0);
        lua_pop(state, 1);
      }
      (void)lua_gc(state, LUA_GCCOLLECT);
    }
  } else {
    for (i = 0; i < 300; ++i) {
      if (role == 0) {
        lua_CFunction previous;
        unsigned selected = i % 2;
        lua_setallocf(state, selected ? allocate_b : allocate_a,
                      &fixture->allocators[selected]);
        lua_setwarnf(state, selected ? warn_b : warn_a,
                     &fixture->warnings[selected]);
        previous = lua_atpanic(state, selected ? panic_b : panic_a);
        if (previous != panic_a && previous != panic_b)
          abort();
      } else
        lua_warning(state, "paired configuration", 0);
      check_allocator(state, fixture);
      (void)lua_newuserdatauv(state, 48, 0);
      lua_pop(state, 1);
      if (i % 30 == 0)
        (void)lua_gc(state, LUA_GCCOLLECT);
    }
  }
  return 0;
}

static void managed_poll(lua_State *state, void *user) {
  struct actor *actor = user;
  (void)state;
  if (!actor->polled) {
    int status;
    actor->polled = 1;
    status = pthread_barrier_wait(&actor->fixture->managed);
    if (status != 0 && status != PTHREAD_BARRIER_SERIAL_THREAD)
      abort();
  }
}

static void *run_actor(void *user) {
  struct actor *actor = user;
  actor->status = lua_resume(actor->state, NULL, 0, &actor->results);
  check(pthread_mutex_lock(&actor->fixture->mutex));
  actor->done = 1;
  check(pthread_cond_broadcast(&actor->fixture->changed));
  check(pthread_mutex_unlock(&actor->fixture->mutex));
  return NULL;
}

int main(int argc, char **argv) {
  static const char source[] =
      "return function() coroutine.yield(); fixture_work(0); return true end, "
      "function() coroutine.yield(); fixture_work(1); return true end";
  struct fixture fixture;
  lua_State *main_state;
  pthread_t carriers[2];
  int i, failed = 0;
  if (argc != 2)
    return 2;
  memset(&fixture, 0, sizeof(fixture));
  fixture.blocked = strcmp(argv[1], "blocked") == 0;
  if (!fixture.blocked && strcmp(argv[1], "replace") != 0)
    return 2;
  check(pthread_mutex_init(&fixture.mutex, NULL));
  check(pthread_cond_init(&fixture.changed, NULL));
  check(pthread_barrier_init(&fixture.managed, NULL, 2));
  for (i = 0; i < 2; ++i) {
    fixture.allocators[i].fixture = fixture.warnings[i].fixture = &fixture;
    fixture.allocators[i].kind = fixture.warnings[i].kind = (unsigned)i;
  }
  main_state = lua_newstate(allocate_a, &fixture.allocators[0], 0);
  if (main_state == NULL)
    abort();
  luaL_openlibs(main_state);
  lua_setwarnf(main_state, warn_a, &fixture.warnings[0]);
  (void)lua_atpanic(main_state, panic_a);
  lua_pushlightuserdata(main_state, &fixture);
  lua_pushcclosure(main_state, work, 1);
  lua_setglobal(main_state, "fixture_work");
  if (luaL_loadstring(main_state, source) != LUA_OK ||
      lua_pcall(main_state, 0, 2, 0) != LUA_OK)
    abort();
  for (i = 0; i < 2; ++i) {
    struct actor *actor = &fixture.actors[i];
    int results;
    actor->fixture = &fixture;
    actor->state = lua_newthread(main_state);
    lua_pushvalue(main_state, i + 1);
    lua_xmove(main_state, actor->state, 1);
    if (lua_resume(actor->state, NULL, 0, &results) != LUA_YIELD ||
        results != 0)
      abort();
    actor->state->state_poll = managed_poll;
    actor->state->state_poll_context = actor;
    actor->state->poll_remaining = 1;
  }
  for (i = 0; i < 2; ++i)
    check(pthread_create(&carriers[i], NULL, run_actor, &fixture.actors[i]));
  if (fixture.blocked) {
    check(pthread_join(carriers[1], NULL));
    check(pthread_mutex_lock(&fixture.mutex));
    failed |= fixture.actors[0].done || !fixture.entered;
    fixture.release = 1;
    check(pthread_cond_broadcast(&fixture.changed));
    check(pthread_mutex_unlock(&fixture.mutex));
    check(pthread_join(carriers[0], NULL));
  } else {
    for (i = 0; i < 2; ++i)
      check(pthread_join(carriers[i], NULL));
  }
  for (i = 0; i < 2; ++i) {
    struct actor *actor = &fixture.actors[i];
    failed |= actor->status != LUA_OK || actor->results != 1 ||
              !actor->polled || !lua_toboolean(actor->state, -1);
  }
  failed |=
      fixture.allocators[0].calls == 0 || fixture.allocators[1].calls == 0;
  failed |= fixture.warnings[0].calls + fixture.warnings[1].calls !=
            (fixture.blocked ? 2U : 300U);
  lua_close(main_state);
  failed |= fixture.live_bytes != 0;
  check(pthread_barrier_destroy(&fixture.managed));
  check(pthread_cond_destroy(&fixture.changed));
  check(pthread_mutex_destroy(&fixture.mutex));
  if (!failed)
    printf("callback configuration %s: coherent pairs and managed overlap\n",
           argv[1]);
  return failed;
}
