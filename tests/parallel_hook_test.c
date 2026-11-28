#define _GNU_SOURCE
#include <errno.h>
#include <lauxlib.h>
#include <limits.h>
#include <lstate.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum mode { REPLACE, BLOCKED, SIGNAL, YIELD, COUNTS };
struct fixture;
struct actor {
  struct fixture *fixture;
  lua_State *state;
  int polled;
  int status;
  int results;
  unsigned yields;
};
struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_barrier_t managed;
  lua_State *main;
  struct actor actors[2];
  enum mode mode;
  unsigned calls[2];
  unsigned local_edits;
  unsigned steps;
  unsigned inherited;
  int entered;
  int release;
  int finish;
  int replacement_done;
};
static struct fixture *active_fixture;
static lua_State *signal_state;
static sem_t signal_seen;
static pthread_barrier_t *first_hook_lookup;

static void check(int status) {
  if (status != 0)
    abort();
}

static void rendezvous(pthread_barrier_t *barrier) {
  int status = pthread_barrier_wait(barrier);
  if (status != 0 && status != PTHREAD_BARRIER_SERIAL_THREAD)
    abort();
}

int __real_lua_getfield(lua_State *state, int index, const char *key);
int __wrap_lua_getfield(lua_State *state, int index, const char *key) {
  int result = __real_lua_getfield(state, index, key);
  /* Force both first registrations past the initial registry miss. */
  if (first_hook_lookup != NULL && index == LUA_REGISTRYINDEX &&
      result == LUA_TNIL && strcmp(key, "_HOOKKEY") == 0)
    rendezvous(first_hook_lookup);
  return result;
}

static void hook_a(lua_State *, lua_Debug *);
static void hook_b(lua_State *, lua_Debug *);

static void inspect_frame(lua_State *state, lua_Debug *record) {
  struct fixture *fixture = active_fixture;
  int i;
  if (!lua_getinfo(state, "Sl", record))
    abort();
  for (i = 1; i <= 8; ++i) {
    const char *name = lua_getlocal(state, record, i);
    if (name == NULL)
      break;
    if (strcmp(name, "hook_probe") == 0) {
      lua_Integer value = lua_tointeger(state, -1);
      if (value != 7 && value != 11)
        abort();
      lua_pop(state, 1);
      lua_pushinteger(state, 11);
      if (strcmp(lua_setlocal(state, record, i), "hook_probe") != 0)
        abort();
      check(pthread_mutex_lock(&fixture->mutex));
      fixture->local_edits++;
      check(pthread_cond_broadcast(&fixture->changed));
      check(pthread_mutex_unlock(&fixture->mutex));
      break;
    }
    lua_pop(state, 1);
  }
}

static void hook(lua_State *state, lua_Debug *record, unsigned kind) {
  struct fixture *fixture = active_fixture;
  unsigned call;
  int inspect;
  if (record->event < LUA_HOOKCALL || record->event > LUA_HOOKTAILCALL)
    abort();
  check(pthread_mutex_lock(&fixture->mutex));
  call = ++fixture->calls[kind];
  inspect = fixture->local_edits == 0;
  check(pthread_cond_broadcast(&fixture->changed));
  if (fixture->mode == BLOCKED && kind == 0 && call == 1) {
    fixture->entered = 1;
    check(pthread_cond_broadcast(&fixture->changed));
    while (!fixture->release)
      check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
    if (!fixture->replacement_done)
      abort();
  }
  check(pthread_mutex_unlock(&fixture->mutex));
  if (fixture->mode == COUNTS) {
    if (record->event != LUA_HOOKCOUNT)
      abort();
    return;
  }
  if (fixture->mode == YIELD) {
    if (record->event != LUA_HOOKCOUNT)
      abort();
    if (call <= 3)
      (void)lua_yield(state, 0);
    else
      lua_sethook(state, NULL, 0, 0);
    return;
  }
  if (inspect)
    inspect_frame(state, record);
  if (call == 1) {
    /* Hook reentry runs Lua with hooks suppressed, and may collect. */
    if (luaL_loadstring(state, "return 6*7") != LUA_OK ||
        lua_pcall(state, 0, 1, 0) != LUA_OK || lua_tointeger(state, -1) != 42)
      abort();
    lua_pop(state, 1);
    (void)lua_gc(state, LUA_GCCOLLECT);
  }
}

