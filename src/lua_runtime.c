#include "luthr/lua_runtime.h"
#include "policy.h"
#include <lapi.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lcontrol.h>
#include <lexec.h>
#include <lua.h>
#include <lualib.h>

/* Private scalar policy shared by the hook and its host invocation. */
#define policy_load(field) __atomic_load_n(&(field), __ATOMIC_SEQ_CST)
#define policy_store(field, value)                                             \
  __atomic_store_n(&(field), (value), __ATOMIC_SEQ_CST)

struct luthr_lua_runtime_chunk {
  char *module_name;
  size_t module_name_size;
  char *chunk_name;
  size_t chunk_name_size;
  unsigned char *source;
  size_t source_size;
  size_t source_alloc_size;
  struct luthr_lua_runtime_chunk *next;
};

struct luthr_lua_runtime_allocator {
  luthr_lua_runtime_allocator_config config;
  size_t used;
  pthread_mutex_t *mutex;
};

struct luthr_lua_runtime_c_module {
  luthr_lua_runtime_c_module_open_fn opener;
  struct luthr_lua_runtime_c_module *next;
};

struct luthr_lua_runtime {
  lua_State *state;
  pthread_mutex_t policy_mutex;
  pthread_mutex_t allocator_mutex;
  struct luthr_lua_runtime_allocator allocator;
  char *last_error;
  void *context;
  luthr_lua_runtime_warning_fn warning_callback;
  void *warning_context;
  luthr_lua_runtime_trace_fn trace_callback;
  void *trace_context;
  int active_calls;
  int closing;
  struct luthr_lua_runtime_chunk *chunks;
  struct luthr_lua_runtime_c_module *c_modules;
  int traceback_enabled;
  int instruction_limit;
  LuaPolicy *destruction_policy;
};

struct luthr_lua_runtime_open_lib {
  const char *name;
  lua_CFunction opener;
};

typedef int (*luthr_lua_runtime_protected_fn)(lua_State *state);

struct luthr_lua_runtime_protected_call {
  luthr_lua_runtime_protected_fn fn;
  void *context;
};

struct luthr_lua_runtime_set_arg_context {
  const char *script_name;
  int argc;
  const char *const *argv;
};

struct luthr_lua_runtime_package_field_context {
  const char *field_name;
  const char *value;
  int prepend;
};

struct luthr_lua_runtime_global_string_context {
  const char *name;
  const char *value;
};

struct luthr_lua_runtime_global_boolean_context {
  const char *name;
  int value;
};

struct luthr_lua_runtime_global_number_context {
  const char *name;
  double value;
};

struct luthr_lua_runtime_global_integer_context {
  const char *name;
  long value;
};

struct luthr_lua_runtime_c_module_context {
  const char *module_name;
  struct luthr_lua_runtime_c_module *module;
};

struct luthr_lua_runtime_lua_module_context {
  struct luthr_lua_runtime_chunk *chunk;
};

struct luthr_lua_runtime_require_context {
  const char *module_name;
};

struct luthr_lua_runtime_load_file_context {
  struct luthr_lua_runtime_set_arg_context args;
  const char *path;
};

struct luthr_lua_runtime_load_buffer_context {
  struct luthr_lua_runtime_set_arg_context args;
  const unsigned char *source;
  size_t source_size;
  const char *chunk_name;
};

static int luthr_lua_runtime_c_module_loader(lua_State *state);
static int luthr_lua_runtime_lua_module_loader(lua_State *state);

static void policy_check(int status) {
  if (status != 0) {
    abort();
  }
}

/* This is a callback reentry guard, not a global execution-active flag. */
static int in_trace(luthr_lua_runtime *runtime) {
  return runtime->closing || luthr_policy_tracing(runtime);
}

static void policy_destroy(luthr_lua_runtime *runtime) {
  policy_check(pthread_mutex_destroy(&runtime->allocator_mutex));
  policy_check(pthread_mutex_destroy(&runtime->policy_mutex));
}

static void luthr_lua_runtime_native_warning(void *context, const char *message,
                                             int to_continue) {
  luthr_lua_runtime *runtime;
  luthr_lua_runtime_warning_fn callback;
  void *callback_context;

  runtime = (luthr_lua_runtime *)context;
  policy_check(pthread_mutex_lock(&runtime->policy_mutex));
  callback = runtime->warning_callback;
  callback_context = runtime->warning_context;
  policy_check(pthread_mutex_unlock(&runtime->policy_mutex));
  if (callback != NULL) {
    callback(callback_context, message, to_continue);
  }
}

static void *luthr_lua_runtime_default_alloc(void *user, size_t size) {
  (void)user;
  return malloc(size);
}

static void *luthr_lua_runtime_default_realloc(void *user, void *ptr,
                                               size_t old_size,
                                               size_t new_size) {
  (void)user;
  (void)old_size;
  return realloc(ptr, new_size);
}

static void luthr_lua_runtime_default_free(void *user, void *ptr, size_t size) {
  (void)user;
  (void)size;
  free(ptr);
}

static void luthr_lua_runtime_allocator_init(
    struct luthr_lua_runtime_allocator *allocator,
    const luthr_lua_runtime_allocator_config *config, size_t max_bytes) {
  int use_default;

  memset(allocator, 0, sizeof(*allocator));
  if (config != NULL) {
    allocator->config = *config;
  }
  use_default =
      allocator->config.alloc_fn == NULL && allocator->config.free_fn == NULL;
  if (use_default) {
    allocator->config.alloc_fn = luthr_lua_runtime_default_alloc;
    allocator->config.realloc_fn = luthr_lua_runtime_default_realloc;
    allocator->config.free_fn = luthr_lua_runtime_default_free;
  }
  if (max_bytes != 0) {
    allocator->config.max_bytes = max_bytes;
  }
}

static int luthr_lua_runtime_allocator_can_grow(
    struct luthr_lua_runtime_allocator *allocator, size_t old_size,
    size_t new_size) {
  size_t adjusted_used;

  if (allocator == NULL || allocator->config.max_bytes == 0 ||
      new_size <= old_size) {
    return 1;
  }
  adjusted_used = allocator->used;
  if (adjusted_used >= old_size) {
    adjusted_used -= old_size;
  } else {
    adjusted_used = 0;
  }
  if (adjusted_used >= allocator->config.max_bytes) {
    return 0;
  }
  if (new_size > allocator->config.max_bytes - adjusted_used) {
    return 0;
  }
  return 1;
}

static void luthr_lua_runtime_allocator_account(
    struct luthr_lua_runtime_allocator *allocator, size_t old_size,
    size_t new_size) {
  if (allocator == NULL) {
    return;
  }
  if (allocator->used >= old_size) {
    allocator->used -= old_size;
  } else {
    allocator->used = 0;
  }
  allocator->used += new_size;
}

static void *luthr_lua_runtime_allocator_alloc_unlocked(
    struct luthr_lua_runtime_allocator *allocator, size_t size) {
  void *ptr;

  if (size == 0) {
    size = 1;
  }
  if (!luthr_lua_runtime_allocator_can_grow(allocator, 0, size)) {
    return NULL;
  }
  ptr = allocator->config.alloc_fn(allocator->config.user, size);
  if (ptr == NULL) {
    return NULL;
  }
  luthr_lua_runtime_allocator_account(allocator, 0, size);
  return ptr;
}

