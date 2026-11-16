#include <lauxlib.h>
#include <lstate.h>
#include <lua.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct context {
  lua_State *state;
  lua_State *collector;
  int reject_growth;
  int allocation_failures;
  int oom_case;
  int forward_case;
  int forward_gc_pending;
  int injected_collections;
  int finalized;
  int warnings;
  int failed;
};

static struct context *active_context;

void __real_luaX_lockstate(lua_State *state);

void __wrap_luaX_lockstate(lua_State *state) {
  struct context *context = active_context;
  if (context != NULL && context->forward_gc_pending &&
      state == context->state) {
    context->forward_gc_pending = 0;
    /* Force real collection between source detach and destination entry. The
     * error's coroutine has no ordinary Lua root during this exact handoff. */
    (void)lua_gc(context->collector, LUA_GCCOLLECT);
    (void)lua_gc(context->collector, LUA_GCCOLLECT);
    context->injected_collections += 2;
    if (context->finalized != 0)
      abort();
  }
  __real_luaX_lockstate(state);
}

static void *allocate(void *user, void *pointer, size_t old_size, size_t size) {
  struct context *context = user;
  if (size == 0) {
    free(pointer);
    return NULL;
  }
  if (context->reject_growth && (pointer == NULL || size > old_size)) {
    context->allocation_failures++;
    return NULL;
  }
  return realloc(pointer, size);
}

static int finalize(lua_State *state) {
  struct context *context = *(struct context **)lua_touserdata(state, 1);
  context->finalized++;
  if (context->oom_case) {
    context->reject_growth = 1;
    (void)lua_newuserdatauv(state, 4096, 0);
    abort(); /* the allocator must have rejected this growth */
  }
  lua_pushliteral(state, "expected finalizer failure");
  return lua_error(state);
}

static void warn_then_throw(void *user, const char *message, int continuation) {
  struct context *context = user;
  lua_State *state = context->state;
  (void)message;
  (void)continuation;
  context->warnings++;
  context->reject_growth = 0;
  lua_setwarnf(state, NULL, NULL);
  lua_pushlightuserdata(state, context);
  (void)lua_error(state); /* abandon the complete collector/native call chain */
  abort();
}

static int collect(lua_State *state) {
  (void)lua_gc(state, LUA_GCCOLLECT);
  return 0;
}

static int finalize_forwarded(lua_State *state) {
  struct context *context = *(struct context **)lua_touserdata(state, 1);
  context->finalized++;
  return 0;
}

static int forward_error(lua_State *state) {
  struct context *context = active_context;
  lua_State *child = lua_newthread(state);
  struct context **resource = lua_newuserdatauv(child, sizeof(*resource), 0);
  *resource = context;
  luaL_setmetatable(child, "forwarded.resource");
  lua_pop(state, 1); /* remove the last ordinary reference to the child */
  context->forward_gc_pending = 1;
  return lua_error(child);
}

static int exercise_forwarding(lua_State *state, struct context *context) {
  int status;
  context->collector = lua_newthread(state);
  (void)luaL_ref(state, LUA_REGISTRYINDEX);
  luaL_newmetatable(state, "forwarded.resource");
  lua_pushcfunction(state, finalize_forwarded);
  lua_setfield(state, -2, "__gc");
  lua_pop(state, 1);
  lua_pushcfunction(state, forward_error);
  status = lua_pcall(state, 0, 0, 0);
  if (status != LUA_ERRRUN || lua_type(state, -1) != LUA_TUSERDATA)
    return 1;
  if (*(struct context **)lua_touserdata(state, -1) != context ||
      context->injected_collections != 2 || context->finalized != 0)
    return 1;
  lua_settop(state, 0);
  (void)lua_gc(state, LUA_GCCOLLECT);
  return context->finalized != 1 || state->native_depth != 0 ||
         state->world_depth != 0 || luaW_activecount(&G(state)->world) != 0 ||
         G(state)->world.stopped || G(state)->world.heap_depth != 0 ||
         state->heap_waiting != 0;
}

static void *run(void *user) {
  struct context *context = user;
  lua_State *state = lua_newstate(allocate, context, 0);
  struct context **resource;
  int status;
  if (state == NULL) {
    context->failed = 1;
    return NULL;
  }
  context->state = state;
  active_context = context;
  (void)lua_gc(state, LUA_GCSTOP);
  if (context->forward_case) {
    context->failed = exercise_forwarding(state, context);
    lua_close(state);
    return NULL;
  }
  resource = lua_newuserdatauv(state, sizeof(*resource), 0);
  *resource = context;
  lua_createtable(state, 0, 1);
  lua_pushcfunction(state, finalize);
  lua_setfield(state, -2, "__gc");
  (void)lua_setmetatable(state, -2);
  lua_pop(state, 1);
  lua_setwarnf(state, warn_then_throw, context);
  lua_pushcfunction(state, collect);
  status = lua_pcall(state, 0, 0, 0);
  context->failed =
      status != LUA_ERRRUN || lua_touserdata(state, -1) != context ||
      context->finalized != 1 || context->warnings != 1 ||
      (context->oom_case && context->allocation_failures == 0) ||
      state->native_depth != 0 || state->world_depth != 0 ||
      luaW_activecount(&G(state)->world) != 0 || G(state)->world.stopped ||
      G(state)->world.heap_depth != 0 || state->heap_waiting != 0 ||
      G(state)->gcemergency != 0 || G(state)->gcstopem != 0;
  context->reject_growth = 0;
  lua_settop(state, 0);
  context->failed |= lua_gc(state, LUA_GCCOLLECT) != 0;
  context->failed |= luaL_dostring(state, "return 6*7") != LUA_OK;
  context->failed |= lua_tointeger(state, -1) != 42;
  lua_close(state);
  context->failed |= context->finalized != 1;
  return NULL;
}

int main(int argc, char **argv) {
  struct context context;
  pthread_t worker;
  if (argc != 2)
    return 2;
  memset(&context, 0, sizeof(context));
  context.oom_case = strcmp(argv[1], "warning_oom") == 0;
  context.forward_case = strcmp(argv[1], "forwarded_gc") == 0;
  if (pthread_create(&worker, NULL, run, &context) != 0)
    return 1;
  if (pthread_join(worker, NULL) != 0)
    return 1;
  if (!context.failed)
    printf("world unwind %s: collector/native scopes retired, VM reusable\n",
           argv[1]);
  return context.failed;
}
