#include <laccess.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lstate.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fixture {
  lua_State *main;
  lua_State *worker;
  void *old_stack;
  void *retired_stack;
  char *bytecode;
  size_t bytecode_size;
  unsigned collections;
  unsigned live_allocations;
  int entry_case;
  int rawset_case;
  int selfcopy_case;
  int vm_case;
  const char *mode;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  int snapshot_ready;
  int replacement_ready;
  int length_case;
  int armed;
  int failed;
};

static struct fixture *active_fixture;

void __real_luaX_lockstate(lua_State *state);
void __real_luaX_unlockstate(lua_State *state);
void __real_luaA_end(lua_State *state, LuaObjectScope *scope);

static void check(int status) {
  if (status != 0)
    abort();
}

void __wrap_luaA_end(lua_State *state, LuaObjectScope *scope) {
  struct fixture *fixture = active_fixture;
  __real_luaA_end(state, scope);
  if (fixture != NULL && fixture->selfcopy_case && fixture->armed &&
      state == fixture->worker) {
    fixture->armed = 0;
    check(pthread_mutex_lock(&fixture->mutex));
    fixture->snapshot_ready = 1;
    check(pthread_cond_broadcast(&fixture->changed));
    while (!fixture->replacement_ready)
      check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
    check(pthread_mutex_unlock(&fixture->mutex));
  }
}

static void collect_at_boundary(struct fixture *fixture) {
  uintptr_t original = (uintptr_t)fixture->old_stack;
  fixture->armed = 0;
  /* A real collection shrinks the parked VM or native frame's unused stack. The
   * retired mapping stays allocated but is cleared, making stale slot reads
   * deterministic without depending on the host realloc implementation. */
  if (!fixture->vm_case && fixture->worker->native_depth == 0)
    abort();
  (void)lua_gc(fixture->main, LUA_GCCOLLECT);
  fixture->collections++;
  if ((uintptr_t)fixture->worker->stack.p == original ||
      fixture->retired_stack == NULL)
    abort();
}

void __wrap_luaX_lockstate(lua_State *state) {
  struct fixture *fixture = active_fixture;
  if (fixture != NULL && fixture->armed && fixture->entry_case &&
      state == fixture->worker)
    collect_at_boundary(fixture);
  __real_luaX_lockstate(state);
}

void __wrap_luaX_unlockstate(lua_State *state) {
  struct fixture *fixture = active_fixture;
  __real_luaX_unlockstate(state);
  if (fixture != NULL && fixture->armed && !fixture->entry_case &&
      state == fixture->worker && (!fixture->vm_case || isLua(state->ci)))
    collect_at_boundary(fixture);
}

static void *allocate(void *user, void *pointer, size_t old_size, size_t size) {
  struct fixture *fixture = user;
  void *replacement;
  if (size == 0) {
    if (pointer != NULL) {
      fixture->live_allocations--;
      free(pointer);
    }
    return NULL;
  }
  replacement = malloc(size);
  if (replacement == NULL)
    return NULL;
  if (pointer == NULL)
    fixture->live_allocations++;
  else {
    memcpy(replacement, pointer, old_size < size ? old_size : size);
    if (fixture->old_stack != NULL &&
        (uintptr_t)fixture->old_stack >= (uintptr_t)pointer &&
        (uintptr_t)fixture->old_stack - (uintptr_t)pointer < old_size &&
        fixture->retired_stack == NULL) {
      fixture->retired_stack = pointer;
      memset(pointer, 0, old_size);
    } else
      free(pointer);
  }
  return replacement;
}

static int write_bytecode(lua_State *state, const void *data, size_t size,
                          void *user) {
  struct fixture *fixture = user;
  char *replacement;
  if (luaW_scope(state).heap != 0 || state->world_depth != 0)
    abort();
  replacement = realloc(fixture->bytecode, fixture->bytecode_size + size);
  if (replacement == NULL && size != 0)
    return 1;
  fixture->bytecode = replacement;
  if (size != 0)
    memcpy(fixture->bytecode + fixture->bytecode_size, data, size);
  fixture->bytecode_size += size;
  return 0;
}

static int exercise(lua_State *state) {
  struct fixture *fixture = active_fixture;
  if (fixture->selfcopy_case) {
    fixture->armed = 1;
    lua_copy(state, lua_upvalueindex(1), lua_upvalueindex(1));
    if (fixture->armed || lua_tointeger(state, lua_upvalueindex(1)) != 42)
      abort();
    lua_pushboolean(state, 1);
    return 1;
  }
  luaX_lockstate(state);
  /* Grow spare capacity without making it part of the current frame's
   * declared live extent, so ordinary GC is allowed to shrink it again. */
  if (!luaD_growstack(state, 8192, 0))
    abort();
  fixture->old_stack = state->stack.p;
  luaX_unlockstate(state);
  if (fixture->rawset_case) {
    lua_pushliteral(state, "answer");
    lua_pushinteger(state, 42);
  }
  fixture->armed = 1;
  if (fixture->vm_case)
    return 0; /* The next VM allocation handoff is the collection boundary. */
  if (fixture->rawset_case) {
    lua_rawset(state, 1);
    lua_getfield(state, 1, "answer");
    if (lua_tointeger(state, -1) != 42)
      abort();
    lua_pop(state, 1);
  } else if (fixture->entry_case) {
    if (lua_dump(state, write_bytecode, fixture, 0) != 0 ||
        fixture->bytecode_size == 0)
      abort();
    if (luaL_loadbufferx(state, fixture->bytecode, fixture->bytecode_size,
                         "ownership", "b") != LUA_OK)
      abort();
    lua_call(state, 0, 1);
    if (lua_tointeger(state, -1) != 42)
      abort();
    lua_pop(state, 1);
  } else {
    size_t length = 0;
    const char *result =
        lua_tolstring(state, 1, fixture->length_case ? &length : NULL);
    if (result == NULL || strcmp(result, "retained string") != 0 ||
        (fixture->length_case && length != strlen("retained string")))
      abort();
  }
  if (fixture->armed || fixture->collections != 1)
    abort();
  free(fixture->retired_stack);
  fixture->retired_stack = NULL;
  fixture->old_stack = NULL;
  lua_pushboolean(state, 1);
  return 1;
}