static void *luthr_lua_runtime_allocator_realloc_unlocked(
    struct luthr_lua_runtime_allocator *allocator, void *ptr, size_t old_size,
    size_t new_size) {
  void *next;
  size_t copy_size;

  if (new_size == 0) {
    if (ptr == NULL) {
      return NULL;
    }
    allocator->config.free_fn(allocator->config.user, ptr, old_size);
    luthr_lua_runtime_allocator_account(allocator, old_size, 0);
    return NULL;
  }
  if (ptr == NULL) {
    return luthr_lua_runtime_allocator_alloc_unlocked(allocator, new_size);
  }
  if (!luthr_lua_runtime_allocator_can_grow(allocator, old_size, new_size)) {
    return NULL;
  }
  if (allocator->config.realloc_fn != NULL) {
    next = allocator->config.realloc_fn(allocator->config.user, ptr, old_size,
                                        new_size);
    if (next == NULL) {
      return NULL;
    }
    luthr_lua_runtime_allocator_account(allocator, old_size, new_size);
    return next;
  }

  next = allocator->config.alloc_fn(allocator->config.user, new_size);
  if (next == NULL) {
    return NULL;
  }
  copy_size = old_size < new_size ? old_size : new_size;
  if (copy_size != 0) {
    memcpy(next, ptr, copy_size);
  }
  allocator->config.free_fn(allocator->config.user, ptr, old_size);
  luthr_lua_runtime_allocator_account(allocator, old_size, new_size);
  return next;
}

static void luthr_lua_runtime_allocator_free_unlocked(
    struct luthr_lua_runtime_allocator *allocator, void *ptr, size_t size) {
  if (allocator == NULL || ptr == NULL) {
    return;
  }
  allocator->config.free_fn(allocator->config.user, ptr, size);
  luthr_lua_runtime_allocator_account(allocator, size, 0);
}

static void allocator_lock(struct luthr_lua_runtime_allocator *allocator) {
  if (allocator->mutex != NULL)
    policy_check(pthread_mutex_lock(allocator->mutex));
}
static void allocator_unlock(struct luthr_lua_runtime_allocator *allocator) {
  if (allocator->mutex != NULL)
    policy_check(pthread_mutex_unlock(allocator->mutex));
}
static void *
luthr_lua_runtime_allocator_alloc(struct luthr_lua_runtime_allocator *allocator,
                                  size_t size) {
  void *result;
  allocator_lock(allocator);
  result = luthr_lua_runtime_allocator_alloc_unlocked(allocator, size);
  allocator_unlock(allocator);
  return result;
}
static void *luthr_lua_runtime_allocator_realloc(
    struct luthr_lua_runtime_allocator *allocator, void *ptr, size_t old_size,
    size_t size) {
  void *result;
  allocator_lock(allocator);
  result = luthr_lua_runtime_allocator_realloc_unlocked(allocator, ptr,
                                                        old_size, size);
  allocator_unlock(allocator);
  return result;
}
static void
luthr_lua_runtime_allocator_free(struct luthr_lua_runtime_allocator *allocator,
                                 void *ptr, size_t size) {
  allocator_lock(allocator);
  luthr_lua_runtime_allocator_free_unlocked(allocator, ptr, size);
  allocator_unlock(allocator);
}

static void *luthr_lua_runtime_lua_alloc(void *ud, void *ptr, size_t osize,
                                         size_t nsize) {
  struct luthr_lua_runtime_allocator *allocator;

  allocator = (struct luthr_lua_runtime_allocator *)ud;
  return luthr_lua_runtime_allocator_realloc(allocator, ptr, osize, nsize);
}

static char *luthr_lua_runtime_strdup(luthr_lua_runtime *runtime,
                                      const char *value, size_t *out_size) {
  size_t size;
  char *copy;

  if (value == NULL) {
    return NULL;
  }

  size = strlen(value) + 1;
  copy = (char *)luthr_lua_runtime_allocator_alloc(&runtime->allocator, size);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, size);
  if (out_size != NULL) {
    *out_size = size;
  }
  return copy;
}

static void
luthr_lua_runtime_free_chunk(luthr_lua_runtime *runtime,
                             struct luthr_lua_runtime_chunk *chunk) {
  if (runtime == NULL || chunk == NULL) {
    return;
  }
  luthr_lua_runtime_allocator_free(&runtime->allocator, chunk->module_name,
                                   chunk->module_name_size);
  luthr_lua_runtime_allocator_free(&runtime->allocator, chunk->chunk_name,
                                   chunk->chunk_name_size);
  luthr_lua_runtime_allocator_free(&runtime->allocator, chunk->source,
                                   chunk->source_alloc_size);
  luthr_lua_runtime_allocator_free(&runtime->allocator, chunk, sizeof(*chunk));
}

static void luthr_lua_runtime_clear_chunks(luthr_lua_runtime *runtime) {
  struct luthr_lua_runtime_chunk *chunk;
  struct luthr_lua_runtime_chunk *next;

  chunk = runtime->chunks;
  while (chunk != NULL) {
    next = chunk->next;
    luthr_lua_runtime_free_chunk(runtime, chunk);
    chunk = next;
  }
  runtime->chunks = NULL;
}

static luthr_lua_runtime_status
luthr_lua_runtime_set_error(luthr_lua_runtime *runtime,
                            luthr_lua_runtime_status status,
                            const char *message) {
  char *copy;

  if (runtime == NULL) {
    return status;
  }

  luthr_lua_runtime_allocator_free(
      &runtime->allocator, runtime->last_error,
      runtime->last_error != NULL ? strlen(runtime->last_error) + 1 : 0);
  runtime->last_error = NULL;

  if (message != NULL) {
    copy = luthr_lua_runtime_strdup(runtime, message, NULL);
    if (copy == NULL) {
      runtime->last_error =
          luthr_lua_runtime_strdup(runtime, "out of memory", NULL);
      return LUTHR_LUA_RUNTIME_ERR_ALLOC;
    }
    runtime->last_error = copy;
  }

  return status;
}

/* VM extra space belongs to the embedder, unlike Lua's writable registry. */
static luthr_lua_runtime *luthr_lua_runtime_from_state(lua_State *state) {
  return *(luthr_lua_runtime **)lua_getextraspace(state);
}

static void luthr_lua_runtime_store_state(lua_State *state,
                                          luthr_lua_runtime *runtime) {
  *(luthr_lua_runtime **)lua_getextraspace(state) = runtime;
}

static int luthr_lua_runtime_store_state_protected(lua_State *state) {
  struct luthr_lua_runtime_protected_call *call;
  luthr_lua_runtime *runtime;

  call = (struct luthr_lua_runtime_protected_call *)lua_touserdata(state, 1);
  runtime = call != NULL ? (luthr_lua_runtime *)call->context : NULL;
  luthr_lua_runtime_store_state(state, runtime);
  return 0;
}

static void luthr_lua_runtime_set_arg(
    lua_State *state, const struct luthr_lua_runtime_set_arg_context *context) {
  int i;

  lua_createtable(state, context->argc, 1);
  lua_pushstring(state,
                 context->script_name != NULL ? context->script_name : "");
  lua_rawseti(state, -2, 0);

  for (i = 0; i < context->argc; ++i) {
    lua_pushstring(state, context->argv[i] != NULL ? context->argv[i] : "");
    lua_rawseti(state, -2, i + 1);
  }

  lua_setglobal(state, "arg");
}

static int luthr_lua_runtime_call_protected_fn(lua_State *state) {
  struct luthr_lua_runtime_protected_call *call;

  call = (struct luthr_lua_runtime_protected_call *)lua_touserdata(state, 1);
  if (call == NULL || call->fn == NULL) {
    return luaL_error(state, "missing protected Lua runtime operation");
  }
  return call->fn(state);
}

static luthr_lua_runtime_status
luthr_lua_runtime_maybe_openlibs(luthr_lua_runtime *runtime, int flags) {
  if ((flags & LUTHR_LUA_RUNTIME_OPEN_LIBS) != 0) {
    return luthr_lua_runtime_openlibs(runtime);
  }
  return LUTHR_LUA_RUNTIME_OK;
}

