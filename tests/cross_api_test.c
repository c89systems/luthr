#define _GNU_SOURCE
#include <lauxlib.h>
#include <ldo.h>
#include <lgc.h>
#include <lstate.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum mode {
  MOVES,
  WAIT_MOVE,
  WAIT_RESUME,
  WAIT_CLOSE,
  FOREIGN_ERROR,
  FORWARD_CLOSE
};
struct fixture;
struct actor {
  struct fixture *fixture;
  lua_State *state;
  int polled;
  int status;
  int results;
};
struct resource {
  struct fixture *fixture;
  int id;
  unsigned magic;
};
struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_barrier_t managed;
  lua_State *main;
  lua_State *boxes[2];
  struct actor actors[2];
  pthread_cond_t *watched;
  enum mode mode;
  int tokens;
  int batch;
  int wait_seen;
  int worker_done;
  int entered;
  int release;
  int panic_seen;
  int failed;
  int finalized;
  int closed;
  int close_active;
  unsigned close_gc_frees;
  global_State *global;
  size_t live_bytes;
  pthread_t error_carrier;
  jmp_buf panic_target;
};

static struct fixture *active_fixture;
int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);

static void check(int status) {
  if (status != 0)
    abort();
}

int __wrap_pthread_cond_wait(pthread_cond_t *condition,
                             pthread_mutex_t *mutex) {
  struct fixture *fixture = active_fixture;
  if (fixture != NULL && condition == fixture->watched) {
    check(pthread_mutex_lock(&fixture->mutex));
    fixture->wait_seen = 1;
    check(pthread_cond_broadcast(&fixture->changed));
    check(pthread_mutex_unlock(&fixture->mutex));
  }
  return __real_pthread_cond_wait(condition, mutex);
}

static void *allocate(void *user, void *pointer, size_t old_size, size_t size) {
  struct fixture *fixture = user;
  void *replacement;
  if (pointer == NULL)
    old_size = 0;
  if (size == 0) {
    if (fixture->close_active && fixture->global->world.stopped)
      fixture->close_gc_frees++;
    fixture->live_bytes -= old_size;
    free(pointer);
    return NULL;
  }
  replacement = malloc(size);
  if (replacement != NULL) {
    if (pointer != NULL) {
      memcpy(replacement, pointer, old_size < size ? old_size : size);
      free(pointer);
    }
    fixture->live_bytes = fixture->live_bytes - old_size + size;
  }
  return replacement;
}