static void hook_a(lua_State *state, lua_Debug *record) {
  hook(state, record, 0);
}

static void hook_b(lua_State *state, lua_Debug *record) {
  hook(state, record, 1);
}

static void stop_hook(lua_State *state, lua_Debug *record) {
  if (record->event != LUA_HOOKCOUNT)
    abort();
  lua_sethook(state, NULL, 0, 0);
  (void)luaL_error(state, "expected signal hook stop");
}

static void signal_handler(int number) {
  int previous_errno = errno;
  (void)number;
  lua_sethook(__atomic_load_n(&signal_state, __ATOMIC_ACQUIRE), stop_hook,
              LUA_MASKCOUNT, 1);
  if (sem_post(&signal_seen) != 0)
    _exit(90);
  errno = previous_errno;
}

static int checkpoint(lua_State *state) {
  struct fixture *fixture = lua_touserdata(state, lua_upvalueindex(1));
  unsigned step;
  int finish;
  check(pthread_mutex_lock(&fixture->mutex));
  step = ++fixture->steps;
  finish = fixture->finish;
  check(pthread_mutex_unlock(&fixture->mutex));
  if (fixture->mode == REPLACE && step % 11 == 0) {
    lua_State *child = lua_newthread(state);
    lua_Hook current = lua_gethook(child);
    int mask = lua_gethookmask(child);
    int count = lua_gethookcount(child);
    if ((current != NULL && current != hook_a && current != hook_b) ||
        (mask != 0 && mask != LUA_MASKCOUNT &&
         mask != (LUA_MASKCOUNT | LUA_MASKLINE | LUA_MASKCALL | LUA_MASKRET)) ||
        (count != 0 && count != 1 && count != 17 && count != 257))
      abort();
    check(pthread_mutex_lock(&fixture->mutex));
    fixture->inherited++;
    check(pthread_cond_broadcast(&fixture->changed));
    check(pthread_mutex_unlock(&fixture->mutex));
    lua_pop(state, 1);
  }
  lua_pushboolean(state, finish);
  return 1;
}

static void wait_calls(struct fixture *fixture, unsigned kind, unsigned after) {
  check(pthread_mutex_lock(&fixture->mutex));
  while (fixture->calls[kind] <= after)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  check(pthread_mutex_unlock(&fixture->mutex));
}