static int luthr_lua_runtime_disabled_debug_sethook(lua_State *state) {
  (void)state;
  return luaL_error(
      state, "debug.sethook is unavailable in the luthr Lua runtime facade");
}

static void
luthr_lua_runtime_disable_debug_sethook(luthr_lua_runtime *runtime) {
  lua_State *state;

  state = runtime->state;
  lua_getglobal(state, "debug");
  if (!lua_istable(state, -1)) {
    lua_pop(state, 1);
    return;
  }
  lua_pushcfunction(state, luthr_lua_runtime_disabled_debug_sethook);
  lua_setfield(state, -2, "sethook");
  lua_pop(state, 1);
}

static int luthr_lua_runtime_disable_debug_sethook_protected(lua_State *state) {
  struct luthr_lua_runtime_protected_call *call;
  luthr_lua_runtime *runtime;

  call = (struct luthr_lua_runtime_protected_call *)lua_touserdata(state, 1);
  runtime = call != NULL ? (luthr_lua_runtime *)call->context : NULL;
  if (runtime == NULL) {
    return luaL_error(state, "missing Lua runtime");
  }
  luthr_lua_runtime_disable_debug_sethook(runtime);
  return 0;
}

static int luthr_lua_runtime_traceback(lua_State *state) {
  const char *message;

  message = lua_tostring(state, 1);
  if (message == NULL) {
    message = "Lua runtime error";
  }
  luaL_traceback(state, state, message, 1);
  return 1;
}

/* Numeric error conversion can allocate; it must remain inside lua_pcall. */
static int luthr_lua_runtime_error_to_string(lua_State *state) {
  const char *message;
  message = lua_tostring(state, 1);
  if (message == NULL) {
    lua_pushstring(state, "non-string Lua error");
  }
  return 1;
}

static void runtime_options(luthr_lua_runtime *runtime, const void *settings,
                            luthr_lua_task_options *options) {
  if (settings != NULL)
    *options = *(const luthr_lua_task_options *)settings;
  else {
    memset(options, 0, sizeof(*options));
    policy_check(pthread_mutex_lock(&runtime->policy_mutex));
    options->instruction_limit = (unsigned long)runtime->instruction_limit;
    options->context = runtime->context;
    options->trace = runtime->trace_callback;
    options->trace_context = runtime->trace_context;
    policy_check(pthread_mutex_unlock(&runtime->policy_mutex));
  }
}
static LuaPolicy *runtime_policy(lua_State *state, const void *settings,
                                 void *owner) {
  luthr_lua_runtime *runtime;
  luthr_lua_task_options options;
  runtime = (luthr_lua_runtime *)owner;
  runtime_options(runtime, settings, &options);
  return luthr_policy_new(state, &options, runtime);
}
static void end_policy(luthr_lua_runtime *runtime, LuaPolicy *policy,
                       LuaPolicyScope *scope) {
  policy_store(policy->active, 0);
  luaCP_end(scope);
  lua_lock(runtime->state);
  luaCP_release(runtime->state, policy);
  lua_unlock(runtime->state);
  __atomic_fetch_sub(&runtime->active_calls, 1, __ATOMIC_SEQ_CST);
}

static luthr_lua_runtime_status luthr_lua_runtime_call_with_status(
    luthr_lua_runtime *runtime, int nargs, int nresults,
    luthr_lua_runtime_status failure_status, int traceback_enabled) {
  lua_State *state;
  int function_index;
  int error_index;
  int result;
  LuaPolicy *policy;
  LuaPolicyScope scope;
  luthr_lua_runtime_status status;

  policy = runtime_policy(runtime->state, NULL, runtime);
  if (policy == NULL) {
    lua_settop(runtime->state, lua_gettop(runtime->state) - nargs - 1);
    return luthr_lua_runtime_set_error(runtime, LUTHR_LUA_RUNTIME_ERR_ALLOC,
                                       "out of memory");
  }
  luaCP_begin(policy, &scope);
  __atomic_fetch_add(&runtime->active_calls, 1, __ATOMIC_SEQ_CST);
  state = runtime->state;
  function_index = lua_gettop(state) - nargs;
  error_index = 0;

  if (traceback_enabled) {
    lua_pushcfunction(state, luthr_lua_runtime_traceback);
    lua_insert(state, function_index);
    error_index = function_index;
  }

  result = lua_pcall(state, nargs, nresults, error_index);
  if (result != LUA_OK) {
    if (policy_load(policy->exhausted)) {
      status = LUTHR_LUA_RUNTIME_ERR_LIMIT;
    } else if (result == LUA_ERRMEM) {
      status = LUTHR_LUA_RUNTIME_ERR_ALLOC;
    } else {
      status = failure_status;
    }
    if (lua_type(state, -1) != LUA_TSTRING) {
      lua_pushcfunction(state, luthr_lua_runtime_error_to_string);
      lua_pushvalue(state, -2);
      result = lua_pcall(state, 1, 1, 0);
      if (result == LUA_ERRMEM) {
        status = LUTHR_LUA_RUNTIME_ERR_ALLOC;
      }
      status =
          luthr_lua_runtime_set_error(runtime, status, lua_tostring(state, -1));
      lua_pop(state, 1);
    } else {
      status =
          luthr_lua_runtime_set_error(runtime, status, lua_tostring(state, -1));
    }
    if (error_index != 0) {
      lua_settop(state, error_index - 1);
    } else {
      lua_pop(state, 1);
    }
    end_policy(runtime, policy, &scope);
    return status;
  }

  if (error_index != 0) {
    if (nresults == LUA_MULTRET) {
      lua_settop(state, error_index - 1);
    } else {
      lua_remove(state, error_index);
    }
  }

  if (policy_load(policy->exhausted)) {
    status = luthr_lua_runtime_set_error(runtime, LUTHR_LUA_RUNTIME_ERR_LIMIT,
                                         "Lua instruction limit exceeded");
    end_policy(runtime, policy, &scope);
    return status;
  }

  luthr_lua_runtime_clear_error(runtime);
  end_policy(runtime, policy, &scope);
  return LUTHR_LUA_RUNTIME_OK;
}

static luthr_lua_runtime_status
luthr_lua_runtime_call(luthr_lua_runtime *runtime, int nargs, int nresults) {
  return luthr_lua_runtime_call_with_status(runtime, nargs, nresults,
                                            LUTHR_LUA_RUNTIME_ERR_RUNTIME,
                                            runtime->traceback_enabled);
}

static luthr_lua_runtime_status luthr_lua_runtime_protected_call(
    luthr_lua_runtime *runtime, luthr_lua_runtime_protected_fn fn,
    void *context, int nresults, luthr_lua_runtime_status failure_status) {
  struct luthr_lua_runtime_protected_call call;

  call.fn = fn;
  call.context = context;
  lua_pushcfunction(runtime->state, luthr_lua_runtime_call_protected_fn);
  lua_pushlightuserdata(runtime->state, &call);
  return luthr_lua_runtime_call_with_status(runtime, 1, nresults,
                                            failure_status, 0);
}

static luthr_lua_runtime_status
luthr_lua_runtime_disable_debug_sethook_checked(luthr_lua_runtime *runtime) {
  return luthr_lua_runtime_protected_call(
      runtime, luthr_lua_runtime_disable_debug_sethook_protected, runtime, 0,
      LUTHR_LUA_RUNTIME_ERR_RUNTIME);
}

