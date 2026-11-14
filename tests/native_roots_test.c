#include <lauxlib.h>
#include <lroots.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct context {
  int finalized;
  int reject_growth;
  unsigned allocations;
  lua_State *child;
  int child_ref;
  jmp_buf *panic_target;
  LuaNativeRoot *published;
};

struct resource {
  struct context *context;
  unsigned magic;
};

struct run {
  const char *name;
  int failed;
};

#define REQUIRE(L, expression)                                                 \
  do {                                                                         \
    if (!(expression))                                                         \
      return luaL_error((L), "native roots: %s:%d: %s", __FILE__, __LINE__,    \
                        #expression);                                          \
  } while (0)

static void *allocate(void *user, void *pointer, size_t old_size, size_t size) {
  struct context *context = user;
  if (size == 0) {
    free(pointer);
    return NULL;
  }
  ++context->allocations;
  if (context->reject_growth && (pointer == NULL || size > old_size))
    return NULL;
  return realloc(pointer, size);
}

static int finalize(lua_State *state) {
  struct resource *value = lua_touserdata(state, 1);
  if (value->magic != 0x1234abcdU)
    return luaL_error(state, "resource finalized twice or corrupted");
  value->magic = 0;
  ++value->context->finalized;
  return 0;
}

static void push_resource(lua_State *state, struct context *context) {
  struct resource *value = lua_newuserdatauv(state, sizeof(*value), 0);
  value->context = context;
  value->magic = 0x1234abcdU;
  luaL_setmetatable(state, "native.root.resource");
}

static void collect(lua_State *state) {
  (void)lua_gc(state, LUA_GCCOLLECT);
  (void)lua_gc(state, LUA_GCCOLLECT);
}

static struct context *argument_context(lua_State *state) {
  struct resource *value = lua_touserdata(state, 1);
  return value->context;
}

static void root_argument(lua_State *state, LuaNativeRoot *root) {
  luaR_pushroot(state, root);
  luaR_setroot(state, root, s2v(state->top.p - 1));
  lua_settop(state, 0);
}

static int retained(lua_State *state) {
  struct context *context = argument_context(state);
  LuaNativeRoot root;
  struct resource *value;
  root_argument(state, &root);
  REQUIRE(state, lua_checkstack(state, 4096));
  REQUIRE(state,
          luaL_dostring(state, "collectgarbage(); collectgarbage()") == LUA_OK);
  REQUIRE(state, context->finalized == 0);
  REQUIRE(state, ttisfulluserdata(&root.value));
  value = (struct resource *)getudatamem(uvalue(&root.value));
  REQUIRE(state, value->magic == 0x1234abcdU && value->context == context);
  luaR_poproot(state, &root);
  collect(state);
  REQUIRE(state, context->finalized == 1);
  return 0;
}

static int throw_with_root(lua_State *state) {
  LuaNativeRoot root;
  root_argument(state, &root);
  return luaL_error(state, "expected native-root unwind");
}

static int publish_root(lua_State *state) {
  struct context *context = argument_context(state);
  root_argument(state, context->published);
  return 0;
}

static int nested_error(lua_State *state) {
  struct context *context = argument_context(state);
  LuaNativeRoot outer;
  int status;
  root_argument(state, &outer);
  lua_pushcfunction(state, throw_with_root);
  push_resource(state, context);
  status = lua_pcall(state, 1, 0, 0);
  REQUIRE(state, status == LUA_ERRRUN);
  REQUIRE(state,
          strstr(lua_tostring(state, -1), "expected native-root unwind"));
  lua_pop(state, 1);
  collect(state);
  REQUIRE(state, context->finalized == 1);
  REQUIRE(state, ttisfulluserdata(&outer.value));
  luaR_poproot(state, &outer);
  collect(state);
  REQUIRE(state, context->finalized == 2);
  return 0;
}

static int replace_root(lua_State *state) {
  struct context *context = argument_context(state);
  LuaNativeRoot root;
  root_argument(state, &root);
  collect(state);
  REQUIRE(state, context->finalized == 0);
  push_resource(state, context);
  luaR_setroot(state, &root, s2v(state->top.p - 1));
  lua_pop(state, 1);
  collect(state);
  REQUIRE(state, context->finalized == 1);
  luaR_poproot(state, &root);
  collect(state);
  REQUIRE(state, context->finalized == 2);
  return 0;
}

static int graph_root(lua_State *state) {
  struct resource *payload = lua_touserdata(state, 1);
  struct context *context = payload->context;
  LuaNativeRoot root;
  REQUIRE(state,
          luaL_loadstring(state,
                          "local u=...; return {read=function() return u end, "
                          "text=string.rep('z',4096)}") == LUA_OK);
  lua_pushvalue(state, 1);
  REQUIRE(state, lua_pcall(state, 1, 1, 0) == LUA_OK);
  root_argument(state, &root);
  collect(state);
  REQUIRE(state, context->finalized == 0);
  setobj2s(state, state->top.p, &root.value);
  state->top.p++;
  lua_getfield(state, -1, "text");
  REQUIRE(state, lua_rawlen(state, -1) == 4096);
  lua_pop(state, 1);
  lua_getfield(state, -1, "read");
  REQUIRE(state, lua_pcall(state, 0, 1, 0) == LUA_OK);
  REQUIRE(state, lua_touserdata(state, -1) == payload);
  lua_settop(state, 0);
  luaR_poproot(state, &root);
  collect(state);
  REQUIRE(state, context->finalized == 1);
  return 0;
}

static int yield_continuation(lua_State *state, int status,
                              lua_KContext context) {
  struct context *capture = (struct context *)context;
  REQUIRE(state, status == LUA_YIELD && capture->finalized == 1);
  lua_pushinteger(state, 42);
  return 1;
}

static int yield_with_root(lua_State *state) {
  struct context *context = argument_context(state);
  LuaNativeRoot root;
  root_argument(state, &root);
  return lua_yieldk(state, 0, (lua_KContext)context, yield_continuation);
}

static int close_with_root(lua_State *state) {
  LuaNativeRoot root;
  root_argument(state, &root);
  return lua_closethread(state, state);
}

static int allocation_failure(lua_State *state) {
  struct context *context = argument_context(state);
  LuaNativeRoot root;
  unsigned before = context->allocations;
  int status;
  context->reject_growth = 1;
  root_argument(state, &root);
  REQUIRE(state, context->allocations == before);
  status = luaL_loadstring(state, "return {'native-root-allocation-failure'}");
  context->reject_growth = 0;
  REQUIRE(state, status == LUA_ERRMEM && context->allocations > before);
  lua_pop(state, 1);
  collect(state);
  REQUIRE(state, context->finalized == 0);
  luaR_poproot(state, &root);
  collect(state);
  REQUIRE(state, context->finalized == 1);
  return 0;
}

static int forwarded_error(lua_State *state) {
  struct context *context = lua_touserdata(state, lua_upvalueindex(1));
  LuaNativeRoot root;
  context->child = lua_newthread(state);
  context->child_ref = luaL_ref(state, LUA_REGISTRYINDEX);
  push_resource(context->child, context);
  root_argument(context->child, &root);
  lua_pushliteral(context->child, "expected forwarded native-root unwind");
  return lua_error(context->child);
}

static int panic_handler(lua_State *state) {
  void *user;
  struct context *context;
  (void)lua_getallocf(state, &user);
  context = user;
  longjmp(*context->panic_target, 1);
}

static int exercise_panic(void) {
  struct context *context = calloc(1, sizeof(*context));
  lua_State *state;
  jmp_buf target;
  int failed;
  if (context == NULL)
    return 1;
  state = lua_newstate(allocate, context, 0);
  if (state == NULL) {
    free(context);
    return 1;
  }
  luaL_newmetatable(state, "native.root.resource");
  lua_pushcfunction(state, finalize);
  lua_setfield(state, -2, "__gc");
  lua_pop(state, 1);
  (void)lua_atpanic(state, panic_handler);
  context->panic_target = &target;
  if (setjmp(target) == 0) {
    push_resource(state, context);
    (void)throw_with_root(state);
    failed = 1;
  } else {
    lua_settop(state, 0);
    collect(state);
    failed = context->finalized != 1;
  }
  lua_close(state);
  free(context);
  return failed;
}

static int many_roots(lua_State *state, struct context *context) {
  const unsigned count = 128;
  LuaNativeRoot *roots = calloc(count, sizeof(*roots));
  unsigned i;
  int failed;
  if (roots == NULL)
    return 1;
  for (i = 0; i < count; ++i) {
    push_resource(state, context);
    luaR_pushroot(state, &roots[i]);
    luaR_setroot(state, &roots[i], s2v(state->top.p - 1));
    lua_pop(state, 1);
    (void)lua_gc(state, LUA_GCSTEP, (size_t)1024);
  }
  collect(state);
  failed = context->finalized != 0;
  for (i = count; i > 0; --i)
    luaR_poproot(state, &roots[i - 1]);
  free(roots);
  collect(state);
  return failed || context->finalized != (int)count;
}

static int exercise(const char *name) {
  struct context context;
  lua_State *state;
  int failed = 0;
  int status = LUA_OK;
  LuaNativeRoot published;
  if (strcmp(name, "panic") == 0)
    return exercise_panic();
  memset(&context, 0, sizeof(context));
  state = lua_newstate(allocate, &context, 0);
  if (state == NULL)
    return 1;
  luaL_openlibs(state);
  luaL_newmetatable(state, "native.root.resource");
  lua_pushcfunction(state, finalize);
  lua_setfield(state, -2, "__gc");
  lua_pop(state, 1);
  if (strstr(name, "generational") != NULL)
    (void)lua_gc(state, LUA_GCGEN);
  if (strcmp(name, "many_roots") == 0 ||
      strcmp(name, "many_roots_generational") == 0) {
    failed = many_roots(state, &context);
  } else if (strcmp(name, "yield") == 0 || strcmp(name, "self_close") == 0) {
    lua_State *child = lua_newthread(state);
    int results;
    int yielding = strcmp(name, "yield") == 0;
    lua_pushcfunction(child, yielding ? yield_with_root : close_with_root);
    push_resource(child, &context);
    status = lua_resume(child, state, 1, &results);
    failed = status != (yielding ? LUA_YIELD : LUA_OK) || results != 0;
    collect(state);
    failed |= context.finalized != 1;
    if (!failed && yielding) {
      status = lua_resume(child, state, 0, &results);
      failed =
          status != LUA_OK || results != 1 || lua_tointeger(child, -1) != 42;
    }
  } else if (strcmp(name, "forwarded_error") == 0) {
    lua_pushlightuserdata(state, &context);
    lua_pushcclosure(state, forwarded_error, 1);
    status = lua_pcall(state, 0, 0, 0);
    failed = status != LUA_ERRRUN;
    lua_settop(state, 0);
    collect(state);
    failed |= context.finalized != 1;
    luaL_unref(state, LUA_REGISTRYINDEX, context.child_ref);
  } else if (strcmp(name, "publish") == 0) {
    context.published = &published;
    lua_pushcfunction(state, publish_root);
    push_resource(state, &context);
    status = lua_pcall(state, 1, 0, 0);
    collect(state);
    failed = status != LUA_OK || context.finalized != 0;
    if (!failed) {
      luaR_poproot(state, &published);
      collect(state);
      failed = context.finalized != 1;
    }
  } else {
    lua_CFunction operation = NULL;
    if (strcmp(name, "retain_incremental") == 0 ||
        strcmp(name, "retain_generational") == 0)
      operation = retained;
    else if (strcmp(name, "nested_error") == 0)
      operation = nested_error;
    else if (strcmp(name, "allocation_failure") == 0)
      operation = allocation_failure;
    else if (strcmp(name, "replace") == 0)
      operation = replace_root;
    else if (strcmp(name, "graph") == 0)
      operation = graph_root;
    if (operation == NULL) {
      lua_close(state);
      return 2;
    }
    lua_pushcfunction(state, operation);
    push_resource(state, &context);
    status = lua_pcall(state, 1, 0, 0);
    failed = status != LUA_OK;
  }
  context.reject_growth = 0;
  if (failed) {
    const char *message =
        lua_gettop(state) > 0 ? lua_tostring(state, -1) : NULL;
    fprintf(stderr, "%s: status=%d finalized=%d: %s\n", name, status,
            context.finalized, message == NULL ? "no error message" : message);
  }
  lua_close(state);
  return failed;
}

static void *worker(void *argument) {
  struct run *run = argument;
  run->failed = exercise(run->name);
  return NULL;
}

int main(int argc, char **argv) {
  struct run run;
  pthread_t thread;
  if (argc != 2)
    return 2;
  run.name = argv[1];
  run.failed = 1;
  if (pthread_create(&thread, NULL, worker, &run) != 0 ||
      pthread_join(thread, NULL) != 0)
    return 1;
  return run.failed;
}