static void *run(void *user) {
  struct fixture *fixture = user;
  int results, status;
  fixture->main = lua_newstate(allocate, fixture, 0);
  if (fixture->main == NULL)
    abort();
  fixture->worker = lua_newthread(fixture->main);
  if (fixture->vm_case) {
    const char *source =
        strstr(fixture->mode, "newtable") != NULL
            ? "grow(); local t={42}; local flag=false; "
              "return flag==false and t[1]==42"
        : strstr(fixture->mode, "concat") != NULL
            ? "local a,b=...; grow(); local s=a..b; local flag=false; "
              "return flag==false and s=='leftright'"
            : "grow(); local function f() return 42 end; local flag=false; "
              "return flag==false and f()==42";
    lua_pushcfunction(fixture->main, exercise);
    lua_setglobal(fixture->main, "grow");
    if (strstr(fixture->mode, "generational") != NULL)
      (void)lua_gc(fixture->main, LUA_GCGEN);
    (void)lua_gc(fixture->main, LUA_GCSTOP);
    if (luaL_loadstring(fixture->worker, source) != LUA_OK)
      abort();
    lua_pushliteral(fixture->worker, "left");
    lua_pushliteral(fixture->worker, "right");
  } else if (fixture->selfcopy_case) {
    lua_pushinteger(fixture->main, 7);
    lua_pushcclosure(fixture->main, exercise, 1);
    lua_pushvalue(fixture->main, -1);
    lua_xmove(fixture->main, fixture->worker, 1);
  } else
    lua_pushcfunction(fixture->worker, exercise);
  if (fixture->vm_case || fixture->selfcopy_case) {
    /* Arguments were prepared above; self-copy retains its closure on main. */
  } else if (fixture->rawset_case)
    lua_newtable(fixture->worker);
  else if (fixture->entry_case) {
    if (luaL_loadstring(fixture->worker, "return 42") != LUA_OK)
      abort();
  } else
    lua_pushliteral(fixture->worker, "retained string");
  status = lua_resume(fixture->worker, NULL,
                      fixture->vm_case         ? 2
                      : fixture->selfcopy_case ? 0
                                               : 1,
                      &results);
  fixture->failed =
      status != LUA_OK || results != 1 || !lua_toboolean(fixture->worker, -1);
  if (fixture->vm_case) {
    fixture->failed |= fixture->armed || fixture->collections != 1;
    free(fixture->retired_stack);
    fixture->retired_stack = fixture->old_stack = NULL;
  }
  lua_close(fixture->main);
  fixture->failed |= fixture->live_allocations != 0;
  return NULL;
}

int main(int argc, char **argv) {
  struct fixture fixture;
  pthread_t carrier;
  if (argc != 2)
    return 2;
  memset(&fixture, 0, sizeof(fixture));
  fixture.mode = argv[1];
  fixture.vm_case = strncmp(argv[1], "vm_", 3) == 0;
  fixture.rawset_case = strcmp(argv[1], "rawset_entry") == 0;
  fixture.selfcopy_case = strcmp(argv[1], "capture_selfcopy") == 0;
  fixture.entry_case =
      fixture.rawset_case || strcmp(argv[1], "dump_entry") == 0;
  fixture.length_case = strcmp(argv[1], "string_length") == 0;
  if (!fixture.vm_case && !fixture.entry_case && !fixture.length_case &&
      !fixture.selfcopy_case && strcmp(argv[1], "string_pointer") != 0)
    return 2;
  active_fixture = &fixture;
  check(pthread_mutex_init(&fixture.mutex, NULL));
  check(pthread_cond_init(&fixture.changed, NULL));
  if (pthread_create(&carrier, NULL, run, &fixture) != 0)
    abort();
  if (fixture.selfcopy_case) {
    check(pthread_mutex_lock(&fixture.mutex));
    while (!fixture.snapshot_ready)
      check(pthread_cond_wait(&fixture.changed, &fixture.mutex));
    check(pthread_mutex_unlock(&fixture.mutex));
    lua_pushinteger(fixture.main, 42);
    if (lua_setupvalue(fixture.main, 2, 1) == NULL)
      abort();
    check(pthread_mutex_lock(&fixture.mutex));
    fixture.replacement_ready = 1;
    check(pthread_cond_broadcast(&fixture.changed));
    check(pthread_mutex_unlock(&fixture.mutex));
  }
  check(pthread_join(carrier, NULL));
  check(pthread_cond_destroy(&fixture.changed));
  check(pthread_mutex_destroy(&fixture.mutex));
  free(fixture.bytecode);
  if (!fixture.failed)
    printf("API ownership %s: protected observable result\n", argv[1]);
  return fixture.failed;
}