static int luthr_lua_runtime_open_one_lib_protected(lua_State *state) {
  struct luthr_lua_runtime_open_lib *lib;

  struct luthr_lua_runtime_protected_call *call;

  call = (struct luthr_lua_runtime_protected_call *)lua_touserdata(state, 1);
  lib =
      call != NULL ? (struct luthr_lua_runtime_open_lib *)call->context : NULL;
  if (lib == NULL || lib->name == NULL || lib->opener == NULL) {
    return luaL_error(state, "missing Lua library opener");
  }
  luaL_requiref(state, lib->name, lib->opener, 1);
  return 1;
}

static luthr_lua_runtime_status
luthr_lua_runtime_open_one_lib(luthr_lua_runtime *runtime, const char *name,
                               lua_CFunction opener) {
  struct luthr_lua_runtime_open_lib lib;
  luthr_lua_runtime_status status;

  lib.name = name;
  lib.opener = opener;
  status = luthr_lua_runtime_protected_call(
      runtime, luthr_lua_runtime_open_one_lib_protected, &lib, 1,
      LUTHR_LUA_RUNTIME_ERR_RUNTIME);
  if (status != LUTHR_LUA_RUNTIME_OK) {
    return status;
  }
  lua_pop(runtime->state, 1);
  return LUTHR_LUA_RUNTIME_OK;
}

static int luthr_lua_runtime_set_package_field_protected(lua_State *state) {
  struct luthr_lua_runtime_protected_call *call;
  struct luthr_lua_runtime_package_field_context *context;
  const char *current;

  call = (struct luthr_lua_runtime_protected_call *)lua_touserdata(state, 1);
  context =
      call != NULL
          ? (struct luthr_lua_runtime_package_field_context *)call->context
          : NULL;
  if (context == NULL || context->field_name == NULL ||
      context->value == NULL) {
    return luaL_error(state, "missing package field");
  }
  lua_getglobal(state, "package");
  if (!lua_istable(state, -1)) {
    return luaL_error(
        state,
        "Lua package table is unavailable; open the package library first");
  }
  if (context->prepend) {
    lua_getfield(state, -1, context->field_name);
    current = lua_tostring(state, -1);
    if (current != NULL && current[0] != '\0') {
      lua_pushfstring(state, "%s;%s", context->value, current);
    } else {
      lua_pushstring(state, context->value);
    }
    lua_remove(state, -2);
  } else {
    lua_pushstring(state, context->value);
  }
  lua_setfield(state, -2, context->field_name);
  lua_pop(state, 1);
  return 0;
}

static luthr_lua_runtime_status
luthr_lua_runtime_update_package_field(luthr_lua_runtime *runtime,
                                       const char *field_name,
                                       const char *value, int prepend) {
  struct luthr_lua_runtime_package_field_context context;

  context.field_name = field_name;
  context.value = value;
  context.prepend = prepend;
  return luthr_lua_runtime_protected_call(
      runtime, luthr_lua_runtime_set_package_field_protected, &context, 0,
      LUTHR_LUA_RUNTIME_ERR_RUNTIME);
}

static int luthr_lua_runtime_set_global_string_protected(lua_State *state) {
  struct luthr_lua_runtime_protected_call *call;
  struct luthr_lua_runtime_global_string_context *context;

  call = (struct luthr_lua_runtime_protected_call *)lua_touserdata(state, 1);
  context =
      call != NULL
          ? (struct luthr_lua_runtime_global_string_context *)call->context
          : NULL;
  if (context == NULL || context->name == NULL || context->value == NULL) {
    return luaL_error(state, "missing string global");
  }
  lua_pushstring(state, context->value);
  lua_setglobal(state, context->name);
  return 0;
}

static int luthr_lua_runtime_set_global_boolean_protected(lua_State *state) {
  struct luthr_lua_runtime_protected_call *call;
  struct luthr_lua_runtime_global_boolean_context *context;

  call = (struct luthr_lua_runtime_protected_call *)lua_touserdata(state, 1);
  context =
      call != NULL
          ? (struct luthr_lua_runtime_global_boolean_context *)call->context
          : NULL;
  if (context == NULL || context->name == NULL) {
    return luaL_error(state, "missing boolean global");
  }
  lua_pushboolean(state, context->value != 0);
  lua_setglobal(state, context->name);
  return 0;
}

static int luthr_lua_runtime_set_global_number_protected(lua_State *state) {
  struct luthr_lua_runtime_protected_call *call;
  struct luthr_lua_runtime_global_number_context *context;

  call = (struct luthr_lua_runtime_protected_call *)lua_touserdata(state, 1);
  context =
      call != NULL
          ? (struct luthr_lua_runtime_global_number_context *)call->context
          : NULL;
  if (context == NULL || context->name == NULL) {
    return luaL_error(state, "missing number global");
  }
  lua_pushnumber(state, (lua_Number)context->value);
  lua_setglobal(state, context->name);
  return 0;
}

static int luthr_lua_runtime_set_global_integer_protected(lua_State *state) {
  struct luthr_lua_runtime_protected_call *call;
  struct luthr_lua_runtime_global_integer_context *context;

  call = (struct luthr_lua_runtime_protected_call *)lua_touserdata(state, 1);
  context =
      call != NULL
          ? (struct luthr_lua_runtime_global_integer_context *)call->context
          : NULL;
  if (context == NULL || context->name == NULL) {
    return luaL_error(state, "missing integer global");
  }
  lua_pushinteger(state, (lua_Integer)context->value);
  lua_setglobal(state, context->name);
  return 0;
}

static int luthr_lua_runtime_register_c_module_protected(lua_State *state) {
  struct luthr_lua_runtime_protected_call *call;
  struct luthr_lua_runtime_c_module_context *context;

  call = (struct luthr_lua_runtime_protected_call *)lua_touserdata(state, 1);
  context = call != NULL
                ? (struct luthr_lua_runtime_c_module_context *)call->context
                : NULL;
  if (context == NULL || context->module_name == NULL ||
      context->module == NULL || context->module->opener == NULL) {
    return luaL_error(state, "missing C module registration");
  }
  lua_getglobal(state, "package");
  if (!lua_istable(state, -1)) {
    return luaL_error(state, "Lua package table is unavailable; open standard "
                             "libraries before registering preload modules");
  }
  lua_getfield(state, -1, "preload");
  lua_remove(state, -2);
  if (!lua_istable(state, -1)) {
    return luaL_error(state, "Lua package.preload table is unavailable");
  }
  lua_pushlightuserdata(state, context->module);
  lua_pushcclosure(state, luthr_lua_runtime_c_module_loader, 1);
  lua_pushstring(state, context->module_name);
  lua_insert(state, -2);
  lua_rawset(state, -3);
  lua_pop(state, 1);
  return 0;
}

static int luthr_lua_runtime_register_lua_module_protected(lua_State *state) {
  struct luthr_lua_runtime_protected_call *call;
  struct luthr_lua_runtime_lua_module_context *context;
  struct luthr_lua_runtime_chunk *chunk;

  call = (struct luthr_lua_runtime_protected_call *)lua_touserdata(state, 1);
  context = call != NULL
                ? (struct luthr_lua_runtime_lua_module_context *)call->context
                : NULL;
  chunk = context != NULL ? context->chunk : NULL;
  if (chunk == NULL || chunk->module_name == NULL) {
    return luaL_error(state, "missing Lua module registration");
  }
  lua_getglobal(state, "package");
  if (!lua_istable(state, -1)) {
    return luaL_error(state, "Lua package table is unavailable; open standard "
                             "libraries before registering preload modules");
  }
  lua_getfield(state, -1, "preload");
  lua_remove(state, -2);
  if (!lua_istable(state, -1)) {
    return luaL_error(state, "Lua package.preload table is unavailable");
  }
  lua_pushlightuserdata(state, chunk);
  lua_pushcclosure(state, luthr_lua_runtime_lua_module_loader, 1);
  lua_pushstring(state, chunk->module_name);
  lua_insert(state, -2);
  /* Publish without invoking a metamethod that could retain the loader and
   * then raise an error before runtime ownership is committed. */
  lua_rawset(state, -3);
  lua_pop(state, 1);
  return 0;
}

