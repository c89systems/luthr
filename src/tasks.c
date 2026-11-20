#define _GNU_SOURCE
#include "cpu_capacity.h"
#include "policy.h"
#include <errno.h>
#include <lapi.h>
#include <lauxlib.h>
#include <limits.h>
#include <lmem.h>
#include <lpipe.h>
#include <lscheduler.h>
#include <luthr/tasks.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

struct luthr_lua_scheduler {
  LuaScheduler engine;
  lua_State *host;
  unsigned handles;
  unsigned registrations;
  int completion;
  struct luthr_capacity capacity;
};
struct luthr_lua_task {
  LuaScheduledTask task;
  luthr_lua_scheduler *scheduler;
  int collected;
};
struct luthr_lua_callback {
  LuaCallback callback;
  luthr_lua_scheduler *scheduler;
  luthr_lua_task_options options;
  unsigned entered;
};
struct luthr_lua_carrier_pin {
  LuaCarrierPin pin;
  global_State *global;
};

static void check(int status) {
  if (status != 0)
    abort();
}
static void *allocate(lua_State *L, size_t size) {
  void *block;
  lua_lock(L);
  block = luaM_alloccontrol(L, size);
  if (block != NULL)
    memset(block, 0, size);
  lua_unlock(L);
  return block;
}
static void release(lua_State *L, void *block, size_t size) {
  lua_lock(L);
  luaM_free_(L, block, size);
  lua_unlock(L);
}
static void completed(void *context) {
  luthr_lua_scheduler *scheduler = context;
  uint64_t notice = 1;
  ssize_t count;
  do {
    count = write(scheduler->completion, &notice, sizeof(notice));
  } while (count < 0 && errno == EINTR);
  if (count < 0 && errno != EAGAIN)
    abort();
}

static LuaPolicy *default_policy(lua_State *L, const void *settings,
                                 void *owner) {
  return luthr_policy_new(L, settings, owner);
}