static int control(lua_State *state) {
  struct fixture *fixture = lua_touserdata(state, lua_upvalueindex(1));
  lua_State *owner = fixture->actors[0].state;
  unsigned i, before, inherited_before;
  int full = LUA_MASKCOUNT | LUA_MASKLINE | LUA_MASKCALL | LUA_MASKRET;
  lua_sethook(owner, hook_a, full, 1);
  if (fixture->mode == BLOCKED) {
    check(pthread_mutex_lock(&fixture->mutex));
    while (!fixture->entered)
      check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
    check(pthread_mutex_unlock(&fixture->mutex));
    lua_sethook(owner, hook_b, LUA_MASKCOUNT, 1);
    for (i = 0; i < 300; ++i) {
      (void)lua_newuserdatauv(state, 48, 0);
      lua_pop(state, 1);
    }
    (void)lua_gc(state, LUA_GCCOLLECT);
    check(pthread_mutex_lock(&fixture->mutex));
    fixture->replacement_done = fixture->release = 1;
    check(pthread_cond_broadcast(&fixture->changed));
    check(pthread_mutex_unlock(&fixture->mutex));
  } else {
    wait_calls(fixture, 0, 0);
    lua_sethook(owner, hook_b, full, 1);
    wait_calls(fixture, 1, 0);
    check(pthread_mutex_lock(&fixture->mutex));
    inherited_before = fixture->inherited;
    check(pthread_mutex_unlock(&fixture->mutex));
    /* Continue replacing until new children are created during this phase. */
    for (i = 0;; ++i) {
      lua_Hook current;
      int mask, count, inherited;
      lua_sethook(owner, i % 2 ? hook_b : hook_a,
                  i % 3 == 0 ? 0 : (i % 3 == 1 ? LUA_MASKCOUNT : full),
                  i % 3 == 0 ? 257 : (i % 3 == 1 ? 17 : 1));
      current = lua_gethook(owner);
      mask = lua_gethookmask(owner);
      count = lua_gethookcount(owner);
      if ((current != NULL && current != hook_a && current != hook_b) ||
          (mask != 0 && mask != LUA_MASKCOUNT && mask != full) ||
          (count != 1 && count != 17 && count != 257))
        abort();
      (void)lua_newuserdatauv(state, 8, 0);
      lua_pop(state, 1);
      if (i % 101 == 0)
        (void)lua_gc(state, LUA_GCCOLLECT);
      check(pthread_mutex_lock(&fixture->mutex));
      inherited = fixture->inherited - inherited_before >= 8;
      check(pthread_mutex_unlock(&fixture->mutex));
      if (i >= 599 && inherited)
        break;
    }
  }
  check(pthread_mutex_lock(&fixture->mutex));
  before = fixture->calls[1];
  check(pthread_mutex_unlock(&fixture->mutex));
  lua_sethook(owner, hook_b, LUA_MASKCOUNT, 1);
  wait_calls(fixture, 1, before);
  check(pthread_mutex_lock(&fixture->mutex));
  while (fixture->mode == REPLACE &&
         (fixture->inherited == 0 || fixture->local_edits == 0))
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  fixture->finish = 1;
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static void managed_poll(lua_State *state, void *user) {
  struct actor *actor = user;
  struct fixture *fixture = actor->fixture;
  (void)state;
  if (actor->polled)
    return;
  actor->polled = 1;
  if (fixture->mode == SIGNAL) {
    check(pthread_mutex_lock(&fixture->mutex));
    fixture->entered = 1;
    check(pthread_cond_broadcast(&fixture->changed));
    while (!fixture->release)
      check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
    check(pthread_mutex_unlock(&fixture->mutex));
  } else {
    rendezvous(&fixture->managed);
  }
}

static void *run_actor(void *user) {
  struct actor *actor = user;
  do {
    actor->status = lua_resume(actor->state, NULL, 0, &actor->results);
    if (actor->status == LUA_YIELD) {
      if (actor->results != 0)
        abort();
      actor->yields++;
    }
  } while (actor->status == LUA_YIELD);
  return NULL;
}

static int after_hook_install(lua_State *state) {
  pthread_barrier_t *barrier = lua_touserdata(state, lua_upvalueindex(1));
  rendezvous(barrier);
  return 0;
}

static int first_lua_hooks(struct fixture *fixture) {
  static const char source[] =
      "local calls=0; local function target() end; "
      "local function observer() "
      "if debug.getinfo(2,'f').func==target then calls=calls+1 end end; "
      "debug.sethook(observer,'c'); fixture_after_hook_install(); "
      "assert(debug.gethook()==observer,'lost first Lua hook'); "
      "target(); debug.sethook(); assert(calls==1); return true";
  pthread_t carriers[2];
  int failed = 0;

  lua_pushlightuserdata(fixture->main, &fixture->managed);
  lua_pushcclosure(fixture->main, after_hook_install, 1);
  lua_setglobal(fixture->main, "fixture_after_hook_install");
  for (unsigned i = 0; i < 2; ++i) {
    struct actor *actor = &fixture->actors[i];
    actor->state = lua_newthread(fixture->main);
    if (luaL_loadstring(actor->state, source) != LUA_OK)
      abort();
  }
  first_hook_lookup = &fixture->managed;
  for (unsigned i = 0; i < 2; ++i)
    check(pthread_create(&carriers[i], NULL, run_actor, &fixture->actors[i]));
  for (unsigned i = 0; i < 2; ++i) {
    struct actor *actor = &fixture->actors[i];
    check(pthread_join(carriers[i], NULL));
    if (actor->status != LUA_OK || actor->results != 1 ||
        !lua_toboolean(actor->state, -1)) {
      const char *message = lua_tostring(actor->state, -1);
      fprintf(stderr, "Lua hook registrar %u: %s\n", i,
              message != NULL ? message : "invalid result");
      failed = 1;
    }
  }
  first_hook_lookup = NULL;
  return failed;
}

static void *run_counts(void *user) {
  static const int counts[] = {0, -1, INT_MIN, 17};
  struct fixture *fixture = user;
  lua_State *parent = fixture->actors[0].state;
  unsigned i;
  for (i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
    lua_State *child;
    int results;
    unsigned before = fixture->calls[0];
    lua_sethook(parent, hook_a, LUA_MASKCOUNT, counts[i]);
    child = lua_newthread(parent);
    if (lua_gethook(child) != hook_a ||
        lua_gethookmask(child) != LUA_MASKCOUNT ||
        lua_gethookcount(child) != counts[i])
      abort();
    if (luaL_loadstring(child, "local sum=0; for i=1,500 do sum=sum+i end; "
                               "return sum") != LUA_OK ||
        lua_resume(child, parent, 0, &results) != LUA_OK || results != 1 ||
        lua_tointeger(child, -1) != 125250)
      abort();
    if ((counts[i] > 0) != (fixture->calls[0] > before))
      abort();
    lua_pop(parent, 1);
    (void)lua_gc(parent, LUA_GCCOLLECT);
  }
  lua_sethook(parent, hook_a, 0, 17);
  if (lua_gethook(parent) != NULL || lua_gethookmask(parent) != 0)
    abort();
  lua_sethook(parent, NULL, LUA_MASKCOUNT, 1);
  if (lua_gethook(parent) != NULL || lua_gethookmask(parent) != 0)
    abort();
  return NULL;
}

int main(int argc, char **argv) {
  static const char source[] =
      "return function() coroutine.yield(); "
      "local function inner() local hook_probe=7; local sum=0; "
      "for i=1,20 do sum=sum+i end; "
      "assert(hook_probe==7 or hook_probe==11); return sum end; "
      "local total,n=0,0; repeat total=total+inner(); n=n+1 "
      "until fixture_checkpoint(); assert(total==n*210); return n end, "
      "function() coroutine.yield(); fixture_control(); return true end";
  struct fixture fixture;
  struct sigaction action, previous;
  pthread_t carriers[2];
  lua_State *main_state;
  int i, count, first_lua, failed = 0;
  if (argc != 2)
    return 2;
  first_lua = strcmp(argv[1], "first_lua_incremental") == 0 ||
              strcmp(argv[1], "first_lua_generational") == 0;
  memset(&fixture, 0, sizeof(fixture));
  if (strncmp(argv[1], "replace_", 8) == 0)
    fixture.mode = REPLACE;
  else if (strcmp(argv[1], "blocked") == 0)
    fixture.mode = BLOCKED;
  else if (strcmp(argv[1], "signal") == 0)
    fixture.mode = SIGNAL;
  else if (strcmp(argv[1], "yield") == 0)
    fixture.mode = YIELD;
  else if (strcmp(argv[1], "counts") == 0)
    fixture.mode = COUNTS;
  else if (!first_lua)
    return 2;
  active_fixture = &fixture;
  check(pthread_mutex_init(&fixture.mutex, NULL));
  check(pthread_cond_init(&fixture.changed, NULL));
  check(pthread_barrier_init(&fixture.managed, NULL, 2));
  fixture.main = main_state = luaL_newstate();
  if (main_state == NULL)
    abort();
  luaL_openlibs(main_state);
  if (strstr(argv[1], "generational") != NULL)
    (void)lua_gc(main_state, LUA_GCGEN);
  if (first_lua) {
    failed = first_lua_hooks(&fixture);
    goto cleanup;
  }
  lua_pushlightuserdata(main_state, &fixture);
  lua_pushcclosure(main_state, checkpoint, 1);
  lua_setglobal(main_state, "fixture_checkpoint");
  lua_pushlightuserdata(main_state, &fixture);
  lua_pushcclosure(main_state, control, 1);
  lua_setglobal(main_state, "fixture_control");
  if (fixture.mode == COUNTS) {
    fixture.actors[0].state = lua_newthread(main_state);
    check(pthread_create(&carriers[0], NULL, run_counts, &fixture));
    check(pthread_join(carriers[0], NULL));
    goto cleanup;
  }
  if (luaL_loadstring(main_state, source) != LUA_OK ||
      lua_pcall(main_state, 0, 2, 0) != LUA_OK)
    abort();
  count = fixture.mode == REPLACE || fixture.mode == BLOCKED ? 2 : 1;
  for (i = 0; i < count; ++i) {
    struct actor *actor = &fixture.actors[i];
    int results;
    actor->fixture = &fixture;
    actor->state = lua_newthread(main_state);
    lua_pushvalue(main_state, i + 1);
    lua_xmove(main_state, actor->state, 1);
    if (lua_resume(actor->state, NULL, 0, &results) != LUA_YIELD ||
        results != 0)
      abort();
    if (fixture.mode != YIELD) {
      actor->state->state_poll = managed_poll;
      actor->state->state_poll_context = actor;
      actor->state->poll_remaining = 1;
    }
  }
  if (fixture.mode == SIGNAL) {
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    check(sigemptyset(&action.sa_mask));
    check(sigaction(SIGUSR1, &action, &previous));
    check(sem_init(&signal_seen, 0, 0));
    __atomic_store_n(&signal_state, fixture.actors[0].state, __ATOMIC_RELEASE);
  } else if (fixture.mode == YIELD) {
    fixture.finish = 1;
    lua_sethook(fixture.actors[0].state, hook_a, LUA_MASKCOUNT, 1);
  }
  for (i = 0; i < count; ++i)
    check(pthread_create(&carriers[i], NULL, run_actor, &fixture.actors[i]));
  if (fixture.mode == SIGNAL) {
    check(pthread_mutex_lock(&fixture.mutex));
    while (!fixture.entered)
      check(pthread_cond_wait(&fixture.changed, &fixture.mutex));
    check(pthread_mutex_unlock(&fixture.mutex));
    check(pthread_kill(carriers[0], SIGUSR1));
    while (sem_wait(&signal_seen) != 0)
      if (errno != EINTR)
        abort();
    check(pthread_mutex_lock(&fixture.mutex));
    fixture.release = 1;
    check(pthread_cond_broadcast(&fixture.changed));
    check(pthread_mutex_unlock(&fixture.mutex));
  }
  for (i = 0; i < count; ++i)
    check(pthread_join(carriers[i], NULL));
  if (fixture.mode == SIGNAL) {
    failed |= fixture.actors[0].status != LUA_ERRRUN ||
              strstr(lua_tostring(fixture.actors[0].state, -1),
                     "expected signal hook stop") == NULL;
    check(sigaction(SIGUSR1, &previous, NULL));
    check(sem_destroy(&signal_seen));
  } else {
    for (i = 0; i < count; ++i)
      failed |=
          fixture.actors[i].status != LUA_OK || fixture.actors[i].results != 1;
    failed |= fixture.steps == 0;
    if (fixture.mode == YIELD)
      failed |= fixture.actors[0].yields != 3 || fixture.calls[0] != 4;
    else {
      failed |= fixture.calls[0] == 0 || fixture.calls[1] == 0 ||
                fixture.local_edits == 0;
      if (fixture.mode == REPLACE)
        failed |= fixture.inherited == 0;
    }
  }
cleanup:
  lua_close(main_state);
  check(pthread_barrier_destroy(&fixture.managed));
  check(pthread_cond_destroy(&fixture.changed));
  check(pthread_mutex_destroy(&fixture.mutex));
  if (!failed)
    printf("parallel hook %s: safe publication, dispatch and cleanup\n",
           argv[1]);
  return failed;
}