static int luthr_lua_runtime_require_protected(lua_State *state) {
  struct luthr_lua_runtime_protected_call *call;
  struct luthr_lua_runtime_require_context *context;

  call = (struct luthr_lua_runtime_protected_call *)lua_touserdata(state, 1);
  context = call != NULL
                ? (struct luthr_lua_runtime_require_context *)call->context
                : NULL;
  if (context == NULL || context->module_name == NULL) {
    return luaL_error(state, "missing require module name");
  }
  lua_getglobal(state, "require");
  if (!lua_isfunction(state, -1)) {
    return luaL_error(
        state,
        "Lua require function is unavailable; open the package library first");
  }
  lua_pushstring(state, context->module_name);
  lua_call(state, 1, 1);
  return 1;
}

static int luthr_lua_runtime_load_file_protected(lua_State *state) {
  struct luthr_lua_runtime_protected_call *call;
  struct luthr_lua_runtime_load_file_context *context;
  int result;

  call = (struct luthr_lua_runtime_protected_call *)lua_touserdata(state, 1);
  context = call != NULL
                ? (struct luthr_lua_runtime_load_file_context *)call->context
                : NULL;
  if (context == NULL || context->path == NULL) {
    return luaL_error(state, "missing Lua file path");
  }
  luthr_lua_runtime_set_arg(state, &context->args);
  result = luaL_loadfile(state, context->path);
  if (result != LUA_OK) {
    return lua_error(state);
  }
  return 1;
}

static int luthr_lua_runtime_load_buffer_protected(lua_State *state) {
  struct luthr_lua_runtime_protected_call *call;
  struct luthr_lua_runtime_load_buffer_context *context;
  int result;

  call = (struct luthr_lua_runtime_protected_call *)lua_touserdata(state, 1);
  context = call != NULL
                ? (struct luthr_lua_runtime_load_buffer_context *)call->context
                : NULL;
  if (context == NULL || context->source == NULL) {
    return luaL_error(state, "missing Lua source buffer");
  }
  luthr_lua_runtime_set_arg(state, &context->args);
  result = luaL_loadbuffer(
      state, (const char *)context->source, context->source_size,
      context->chunk_name != NULL ? context->chunk_name : "buffer");
  if (result != LUA_OK) {
    return lua_error(state);
  }
  return 1;
}

static luthr_lua_runtime_status
luthr_lua_runtime_set_package_field(luthr_lua_runtime *runtime,
                                    const char *field_name, const char *value) {
  return luthr_lua_runtime_update_package_field(runtime, field_name, value, 0);
}

static luthr_lua_runtime_status luthr_lua_runtime_prepend_package_field(
    luthr_lua_runtime *runtime, const char *field_name, const char *value) {
  return luthr_lua_runtime_update_package_field(runtime, field_name, value, 1);
}

static int luthr_lua_runtime_c_module_loader(lua_State *state) {
  luthr_lua_runtime *runtime;
  struct luthr_lua_runtime_c_module *module;
  void *candidate;

  runtime = luthr_lua_runtime_from_state(state);
  if (runtime == NULL || !lua_islightuserdata(state, lua_upvalueindex(1))) {
    return luaL_error(state, "invalid C module opener");
  }
  candidate = lua_touserdata(state, lua_upvalueindex(1));
  for (module = runtime->c_modules; module != NULL; module = module->next) {
    if ((void *)module == candidate) {
      return module->opener((void *)state);
    }
  }
  return luaL_error(state, "invalid C module opener");
}