luthr_lua_runtime_status
luthr_lua_scheduler_new(luthr_lua_state *state,
                        const luthr_lua_scheduler_config *config,
                        luthr_lua_scheduler **out) {
  luthr_lua_scheduler_config requested = {0};
  LuaSchedulerConfig selected;
  luthr_lua_scheduler *scheduler;
  lua_State *L = (lua_State *)state;
  unsigned automatic;
  int status;
  if (out == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  *out = NULL;
  if (L == NULL || luaQ_current() != NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  if (config != NULL)
    requested = *config;
  L = mainthread(G(L));
  scheduler = allocate(L, sizeof(*scheduler));
  if (scheduler == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ALLOC;
  scheduler->host = L;
  scheduler->completion = -1;
  if (luthr_capacity_read(&scheduler->capacity, "/proc/self/cgroup",
                          "/proc/self/mountinfo") != 0)
    scheduler->capacity.cpus = 1;
  automatic =
      scheduler->capacity.effective_milli != 0
          ? (unsigned)((scheduler->capacity.effective_milli + 999) / 1000)
          : scheduler->capacity.cpus;
  /* Candidate automatic sizing is constrained by affinity/quota. Final
   * integrated workload measurements govern tuning of this private ceiling. */
  if (automatic > 8)
    automatic = 8;
  if (automatic == 0)
    automatic = 1;
  selected.slots = requested.parallelism ? requested.parallelism : automatic;
  selected.native_limit =
      requested.native_limit ? requested.native_limit : selected.slots;
  if (selected.native_limit > UINT_MAX - selected.slots) {
    release(L, scheduler, sizeof(*scheduler));
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  selected.carriers = requested.carrier_limit
                          ? requested.carrier_limit
                          : selected.slots + selected.native_limit;
  selected.callback_limit =
      requested.callback_limit ? requested.callback_limit : selected.slots;
  selected.quantum_polls = requested.quantum_instructions
                               ? (requested.quantum_instructions - 1) / 1024 + 1
                               : 0;
  scheduler->completion = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (scheduler->completion < 0) {
    release(L, scheduler, sizeof(*scheduler));
    return LUTHR_LUA_RUNTIME_ERR_ALLOC;
  }
  status = luaSC_init(L, &scheduler->engine, &selected);
  if (status != 0) {
    close(scheduler->completion);
    release(L, scheduler, sizeof(*scheduler));
    return status == EINVAL  ? LUTHR_LUA_RUNTIME_ERR_ARG
           : status == EBUSY ? LUTHR_LUA_RUNTIME_BUSY
                             : LUTHR_LUA_RUNTIME_ERR_ALLOC;
  }
  scheduler->engine.completed = completed;
  scheduler->engine.completion_context = scheduler;
  if (G(L)->policy_factory == NULL)
    luthr_policy_set_factory(L, default_policy, NULL);
  *out = scheduler;
  return LUTHR_LUA_RUNTIME_OK;
}

int luthr_lua_scheduler_fd(const luthr_lua_scheduler *scheduler) {
  return scheduler != NULL ? scheduler->completion : -1;
}
luthr_lua_runtime_status
luthr_lua_scheduler_drain(luthr_lua_scheduler *scheduler) {
  uint64_t notice;
  ssize_t count;
  if (scheduler == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  do {
    count = read(scheduler->completion, &notice, sizeof(notice));
  } while (count < 0 && errno == EINTR);
  return count == sizeof(notice) || (count < 0 && errno == EAGAIN)
             ? LUTHR_LUA_RUNTIME_OK
             : LUTHR_LUA_RUNTIME_ERR_RUNTIME;
}
luthr_lua_runtime_status
luthr_lua_scheduler_shutdown(luthr_lua_scheduler *scheduler) {
  int status;
  if (scheduler == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  status = luaSC_shutdown(&scheduler->engine);
  return status == LUA_OK ? LUTHR_LUA_RUNTIME_OK : LUTHR_LUA_RUNTIME_PENDING;
}
luthr_lua_runtime_status
luthr_lua_scheduler_free(luthr_lua_scheduler *scheduler) {
  lua_State *L;
  if (scheduler == NULL)
    return LUTHR_LUA_RUNTIME_OK;
  if (luaQ_current() != NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  if (scheduler->handles != 0 || scheduler->registrations != 0)
    return LUTHR_LUA_RUNTIME_BUSY;
  L = scheduler->host;
  if (luaSC_destroy(L, &scheduler->engine) != 0)
    return LUTHR_LUA_RUNTIME_BUSY;
  close(scheduler->completion);
  release(L, scheduler, sizeof(*scheduler));
  return LUTHR_LUA_RUNTIME_OK;
}
luthr_lua_runtime_status
luthr_lua_scheduler_snapshot(luthr_lua_scheduler *scheduler,
                             luthr_lua_scheduler_stats *out) {
  LuaScheduler *engine;
  luthr_lua_scheduler_stats snapshot = {0};
  LuaScheduledTask *task;
  if (scheduler == NULL || out == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  engine = &scheduler->engine;
  snapshot.parallelism = engine->config.slots;
  snapshot.carrier_limit = engine->config.carriers;
  snapshot.native_limit = engine->config.native_limit;
  snapshot.callback_limit = engine->config.callback_limit;
  snapshot.affinity_cpus = scheduler->capacity.cpus;
  snapshot.quota_milli_cpus =
      (unsigned long)scheduler->capacity.effective_milli;
  snapshot.runnable = __atomic_load_n(&engine->runnable, __ATOMIC_ACQUIRE);
  snapshot.native_active = luaSC_nativeactive(engine);
  check(pthread_mutex_lock(&engine->mutex));
  snapshot.callbacks_active = engine->callbacks_active;
  snapshot.tasks = scheduler->handles;
  for (task = engine->tasks; task != NULL; task = task->registered_next) {
    if (task->complete)
      snapshot.finished++;
  }
  check(pthread_mutex_unlock(&engine->mutex));
  for (unsigned i = 0; i < engine->config.slots; i++)
    if (__atomic_load_n(&engine->slots[i].owner, __ATOMIC_ACQUIRE) != NULL)
      snapshot.running++;
  *out = snapshot;
  return LUTHR_LUA_RUNTIME_OK;
}

luthr_lua_runtime_status
luthr_lua_task_submit(luthr_lua_scheduler *scheduler, luthr_lua_state *thread,
                      int nargs, const luthr_lua_task_options *options,
                      luthr_lua_task **out) {
  luthr_lua_task *task;
  LuaPolicy *policy;
  lua_State *L = (lua_State *)thread;
  int status;
  if (out == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  *out = NULL;
  if (scheduler == NULL || L == NULL || nargs < 0 || luaQ_current() != NULL ||
      G(L) != scheduler->engine.global)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  task = allocate(scheduler->host, sizeof(*task));
  if (task == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ALLOC;
  policy = G(L)->policy_factory(L, options, G(L)->policy_context);
  if (policy == NULL) {
    release(scheduler->host, task, sizeof(*task));
    return LUTHR_LUA_RUNTIME_ERR_ALLOC;
  }
  task->task.policy = policy;
  task->scheduler = scheduler;
  status = luaSC_submit(&scheduler->engine, &task->task, L, nargs);
  if (status != LUA_OK) {
    lua_lock(scheduler->host);
    luaCP_release(scheduler->host, policy);
    lua_unlock(scheduler->host);
    release(scheduler->host, task, sizeof(*task));
    return status == ESHUTDOWN ? LUTHR_LUA_RUNTIME_ERR_CLOSED
           : status == LUA_TK_INVALID || status == EINVAL
               ? LUTHR_LUA_RUNTIME_ERR_ARG
               : LUTHR_LUA_RUNTIME_ERR_ALLOC;
  }
  check(pthread_mutex_lock(&scheduler->engine.mutex));
  scheduler->handles++;
  check(pthread_mutex_unlock(&scheduler->engine.mutex));
  *out = task;
  return LUTHR_LUA_RUNTIME_OK;
}

struct preparation {
  const unsigned char *source;
  size_t size;
  const char *name;
  lua_State *thread;
  int status;
};
static int prepare(lua_State *L) {
  struct preparation *context = lua_touserdata(L, 1);
  lua_State *thread = lua_newthread(L);
  context->thread = thread;
  context->status =
      luaL_loadbufferx(thread, (const char *)context->source, context->size,
                       context->name != NULL ? context->name : "=task", NULL);
  if (context->status != LUA_OK) {
    lua_xmove(thread, L, 1);
    return lua_error(L);
  }
  return 1;
}
luthr_lua_runtime_status luthr_lua_task_submit_buffer(
    luthr_lua_scheduler *scheduler, const unsigned char *source, size_t size,
    const char *name, const luthr_lua_task_options *options,
    luthr_lua_task **out) {
  lua_State *L;
  int top, status;
  luthr_lua_runtime_status result;
  struct preparation context = {source, size, name, NULL, LUA_OK};
  if (out == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  *out = NULL;
  if (scheduler == NULL || source == NULL || luaQ_current() != NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  L = scheduler->host;
  top = lua_gettop(L);
  lua_pushcfunction(L, prepare);
  lua_pushlightuserdata(L, &context);
  status = lua_pcall(L, 1, 1, 0);
  if (status != LUA_OK)
    return context.status == LUA_ERRMEM || status == LUA_ERRMEM
               ? LUTHR_LUA_RUNTIME_ERR_ALLOC
               : LUTHR_LUA_RUNTIME_ERR_LOAD;
  result = luthr_lua_task_submit(scheduler, (luthr_lua_state *)context.thread,
                                 0, options, out);
  lua_settop(L, top);
  return result;
}

static luthr_lua_task_result outcome(luthr_lua_task *task) {
  LuaTaskRoot *root = &task->task.root;
  LuaWorld *world = &root->global->world;
  luthr_lua_task_result result;
  check(pthread_mutex_lock(&world->mutex));
  result.lua_status = root->status;
  result.result_count = root->results;
  if (__atomic_load_n(&task->task.policy->exhausted, __ATOMIC_ACQUIRE))
    result.status = LUTHR_LUA_RUNTIME_ERR_LIMIT;
  else if (__atomic_load_n(&root->stop_delivered, __ATOMIC_ACQUIRE))
    result.status = LUTHR_LUA_RUNTIME_CANCELLED;
  else
    result.status = root->status == LUA_OK ? LUTHR_LUA_RUNTIME_OK
                    : root->status == LUA_ERRMEM
                        ? LUTHR_LUA_RUNTIME_ERR_ALLOC
                        : LUTHR_LUA_RUNTIME_ERR_RUNTIME;
  check(pthread_mutex_unlock(&world->mutex));
  return result;
}
luthr_lua_runtime_status luthr_lua_task_poll(luthr_lua_task *task,
                                             luthr_lua_task_result *out) {
  int complete;
  if (task == NULL || out == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  if (__atomic_load_n(&task->collected, __ATOMIC_ACQUIRE))
    return LUTHR_LUA_RUNTIME_ERR_STATE;
  check(pthread_mutex_lock(&task->scheduler->engine.mutex));
  complete = task->task.complete;
  check(pthread_mutex_unlock(&task->scheduler->engine.mutex));
  if (!complete)
    return LUTHR_LUA_RUNTIME_PENDING;
  *out = outcome(task);
  return LUTHR_LUA_RUNTIME_OK;
}
luthr_lua_runtime_status luthr_lua_task_wait(luthr_lua_task *task,
                                             long timeout_ms,
                                             luthr_lua_task_result *out) {
  LuaScheduler *engine;
  struct timespec deadline;
  int status = 0;
  if (task == NULL || out == NULL || luaQ_current() != NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  if (timeout_ms == 0)
    return luthr_lua_task_poll(task, out);
  if (timeout_ms > 0) {
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
      return LUTHR_LUA_RUNTIME_ERR_RUNTIME;
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000;
    }
  }
  engine = &task->scheduler->engine;
  check(pthread_mutex_lock(&engine->mutex));
  while (!task->task.complete && status != ETIMEDOUT) {
    status = timeout_ms < 0
                 ? pthread_cond_wait(&engine->changed, &engine->mutex)
                 : pthread_cond_timedwait(&engine->changed, &engine->mutex,
                                          &deadline);
    if (status != 0 && status != ETIMEDOUT)
      abort();
  }
  check(pthread_mutex_unlock(&engine->mutex));
  return luthr_lua_task_poll(task, out);
}
luthr_lua_runtime_status luthr_lua_task_cancel(luthr_lua_task *task) {
  if (task == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  return luaTK_requeststop(&task->task.root) == LUA_OK
             ? LUTHR_LUA_RUNTIME_OK
             : LUTHR_LUA_RUNTIME_ERR_STATE;
}
luthr_lua_runtime_status luthr_lua_task_collect(luthr_lua_task *task,
                                                luthr_lua_state *receiver,
                                                luthr_lua_task_result *out) {
  lua_State *L;
  luthr_lua_task_result result;
  luthr_lua_runtime_status status;
  int lua_status, count, top, collected;
  if (task == NULL || out == NULL || luaQ_current() != NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  status = luthr_lua_task_poll(task, &result);
  if (status != LUTHR_LUA_RUNTIME_OK)
    return status;
  L = receiver != NULL ? (lua_State *)receiver : task->scheduler->host;
  if (G(L) != task->scheduler->engine.global)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  top = lua_gettop(L);
  collected = luaSC_collect(L, &task->task, &lua_status, &count);
  if (collected != LUA_OK)
    return collected == LUA_TK_STACK ? LUTHR_LUA_RUNTIME_ERR_ALLOC
                                     : LUTHR_LUA_RUNTIME_ERR_STATE;
  if (receiver == NULL)
    lua_settop(L, top);
  __atomic_store_n(&task->collected, 1, __ATOMIC_RELEASE);
  *out = result;
  return LUTHR_LUA_RUNTIME_OK;
}
luthr_lua_runtime_status luthr_lua_task_free(luthr_lua_task *task) {
  luthr_lua_scheduler *scheduler;
  if (task == NULL)
    return LUTHR_LUA_RUNTIME_OK;
  if (luaQ_current() != NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  if (!__atomic_load_n(&task->collected, __ATOMIC_ACQUIRE))
    return LUTHR_LUA_RUNTIME_BUSY;
  scheduler = task->scheduler;
  lua_lock(scheduler->host);
  luaCP_release(scheduler->host, task->task.policy);
  luaM_free_(scheduler->host, task, sizeof(*task));
  lua_unlock(scheduler->host);
  check(pthread_mutex_lock(&scheduler->engine.mutex));
  scheduler->handles--;
  check(pthread_mutex_unlock(&scheduler->engine.mutex));
  return LUTHR_LUA_RUNTIME_OK;
}
const char *luthr_lua_task_error(luthr_lua_task *task) {
  luthr_lua_task_result result;
  lua_State *L;
  if (luthr_lua_task_poll(task, &result) != LUTHR_LUA_RUNTIME_OK ||
      result.lua_status == LUA_OK)
    return "";
  L = task->task.root.thread;
  return lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1)
                                        : "non-string Lua error";
}
void luthr_lua_pipe_new(luthr_lua_state *state, unsigned capacity) {
  luaP_newpipe((lua_State *)state, capacity);
}
void *luthr_lua_task_context(luthr_lua_state *state) {
  lua_State *L = (lua_State *)state;
  void *context = NULL;
  if (L == NULL)
    return NULL;
  lua_lock(L);
  if (L->policy != NULL &&
      __atomic_load_n(&L->policy->active, __ATOMIC_ACQUIRE))
    context = luthr_policy_get_context(L->policy);
  lua_unlock(L);
  return context;
}

luthr_lua_runtime_status
luthr_lua_callback_new(luthr_lua_scheduler *scheduler, luthr_lua_state *state,
                       unsigned entry_limit,
                       const luthr_lua_task_options *options,
                       luthr_lua_callback **out) {
  luthr_lua_callback *callback;
  lua_State *L = (lua_State *)state;
  int status;
  if (out == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  *out = NULL;
  if (scheduler == NULL || L == NULL || G(L) != scheduler->engine.global ||
      luaQ_current() != NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  check(pthread_mutex_lock(&scheduler->engine.mutex));
  status = scheduler->engine.closed;
  check(pthread_mutex_unlock(&scheduler->engine.mutex));
  if (status)
    return LUTHR_LUA_RUNTIME_ERR_CLOSED;
  callback = allocate(scheduler->host, sizeof(*callback));
  if (callback == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ALLOC;
  status = luaCB_init(L, &callback->callback, entry_limit);
  if (status != LUA_OK) {
    release(scheduler->host, callback, sizeof(*callback));
    return status == LUA_ERRMEM ? LUTHR_LUA_RUNTIME_ERR_ALLOC
                                : LUTHR_LUA_RUNTIME_ERR_ARG;
  }
  if (options != NULL) {
    callback->options = *options;
    callback->callback.policy_settings = &callback->options;
  }
  callback->scheduler = scheduler;
  check(pthread_mutex_lock(&scheduler->engine.mutex));
  scheduler->registrations++;
  check(pthread_mutex_unlock(&scheduler->engine.mutex));
  *out = callback;
  return LUTHR_LUA_RUNTIME_OK;
}

struct callback_call {
  luthr_lua_callback_invoke_fn invoke;
  void *context;
};
static void invoke_callback(lua_State *L, void *context) {
  struct callback_call *call = context;
  call->invoke((luthr_lua_state *)L, call->context);
}
luthr_lua_runtime_status
luthr_lua_callback_call(luthr_lua_callback *callback,
                        luthr_lua_callback_invoke_fn invoke, void *context,
                        int *lua_status, char *error, size_t error_size) {
  LuaCallbackOutcome outcome = {0};
  struct callback_call call = {invoke, context};
  luthr_lua_scheduler *scheduler;
  int status;
  if (callback == NULL || invoke == NULL || (error_size != 0 && error == NULL))
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  scheduler = callback->scheduler;
  __atomic_fetch_add(&callback->entered, 1, __ATOMIC_ACQ_REL);
  status = luaSC_callback(&scheduler->engine, &callback->callback,
                          invoke_callback, &call, error, error_size, &outcome);
  __atomic_fetch_sub(&callback->entered, 1, __ATOMIC_ACQ_REL);
  if (status < 0)
    return status == LUA_CB_REVOKED    ? LUTHR_LUA_RUNTIME_ERR_CLOSED
           : status == LUA_CB_CAPACITY ? LUTHR_LUA_RUNTIME_BUSY
                                       : LUTHR_LUA_RUNTIME_ERR_ARG;
  if (lua_status != NULL)
    *lua_status = outcome.lua_status;
  if (outcome.exhausted)
    return LUTHR_LUA_RUNTIME_ERR_LIMIT;
  if (outcome.stopped)
    return LUTHR_LUA_RUNTIME_CANCELLED;
  return status == LUA_OK       ? LUTHR_LUA_RUNTIME_OK
         : status == LUA_ERRMEM ? LUTHR_LUA_RUNTIME_ERR_ALLOC
                                : LUTHR_LUA_RUNTIME_ERR_RUNTIME;
}
luthr_lua_runtime_status
luthr_lua_callback_revoke(luthr_lua_callback *callback) {
  if (callback == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  luaCB_revoke(&callback->callback);
  return LUTHR_LUA_RUNTIME_OK;
}
luthr_lua_runtime_status luthr_lua_callback_free(luthr_lua_callback *callback) {
  luthr_lua_scheduler *scheduler;
  int status;
  if (callback == NULL)
    return LUTHR_LUA_RUNTIME_OK;
  if (__atomic_load_n(&callback->entered, __ATOMIC_ACQUIRE) != 0)
    return LUTHR_LUA_RUNTIME_BUSY;
  if (luaQ_current() != NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  scheduler = callback->scheduler;
  status = luaCB_destroy(scheduler->host, &callback->callback);
  if (status != LUA_OK)
    return status == LUA_CB_PENDING ? LUTHR_LUA_RUNTIME_BUSY
                                    : LUTHR_LUA_RUNTIME_ERR_STATE;
  release(scheduler->host, callback, sizeof(*callback));
  check(pthread_mutex_lock(&scheduler->engine.mutex));
  scheduler->registrations--;
  check(pthread_mutex_unlock(&scheduler->engine.mutex));
  return LUTHR_LUA_RUNTIME_OK;
}

luthr_lua_runtime_status
luthr_lua_carrier_pin_new(luthr_lua_state *state, luthr_lua_carrier_pin **out) {
  lua_State *L = (lua_State *)state;
  luthr_lua_carrier_pin *pin;
  int status;
  if (out == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  *out = NULL;
  if (L == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  pin = allocate(L, sizeof(*pin));
  if (pin == NULL)
    return LUTHR_LUA_RUNTIME_ERR_ALLOC;
  pin->global = G(L);
  status = luaQ_pincarrier(&pin->pin);
  if (status != LUA_OK) {
    release(L, pin, sizeof(*pin));
    return LUTHR_LUA_RUNTIME_BUSY;
  }
  *out = pin;
  return LUTHR_LUA_RUNTIME_OK;
}
int luthr_lua_carrier_pin_owner(const luthr_lua_carrier_pin *pin) {
  return pin != NULL && luaQ_pinowner(&pin->pin);
}
luthr_lua_runtime_status luthr_lua_carrier_unpin(luthr_lua_state *state,
                                                 luthr_lua_carrier_pin *pin) {
  lua_State *L = (lua_State *)state;
  if (L == NULL || pin == NULL || G(L) != pin->global)
    return LUTHR_LUA_RUNTIME_ERR_ARG;
  if (luaQ_unpincarrier(&pin->pin) != LUA_OK)
    return LUTHR_LUA_RUNTIME_ERR_STATE;
  release(L, pin, sizeof(*pin));
  return LUTHR_LUA_RUNTIME_OK;
}