static int finalize(lua_State *state) {
  struct resource *resource = lua_touserdata(state, 1);
  struct fixture *fixture = resource->fixture;
  if (resource->magic != 0x1234abcdU)
    abort();
  resource->magic = 0;
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->finalized++;
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static void push_token(lua_State *state, struct fixture *fixture, int id) {
  struct resource *resource;
  lua_createtable(state, 0, 2);
  lua_pushinteger(state, id);
  lua_setfield(state, -2, "id");
  resource = lua_newuserdatauv(state, sizeof(*resource), 0);
  resource->fixture = fixture;
  resource->id = id;
  resource->magic = 0x1234abcdU;
  luaL_setmetatable(state, "cross.resource");
  lua_setfield(state, -2, "resource");
  fixture->tokens++;
}

static int move_work(lua_State *state) {
  struct fixture *fixture = lua_touserdata(state, lua_upvalueindex(1));
  int role = (int)lua_tointeger(state, 1);
  int i;
  for (i = 0; i < 180; i += fixture->batch) {
    lua_xmove(fixture->boxes[role], fixture->boxes[1 - role], fixture->batch);
    if (i % 17 == 0)
      (void)lua_gc(state, LUA_GCCOLLECT);
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
  return NULL;
}

static int token_id(lua_State *state, int index) {
  struct resource *resource;
  int id;
  if (!lua_istable(state, index))
    abort();
  lua_getfield(state, index, "id");
  id = (int)lua_tointeger(state, -1);
  lua_pop(state, 1);
  lua_getfield(state, index, "resource");
  resource = lua_touserdata(state, -1);
  if (resource == NULL || resource->id != id || resource->magic != 0x1234abcdU)
    abort();
  lua_pop(state, 1);
  return id;
}

static int exercise_moves(struct fixture *fixture) {
  static const char source[] =
      "return function() coroutine.yield(); fixture_move(0); return true end, "
      "function() coroutine.yield(); fixture_move(1); return true end";
  pthread_t carriers[2];
  unsigned char seen[361] = {0};
  int i, side, failed = 0;
  for (side = 0; side < 2; ++side) {
    lua_State *box = fixture->boxes[side];
    if (!lua_checkstack(box, 368))
      abort();
    for (i = 1; i <= 180; ++i)
      push_token(box, fixture, side * 180 + i);
  }
  lua_pushlightuserdata(fixture->main, fixture);
  lua_pushcclosure(fixture->main, move_work, 1);
  lua_setglobal(fixture->main, "fixture_move");
  if (luaL_loadstring(fixture->main, source) != LUA_OK ||
      lua_pcall(fixture->main, 0, 2, 0) != LUA_OK)
    abort();
  for (side = 0; side < 2; ++side) {
    struct actor *actor = &fixture->actors[side];
    int results;
    actor->fixture = fixture;
    actor->state = lua_newthread(fixture->main);
    lua_pushvalue(fixture->main, side + 3);
    lua_xmove(fixture->main, actor->state, 1);
    if (lua_resume(actor->state, NULL, 0, &results) != LUA_YIELD ||
        results != 0)
      abort();
    actor->state->state_poll = managed_poll;
    actor->state->state_poll_context = actor;
    actor->state->poll_remaining = 1;
  }
  for (side = 0; side < 2; ++side)
    check(pthread_create(&carriers[side], NULL, run_actor,
                         &fixture->actors[side]));
  for (side = 0; side < 2; ++side)
    check(pthread_join(carriers[side], NULL));
  for (side = 0; side < 2; ++side) {
    struct actor *actor = &fixture->actors[side];
    lua_State *box = fixture->boxes[side];
    failed |= actor->status != LUA_OK || actor->results != 1 ||
              !actor->polled || !lua_toboolean(actor->state, -1);
    if (lua_gettop(box) != 180)
      abort();
    for (i = 1; i <= 180; ++i) {
      int id = token_id(box, i);
      if (id < 1 || id > 360 || seen[id]++)
        abort();
      if (fixture->batch == 3 && (i - 1) % 3 != (id - 1) % 3)
        abort();
    }
  }
  return failed || fixture->finalized != 0;
}

static void *waiting_operation(void *user) {
  struct fixture *fixture = user;
  if (fixture->mode == WAIT_MOVE)
    lua_xmove(fixture->boxes[0], fixture->boxes[1], 1);
  else if (fixture->mode == WAIT_RESUME) {
    int results;
    int status = lua_resume(fixture->boxes[1], fixture->boxes[0], 0, &results);
    fixture->failed |= status != LUA_OK || results != 1 ||
                       lua_tointeger(fixture->boxes[1], -1) != 7;
  } else
    fixture->failed |=
        lua_closethread(fixture->boxes[1], fixture->boxes[0]) != LUA_OK;
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->worker_done = 1;
  check(pthread_cond_broadcast(&fixture->changed));
  check(pthread_mutex_unlock(&fixture->mutex));
  return NULL;
}

static int exercise_wait(struct fixture *fixture) {
  pthread_t carrier;
  lua_State *controller;
  uintptr_t before;
  int seen;
  push_token(fixture->boxes[0], fixture, 1);
  if (fixture->mode == WAIT_RESUME) {
    if (luaL_loadstring(fixture->boxes[1], "return 7") != LUA_OK)
      abort();
  } else
    push_token(fixture->boxes[1], fixture, 2);
  /* Extra capacity is deliberately outside the declared live frame extent. */
  luaX_lockstate(fixture->boxes[0]);
  if (!luaD_growstack(fixture->boxes[0], 4096, 0))
    abort();
  luaX_unlockstate(fixture->boxes[0]);
  controller = fixture->boxes[fixture->mode == WAIT_MOVE ? 1 : 0];
  fixture->watched = &controller->stack_gate.changed;
  luaX_lockstate(controller);
  before = (uintptr_t)fixture->boxes[0]->stack.p;
  check(pthread_create(&carrier, NULL, waiting_operation, fixture));
  check(pthread_mutex_lock(&fixture->mutex));
  while (!fixture->wait_seen && !fixture->worker_done)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  seen = fixture->wait_seen;
  check(pthread_mutex_unlock(&fixture->mutex));
  /* A waiter owning another reservation must not prevent real collection. */
  luaC_fullgc(controller, 0);
  if ((uintptr_t)fixture->boxes[0]->stack.p == before ||
      fixture->finalized != 0)
    abort();
  luaX_unlockstate(controller);
  check(pthread_join(carrier, NULL));
  if (fixture->mode == WAIT_MOVE) {
    if (lua_gettop(fixture->boxes[0]) != 0 ||
        lua_gettop(fixture->boxes[1]) != 2 ||
        token_id(fixture->boxes[1], 1) != 2 ||
        token_id(fixture->boxes[1], 2) != 1)
      abort();
  } else if (fixture->mode == WAIT_CLOSE && lua_gettop(fixture->boxes[1]) != 0)
    abort();
  return fixture->failed || !seen;
}

static int hold_handler(lua_State *state) {
  struct fixture *fixture = lua_touserdata(state, lua_upvalueindex(1));
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->entered = 1;
  check(pthread_cond_broadcast(&fixture->changed));
  while (!fixture->release)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static void *handler_owner(void *user) {
  struct fixture *fixture = user;
  lua_pushlightuserdata(fixture->main, fixture);
  lua_pushcclosure(fixture->main, hold_handler, 1);
  fixture->actors[0].status = lua_pcall(fixture->main, 0, 0, 0);
  return NULL;
}

static int panic_here(lua_State *state) {
  struct fixture *fixture = active_fixture;
  if (!pthread_equal(fixture->error_carrier, pthread_self()) ||
      state != fixture->boxes[0] || state->native_depth != 0 ||
      state->native_roots != NULL || state->object_scopes != NULL)
    abort();
  longjmp(fixture->panic_target, 1);
}

static void *unprotected_error(void *user) {
  struct fixture *fixture = user;
  lua_State *state = fixture->boxes[0];
  fixture->error_carrier = pthread_self();
  if (setjmp(fixture->panic_target) == 0) {
    lua_pushliteral(state, "expected unprotected child error");
    (void)lua_error(state);
    abort();
  }
  fixture->panic_seen = 1;
  if (lua_status(state) != LUA_ERRRUN ||
      strcmp(lua_tostring(state, -1), "expected unprotected child error") != 0)
    abort();
  lua_settop(state, 0);
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->release = 1;
  check(pthread_cond_broadcast(&fixture->changed));
  check(pthread_mutex_unlock(&fixture->mutex));
  return NULL;
}

static int exercise_foreign_error(struct fixture *fixture) {
  pthread_t owner, source;
  (void)lua_atpanic(fixture->main, panic_here);
  check(pthread_create(&owner, NULL, handler_owner, fixture));
  check(pthread_mutex_lock(&fixture->mutex));
  while (!fixture->entered)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  check(pthread_mutex_unlock(&fixture->mutex));
  check(pthread_create(&source, NULL, unprotected_error, fixture));
  check(pthread_join(source, NULL));
  check(pthread_join(owner, NULL));
  return !fixture->panic_seen || fixture->actors[0].status != LUA_OK;
}

static int close_ack(lua_State *state) {
  struct resource *resource = lua_touserdata(state, 1);
  if (resource->magic != 0x1234abcdU || lua_tointeger(state, 2) != 2001000)
    abort();
  resource->fixture->closed++;
  resource->fixture->close_active = 0;
  return 0;
}

static int error_with_closer(lua_State *state) {
  struct fixture *fixture = active_fixture;
  lua_State *child = lua_newthread(state);
  struct resource *resource = lua_newuserdatauv(child, sizeof(*resource), 0);
  resource->fixture = fixture;
  resource->id = 1;
  resource->magic = 0x1234abcdU;
  fixture->tokens++;
  luaL_setmetatable(child, "cross.close");
  lua_toclose(child, 1);
  lua_pushliteral(child, "expected forwarded close error");
  lua_pop(state, 1); /* the entered error path must retain the child itself */
  fixture->close_active = 1;
  return lua_error(child);
}

static int exercise_forward_close(struct fixture *fixture) {
  lua_State *state = fixture->main;
  int status;
  lua_pushcfunction(state, close_ack);
  lua_setglobal(state, "fixture_close_ack");
  luaL_newmetatable(state, "cross.close");
  lua_pushcfunction(state, finalize);
  lua_setfield(state, -2, "__gc");
  if (luaL_loadstring(state,
                      "return function(self) local sum=0; for i=1,2000 do "
                      "local t={i}; sum=sum+t[1] end; "
                      "fixture_close_ack(self,sum) end") != LUA_OK ||
      lua_pcall(state, 0, 1, 0) != LUA_OK)
    abort();
  lua_setfield(state, -2, "__close");
  lua_pop(state, 1);
  lua_pushcfunction(state, error_with_closer);
  status = lua_pcall(state, 0, 0, 0);
  if (status != LUA_ERRRUN || fixture->closed != 1 ||
      fixture->close_gc_frees == 0 ||
      strcmp(lua_tostring(state, -1), "expected forwarded close error") != 0)
    abort();
  lua_pop(state, 1);
  return 0;
}

int main(int argc, char **argv) {
  struct fixture fixture;
  lua_State *main_state;
  int failed;
  if (argc != 2)
    return 2;
  memset(&fixture, 0, sizeof(fixture));
  fixture.batch = 1;
  if (strncmp(argv[1], "moves_", 6) == 0) {
    fixture.mode = MOVES;
    fixture.batch = strstr(argv[1], "bulk") != NULL ? 3 : 1;
  } else if (strcmp(argv[1], "move_wait_gc") == 0)
    fixture.mode = WAIT_MOVE;
  else if (strcmp(argv[1], "resume_parent_gc") == 0)
    fixture.mode = WAIT_RESUME;
  else if (strcmp(argv[1], "close_parent_gc") == 0)
    fixture.mode = WAIT_CLOSE;
  else if (strcmp(argv[1], "foreign_error") == 0)
    fixture.mode = FOREIGN_ERROR;
  else if (strcmp(argv[1], "forward_close_gc") == 0)
    fixture.mode = FORWARD_CLOSE;
  else
    return 2;
  check(pthread_mutex_init(&fixture.mutex, NULL));
  check(pthread_cond_init(&fixture.changed, NULL));
  check(pthread_barrier_init(&fixture.managed, NULL, 2));
  active_fixture = &fixture;
  fixture.main = main_state = lua_newstate(allocate, &fixture, 0);
  if (main_state == NULL)
    abort();
  fixture.global = G(main_state);
  luaL_openlibs(main_state);
  luaL_newmetatable(main_state, "cross.resource");
  lua_pushcfunction(main_state, finalize);
  lua_setfield(main_state, -2, "__gc");
  lua_pop(main_state, 1);
  fixture.boxes[0] = lua_newthread(main_state);
  fixture.boxes[1] = lua_newthread(main_state);
  if (fixture.mode == WAIT_MOVE &&
      (uintptr_t)fixture.boxes[0] > (uintptr_t)fixture.boxes[1]) {
    lua_State *swap = fixture.boxes[0];
    fixture.boxes[0] = fixture.boxes[1];
    fixture.boxes[1] = swap;
  }
  if (strstr(argv[1], "generational") != NULL)
    (void)lua_gc(main_state, LUA_GCGEN);
  if (fixture.mode == MOVES)
    failed = exercise_moves(&fixture);
  else if (fixture.mode == FOREIGN_ERROR)
    failed = exercise_foreign_error(&fixture);
  else if (fixture.mode == FORWARD_CLOSE)
    failed = exercise_forward_close(&fixture);
  else
    failed = exercise_wait(&fixture);
  lua_settop(fixture.boxes[0], 0);
  lua_settop(fixture.boxes[1], 0);
  (void)lua_gc(main_state, LUA_GCCOLLECT);
  failed |= fixture.finalized != fixture.tokens;
  lua_close(main_state);
  failed |= fixture.live_bytes != 0 || fixture.finalized != fixture.tokens;
  check(pthread_barrier_destroy(&fixture.managed));
  check(pthread_cond_destroy(&fixture.changed));
  check(pthread_mutex_destroy(&fixture.mutex));
  if (!failed)
    printf("cross-state API %s: correct values, collection and cleanup\n",
           argv[1]);
  return failed;
}