static int luthr_lua_runtime_lua_module_loader(lua_State *state) {
  struct luthr_lua_runtime_chunk *chunk;
  luthr_lua_runtime *runtime;
  void *candidate;
  int result;
  int nargs;

  runtime = luthr_lua_runtime_from_state(state);
  if (runtime == NULL || !lua_islightuserdata(state, lua_upvalueindex(1))) {
    return luaL_error(state, "invalid Lua preload chunk");
  }
  candidate = lua_touserdata(state, lua_upvalueindex(1));
  for (chunk = runtime->chunks; chunk != NULL; chunk = chunk->next) {
    if ((void *)chunk == candidate) {
      break;
    }
  }
  if (chunk == NULL) {
    return luaL_error(state, "invalid Lua preload chunk");
  }

  nargs = lua_gettop(state);
  result = luaL_loadbuffer(
      state, (const char *)chunk->source, chunk->source_size,
      chunk->chunk_name != NULL ? chunk->chunk_name : chunk->module_name);
  if (result != LUA_OK) {
    return lua_error(state);
  }

  lua_insert(state, 1);
  lua_call(state, nargs, 1);
  return 1;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
const char *luthr_lua_runtime_lua_version(void) { return LUA_VERSION; }

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
const char *luthr_lua_runtime_facade_version(void) { return "1"; }

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status luthr_lua_runtime_new(luthr_lua_runtime **out) {
  return luthr_lua_runtime_new_with_limit(out, 0);
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status
luthr_lua_runtime_new_with_limit(luthr_lua_runtime **out, size_t max_bytes) {
  luthr_lua_runtime_allocator_config config;

  memset(&config, 0, sizeof(config));
  config.max_bytes = max_bytes;
  return luthr_lua_runtime_new_with_allocator(out, &config);
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status luthr_lua_runtime_new_with_allocator(
    luthr_lua_runtime **out,
    const luthr_lua_runtime_allocator_config *allocator_config) {
  struct luthr_lua_runtime_allocator allocator;
  luthr_lua_runtime *runtime;
  lua_State *state;
  luthr_lua_runtime_status status;

  if (out == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  *out = NULL;
  if (allocator_config != NULL && ((allocator_config->alloc_fn == NULL &&
                                    allocator_config->free_fn != NULL) ||
                                   (allocator_config->alloc_fn != NULL &&
                                    allocator_config->free_fn == NULL) ||
                                   (allocator_config->alloc_fn == NULL &&
                                    allocator_config->realloc_fn != NULL))) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }

  luthr_lua_runtime_allocator_init(&allocator, allocator_config, 0);
  runtime = (luthr_lua_runtime *)luthr_lua_runtime_allocator_alloc(
      &allocator, sizeof(*runtime));
  if (runtime == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ALLOC;
  }
  memset(runtime, 0, sizeof(*runtime));
  runtime->allocator = allocator;
  if (pthread_mutex_init(&runtime->policy_mutex, NULL) != 0) {
    luthr_lua_runtime_allocator_free(&allocator, runtime, sizeof(*runtime));
    return LUTHR_LUA_RUNTIME_ERR_ALLOC;
  }
  if (pthread_mutex_init(&runtime->allocator_mutex, NULL) != 0) {
    policy_check(pthread_mutex_destroy(&runtime->policy_mutex));
    luthr_lua_runtime_allocator_free(&allocator, runtime, sizeof(*runtime));
    return LUTHR_LUA_RUNTIME_ERR_ALLOC;
  }

  runtime->allocator.mutex = &runtime->allocator_mutex;
  if (allocator.config.alloc_fn == luthr_lua_runtime_default_alloc &&
      allocator.config.max_bytes == 0) {
    state = lua_newstate(luaL_alloc, NULL, 0);
  } else {
    state = lua_newstate(luthr_lua_runtime_lua_alloc, &runtime->allocator, 0);
  }
  if (state == NULL) {
    allocator = runtime->allocator;
    allocator.mutex = NULL;
    policy_destroy(runtime);
    luthr_lua_runtime_allocator_free(&allocator, runtime, sizeof(*runtime));
    return LUTHR_LUA_RUNTIME_ERR_ALLOC;
  }

  runtime->state = state;
  luthr_policy_set_factory(state, runtime_policy, runtime);
  lua_setwarnf(state, luthr_lua_runtime_native_warning, runtime);
  status = luthr_lua_runtime_protected_call(
      runtime, luthr_lua_runtime_store_state_protected, runtime, 0,
      LUTHR_LUA_RUNTIME_ERR_RUNTIME);
  if (status == LUTHR_LUA_RUNTIME_OK) {
    runtime->destruction_policy = runtime_policy(state, NULL, runtime);
    if (runtime->destruction_policy == NULL)
      status = LUTHR_LUA_RUNTIME_ERR_ALLOC;
    else
      policy_store(runtime->destruction_policy->active, 0);
  }
  if (status != LUTHR_LUA_RUNTIME_OK) {
    lua_close(runtime->state);
    runtime->state = NULL;
    luthr_lua_runtime_allocator_free(
        &runtime->allocator, runtime->last_error,
        runtime->last_error != NULL ? strlen(runtime->last_error) + 1 : 0);
    allocator = runtime->allocator;
    allocator.mutex = NULL;
    policy_destroy(runtime);
    luthr_lua_runtime_allocator_free(&allocator, runtime, sizeof(*runtime));
    return status;
  }
  *out = runtime;

  return LUTHR_LUA_RUNTIME_OK;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
void luthr_lua_runtime_free(luthr_lua_runtime *runtime) {
  struct luthr_lua_runtime_allocator allocator;
  struct luthr_lua_runtime_c_module *module;
  struct luthr_lua_runtime_c_module *next;

  if (runtime == NULL || in_trace(runtime) ||
      policy_load(runtime->active_calls) != 0 ||
      (runtime->state != NULL && __atomic_load_n(&G(runtime->state)->scheduler,
                                                 __ATOMIC_ACQUIRE) != NULL)) {
    return;
  }
  allocator = runtime->allocator;
  allocator.mutex = NULL;

  if (runtime->state != NULL) {
    LuaPolicy *policy;
    LuaPolicyScope scope;
    luthr_lua_task_options options;
    runtime_options(runtime, NULL, &options);
    policy = runtime->destruction_policy;
    luthr_policy_reset(policy, &options);
    runtime->closing = 1;
    if (policy != NULL) {
      luaCP_begin(policy, &scope);
      lua_lock(runtime->state);
      luaCP_apply(runtime->state);
      luaCP_release(runtime->state, policy);
      lua_unlock(runtime->state);
    }
    lua_close(runtime->state);
    if (policy != NULL)
      luaCP_end(&scope);
  }
  luthr_lua_runtime_clear_chunks(runtime);
  module = runtime->c_modules;
  while (module != NULL) {
    next = module->next;
    luthr_lua_runtime_allocator_free(&runtime->allocator, module,
                                     sizeof(*module));
    module = next;
  }
  luthr_lua_runtime_allocator_free(
      &runtime->allocator, runtime->last_error,
      runtime->last_error != NULL ? strlen(runtime->last_error) + 1 : 0);
  policy_destroy(runtime);
  luthr_lua_runtime_allocator_free(&allocator, runtime, sizeof(*runtime));
}

void *luthr_lua_runtime_state(luthr_lua_runtime *runtime) {
  return runtime != NULL && !in_trace(runtime) ? runtime->state : NULL;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
void luthr_lua_runtime_set_context(luthr_lua_runtime *runtime, void *context) {
  if (runtime != NULL && !in_trace(runtime)) {
    policy_check(pthread_mutex_lock(&runtime->policy_mutex));
    runtime->context = context;
    luthr_policy_context(luthr_policy_current(runtime), context);
    policy_check(pthread_mutex_unlock(&runtime->policy_mutex));
  }
}

/** Implements the public Lua warning callback operation. */
void luthr_lua_runtime_set_warning_callback(
    luthr_lua_runtime *runtime, luthr_lua_runtime_warning_fn callback,
    void *context) {
  if (runtime != NULL && !luthr_policy_tracing(runtime)) {
    policy_check(pthread_mutex_lock(&runtime->policy_mutex));
    runtime->warning_callback = callback;
    runtime->warning_context = context;
    policy_check(pthread_mutex_unlock(&runtime->policy_mutex));
  }
}

/** Implements the public Lua runtime line observer operation. */
luthr_lua_runtime_status
luthr_lua_runtime_set_trace_callback(luthr_lua_runtime *runtime,
                                     luthr_lua_runtime_trace_fn callback,
                                     void *context) {
  if (runtime == NULL || runtime->state == NULL || runtime->closing ||
      (policy_load(runtime->active_calls) && !in_trace(runtime))) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  policy_check(pthread_mutex_lock(&runtime->policy_mutex));
  runtime->trace_callback = callback;
  runtime->trace_context = callback != NULL ? context : NULL;
  policy_check(pthread_mutex_unlock(&runtime->policy_mutex));
  /* During observation, keep the active thread's instruction count intact.
   * The dispatcher sees replacement/removal on its next line event. */
  if (luthr_policy_tracing(runtime)) {
    luthr_policy_trace(luthr_policy_current(runtime), callback, context);
  }
  return LUTHR_LUA_RUNTIME_OK;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
void *luthr_lua_runtime_context(const luthr_lua_runtime *runtime) {
  luthr_lua_runtime *owner;
  void *context;
  if (runtime == NULL) {
    return NULL;
  }
  owner = (luthr_lua_runtime *)runtime;
  policy_check(pthread_mutex_lock(&owner->policy_mutex));
  context = runtime->context;
  policy_check(pthread_mutex_unlock(&owner->policy_mutex));
  return context;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
void *luthr_lua_runtime_context_from_state(void *lua_state) {
  luthr_lua_runtime *runtime;

  if (lua_state == NULL) {
    return NULL;
  }

  runtime = luthr_lua_runtime_from_state((lua_State *)lua_state);
  if (runtime == NULL) {
    return NULL;
  }
  {
    LuaPolicy *policy;
    policy = luthr_policy_from_state((lua_State *)lua_state);
    if (policy != NULL && policy->owner == runtime &&
        policy_load(policy->active))
      return luthr_policy_get_context(policy);
  }
  return luthr_lua_runtime_context(runtime);
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status
luthr_lua_runtime_openlibs(luthr_lua_runtime *runtime) {
  if (runtime == NULL || runtime->state == NULL || in_trace(runtime)) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }

  return luthr_lua_runtime_open_libs(runtime, LUTHR_LUA_RUNTIME_LIB_ALL);
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status luthr_lua_runtime_open_libs(luthr_lua_runtime *runtime,
                                                     int libs) {
  luthr_lua_runtime_status status;

  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      libs < 0) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }

  status = LUTHR_LUA_RUNTIME_OK;
  if ((libs & LUTHR_LUA_RUNTIME_LIB_BASE) != 0) {
    status = luthr_lua_runtime_open_one_lib(runtime, "_G", luaopen_base);
  }
  if (status == LUTHR_LUA_RUNTIME_OK &&
      (libs & LUTHR_LUA_RUNTIME_LIB_PACKAGE) != 0) {
    status = luthr_lua_runtime_open_one_lib(runtime, LUA_LOADLIBNAME,
                                            luaopen_package);
  }
  if (status == LUTHR_LUA_RUNTIME_OK &&
      (libs & LUTHR_LUA_RUNTIME_LIB_COROUTINE) != 0) {
    status = luthr_lua_runtime_open_one_lib(runtime, LUA_COLIBNAME,
                                            luaopen_coroutine);
  }
  if (status == LUTHR_LUA_RUNTIME_OK &&
      (libs & LUTHR_LUA_RUNTIME_LIB_TABLE) != 0) {
    status =
        luthr_lua_runtime_open_one_lib(runtime, LUA_TABLIBNAME, luaopen_table);
  }
  if (status == LUTHR_LUA_RUNTIME_OK &&
      (libs & LUTHR_LUA_RUNTIME_LIB_IO) != 0) {
    status = luthr_lua_runtime_open_one_lib(runtime, LUA_IOLIBNAME, luaopen_io);
  }
  if (status == LUTHR_LUA_RUNTIME_OK &&
      (libs & LUTHR_LUA_RUNTIME_LIB_OS) != 0) {
    status = luthr_lua_runtime_open_one_lib(runtime, LUA_OSLIBNAME, luaopen_os);
  }
  if (status == LUTHR_LUA_RUNTIME_OK &&
      (libs & LUTHR_LUA_RUNTIME_LIB_STRING) != 0) {
    status =
        luthr_lua_runtime_open_one_lib(runtime, LUA_STRLIBNAME, luaopen_string);
  }
  if (status == LUTHR_LUA_RUNTIME_OK &&
      (libs & LUTHR_LUA_RUNTIME_LIB_UTF8) != 0) {
    status =
        luthr_lua_runtime_open_one_lib(runtime, LUA_UTF8LIBNAME, luaopen_utf8);
  }
  if (status == LUTHR_LUA_RUNTIME_OK &&
      (libs & LUTHR_LUA_RUNTIME_LIB_MATH) != 0) {
    status =
        luthr_lua_runtime_open_one_lib(runtime, LUA_MATHLIBNAME, luaopen_math);
  }
  if (status == LUTHR_LUA_RUNTIME_OK &&
      (libs & LUTHR_LUA_RUNTIME_LIB_DEBUG) != 0) {
    status =
        luthr_lua_runtime_open_one_lib(runtime, LUA_DBLIBNAME, luaopen_debug);
    if (status == LUTHR_LUA_RUNTIME_OK) {
      status = luthr_lua_runtime_disable_debug_sethook_checked(runtime);
    }
  }

  if (status == LUTHR_LUA_RUNTIME_OK) {
    luthr_lua_runtime_clear_error(runtime);
  }
  return status;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status
luthr_lua_runtime_set_traceback(luthr_lua_runtime *runtime, int enabled) {
  if (runtime == NULL || runtime->state == NULL || in_trace(runtime)) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  runtime->traceback_enabled = enabled != 0;
  luthr_lua_runtime_clear_error(runtime);
  return LUTHR_LUA_RUNTIME_OK;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status
luthr_lua_runtime_set_instruction_limit(luthr_lua_runtime *runtime,
                                        int instruction_count) {
  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      instruction_count <= 0) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  policy_store(runtime->instruction_limit, instruction_count);
  luthr_lua_runtime_clear_error(runtime);
  return LUTHR_LUA_RUNTIME_OK;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status
luthr_lua_runtime_clear_instruction_limit(luthr_lua_runtime *runtime) {
  if (runtime == NULL || runtime->state == NULL || in_trace(runtime)) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  policy_store(runtime->instruction_limit, 0);
  luthr_lua_runtime_clear_error(runtime);
  return LUTHR_LUA_RUNTIME_OK;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status
luthr_lua_runtime_set_package_path(luthr_lua_runtime *runtime,
                                   const char *path) {
  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      path == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  return luthr_lua_runtime_set_package_field(runtime, "path", path);
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status
luthr_lua_runtime_prepend_package_path(luthr_lua_runtime *runtime,
                                       const char *path) {
  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      path == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  return luthr_lua_runtime_prepend_package_field(runtime, "path", path);
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status
luthr_lua_runtime_set_package_cpath(luthr_lua_runtime *runtime,
                                    const char *path) {
  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      path == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  return luthr_lua_runtime_set_package_field(runtime, "cpath", path);
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status
luthr_lua_runtime_prepend_package_cpath(luthr_lua_runtime *runtime,
                                        const char *path) {
  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      path == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  return luthr_lua_runtime_prepend_package_field(runtime, "cpath", path);
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status
luthr_lua_runtime_set_global_string(luthr_lua_runtime *runtime,
                                    const char *name, const char *value) {
  struct luthr_lua_runtime_global_string_context context;

  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      name == NULL || value == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  context.name = name;
  context.value = value;
  return luthr_lua_runtime_protected_call(
      runtime, luthr_lua_runtime_set_global_string_protected, &context, 0,
      LUTHR_LUA_RUNTIME_ERR_RUNTIME);
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status
luthr_lua_runtime_set_global_boolean(luthr_lua_runtime *runtime,
                                     const char *name, int value) {
  struct luthr_lua_runtime_global_boolean_context context;

  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      name == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  context.name = name;
  context.value = value;
  return luthr_lua_runtime_protected_call(
      runtime, luthr_lua_runtime_set_global_boolean_protected, &context, 0,
      LUTHR_LUA_RUNTIME_ERR_RUNTIME);
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status
luthr_lua_runtime_set_global_number(luthr_lua_runtime *runtime,
                                    const char *name, double value) {
  struct luthr_lua_runtime_global_number_context context;

  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      name == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  context.name = name;
  context.value = value;
  return luthr_lua_runtime_protected_call(
      runtime, luthr_lua_runtime_set_global_number_protected, &context, 0,
      LUTHR_LUA_RUNTIME_ERR_RUNTIME);
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status
luthr_lua_runtime_set_global_integer(luthr_lua_runtime *runtime,
                                     const char *name, long value) {
  struct luthr_lua_runtime_global_integer_context context;

  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      name == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  context.name = name;
  context.value = value;
  return luthr_lua_runtime_protected_call(
      runtime, luthr_lua_runtime_set_global_integer_protected, &context, 0,
      LUTHR_LUA_RUNTIME_ERR_RUNTIME);
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status
luthr_lua_runtime_register_c_module(luthr_lua_runtime *runtime,
                                    const char *module_name,
                                    luthr_lua_runtime_c_module_open_fn opener) {
  struct luthr_lua_runtime_c_module_context context;
  struct luthr_lua_runtime_c_module *module;
  luthr_lua_runtime_status status;

  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      module_name == NULL || opener == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }

  module =
      (struct luthr_lua_runtime_c_module *)luthr_lua_runtime_allocator_alloc(
          &runtime->allocator, sizeof(*module));
  if (module == NULL) {
    return luthr_lua_runtime_set_error(runtime, LUTHR_LUA_RUNTIME_ERR_ALLOC,
                                       "out of memory");
  }
  module->opener = opener;
  module->next = NULL;
  context.module_name = module_name;
  context.module = module;
  status = luthr_lua_runtime_protected_call(
      runtime, luthr_lua_runtime_register_c_module_protected, &context, 0,
      LUTHR_LUA_RUNTIME_ERR_RUNTIME);
  if (status == LUTHR_LUA_RUNTIME_OK) {
    module->next = runtime->c_modules;
    runtime->c_modules = module;
  } else {
    luthr_lua_runtime_allocator_free(&runtime->allocator, module,
                                     sizeof(*module));
  }
  return status;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status luthr_lua_runtime_register_lua_module(
    luthr_lua_runtime *runtime, const char *module_name,
    const unsigned char *source, size_t source_size, const char *chunk_name) {
  luthr_lua_runtime_status status;
  struct luthr_lua_runtime_chunk *chunk;
  struct luthr_lua_runtime_lua_module_context context;

  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      module_name == NULL || source == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }

  chunk = (struct luthr_lua_runtime_chunk *)luthr_lua_runtime_allocator_alloc(
      &runtime->allocator, sizeof(*chunk));
  if (chunk == NULL) {
    return luthr_lua_runtime_set_error(runtime, LUTHR_LUA_RUNTIME_ERR_ALLOC,
                                       "out of memory");
  }
  memset(chunk, 0, sizeof(*chunk));

  chunk->module_name =
      luthr_lua_runtime_strdup(runtime, module_name, &chunk->module_name_size);
  chunk->chunk_name = luthr_lua_runtime_strdup(
      runtime, chunk_name != NULL ? chunk_name : module_name,
      &chunk->chunk_name_size);
  chunk->source_alloc_size = source_size != 0 ? source_size : 1;
  chunk->source = (unsigned char *)luthr_lua_runtime_allocator_alloc(
      &runtime->allocator, chunk->source_alloc_size);
  if (chunk->module_name == NULL || chunk->chunk_name == NULL ||
      chunk->source == NULL) {
    luthr_lua_runtime_allocator_free(&runtime->allocator, chunk->module_name,
                                     chunk->module_name_size);
    luthr_lua_runtime_allocator_free(&runtime->allocator, chunk->chunk_name,
                                     chunk->chunk_name_size);
    luthr_lua_runtime_allocator_free(&runtime->allocator, chunk->source,
                                     chunk->source_alloc_size);
    luthr_lua_runtime_allocator_free(&runtime->allocator, chunk,
                                     sizeof(*chunk));
    return luthr_lua_runtime_set_error(runtime, LUTHR_LUA_RUNTIME_ERR_ALLOC,
                                       "out of memory");
  }
  if (source_size != 0) {
    memcpy(chunk->source, source, source_size);
  }
  chunk->source_size = source_size;

  context.chunk = chunk;
  status = luthr_lua_runtime_protected_call(
      runtime, luthr_lua_runtime_register_lua_module_protected, &context, 0,
      LUTHR_LUA_RUNTIME_ERR_RUNTIME);
  if (status != LUTHR_LUA_RUNTIME_OK) {
    luthr_lua_runtime_free_chunk(runtime, chunk);
    return status;
  }

  chunk->next = runtime->chunks;
  runtime->chunks = chunk;

  luthr_lua_runtime_clear_error(runtime);
  return LUTHR_LUA_RUNTIME_OK;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status luthr_lua_runtime_require(luthr_lua_runtime *runtime,
                                                   const char *module_name) {
  struct luthr_lua_runtime_require_context context;
  luthr_lua_runtime_status status;

  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      module_name == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }

  context.module_name = module_name;
  status = luthr_lua_runtime_protected_call(
      runtime, luthr_lua_runtime_require_protected, &context, 1,
      LUTHR_LUA_RUNTIME_ERR_RUNTIME);
  if (status != LUTHR_LUA_RUNTIME_OK) {
    return status;
  }
  lua_pop(runtime->state, 1);
  luthr_lua_runtime_clear_error(runtime);
  return LUTHR_LUA_RUNTIME_OK;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status luthr_lua_runtime_run_file(luthr_lua_runtime *runtime,
                                                    const char *path, int argc,
                                                    const char *const *argv,
                                                    int flags) {
  luthr_lua_runtime_status status;
  struct luthr_lua_runtime_load_file_context context;

  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      path == NULL || argc < 0) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  if (argc > 0 && argv == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }

  status = luthr_lua_runtime_maybe_openlibs(runtime, flags);
  if (status != LUTHR_LUA_RUNTIME_OK) {
    return status;
  }

  context.args.script_name = path;
  context.args.argc = argc;
  context.args.argv = argv;
  context.path = path;
  status = luthr_lua_runtime_protected_call(
      runtime, luthr_lua_runtime_load_file_protected, &context, 1,
      LUTHR_LUA_RUNTIME_ERR_LOAD);
  if (status != LUTHR_LUA_RUNTIME_OK) {
    return status;
  }

  status = luthr_lua_runtime_call(runtime, 0, LUA_MULTRET);
  if (status != LUTHR_LUA_RUNTIME_OK) {
    return status;
  }

  lua_settop(runtime->state, 0);
  luthr_lua_runtime_clear_error(runtime);
  return LUTHR_LUA_RUNTIME_OK;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
luthr_lua_runtime_status luthr_lua_runtime_run_buffer(
    luthr_lua_runtime *runtime, const unsigned char *source, size_t source_size,
    const char *chunk_name, int argc, const char *const *argv, int flags) {
  luthr_lua_runtime_status status;
  struct luthr_lua_runtime_load_buffer_context context;

  if (runtime == NULL || runtime->state == NULL || in_trace(runtime) ||
      source == NULL || argc < 0) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  if (argc > 0 && argv == NULL) {
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }

  status = luthr_lua_runtime_maybe_openlibs(runtime, flags);
  if (status != LUTHR_LUA_RUNTIME_OK) {
    return status;
  }

  context.args.script_name = chunk_name;
  context.args.argc = argc;
  context.args.argv = argv;
  context.source = source;
  context.source_size = source_size;
  context.chunk_name = chunk_name;
  status = luthr_lua_runtime_protected_call(
      runtime, luthr_lua_runtime_load_buffer_protected, &context, 1,
      LUTHR_LUA_RUNTIME_ERR_LOAD);
  if (status != LUTHR_LUA_RUNTIME_OK) {
    return status;
  }

  status = luthr_lua_runtime_call(runtime, 0, LUA_MULTRET);
  if (status != LUTHR_LUA_RUNTIME_OK) {
    return status;
  }

  lua_settop(runtime->state, 0);
  luthr_lua_runtime_clear_error(runtime);
  return LUTHR_LUA_RUNTIME_OK;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
const char *luthr_lua_runtime_error(const luthr_lua_runtime *runtime) {
  if (runtime == NULL || runtime->last_error == NULL) {
    return "";
  }
  return runtime->last_error;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
void luthr_lua_runtime_clear_error(luthr_lua_runtime *runtime) {
  if (runtime == NULL || in_trace(runtime)) {
    return;
  }
  luthr_lua_runtime_allocator_free(
      &runtime->allocator, runtime->last_error,
      runtime->last_error != NULL ? strlen(runtime->last_error) + 1 : 0);
  runtime->last_error = NULL;
}

/** Implements the public Lua runtime facade function declared in
 * <luthr/lua_runtime.h>. */
const char *luthr_lua_runtime_status_string(luthr_lua_runtime_status status) {
  switch (status) {
  case LUTHR_LUA_RUNTIME_OK:
    return "ok";
  case LUTHR_LUA_RUNTIME_ERR_ARG:
    return "invalid argument";
  case LUTHR_LUA_RUNTIME_ERR_ALLOC:
    return "allocation failure";
  case LUTHR_LUA_RUNTIME_ERR_LOAD:
    return "load error";
  case LUTHR_LUA_RUNTIME_ERR_RUNTIME:
    return "runtime error";
  case LUTHR_LUA_RUNTIME_ERR_LIMIT:
    return "limit exceeded";
  case LUTHR_LUA_RUNTIME_PENDING:
    return "pending";
  case LUTHR_LUA_RUNTIME_CANCELLED:
    return "cancelled";
  case LUTHR_LUA_RUNTIME_ERR_STATE:
    return "invalid operation state";
  case LUTHR_LUA_RUNTIME_ERR_CLOSED:
    return "admission closed";
  case LUTHR_LUA_RUNTIME_BUSY:
    return "resources still active";
  }

  return "unknown status";
}
