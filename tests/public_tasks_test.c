#define _GNU_SOURCE
#include <luthr/tasks.h>
#include <poll.h>
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

struct fixture;
struct context {
  struct fixture *fixture;
  unsigned id, calls, traces;
  int replace, block;
};
struct fixture {
  luthr_lua_runtime *runtime;
  luthr_lua_scheduler *scheduler;
  luthr_lua_state *state;
  luthr_lua_task *tasks[4];
  luthr_lua_callback *callback;
  struct context contexts[2];
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  unsigned entered, release;
  size_t live, allocations;
  unsigned failing, failed;
  size_t fail_size;
  int allocating;
};
struct invocation {
  struct fixture *fixture;
  int argument, value, lua_status;
  luthr_lua_runtime_status status;
  char error[128];
};
static struct fixture *active;

static void *allocate(void *context, size_t size) {
  struct fixture *f = context;
  void *block;
  require(!f->allocating);
  f->allocating = 1;
  f->allocations++;
  if (f->failing != 0 && size >= f->fail_size) {
    f->failing--;
    f->failed++;
    f->allocating = 0;
    return NULL;
  }
  block = malloc(size);
  if (block != NULL)
    f->live += size;
  f->allocating = 0;
  return block;
}
static void deallocate(void *context, void *block, size_t size) {
  struct fixture *f = context;
  require(!f->allocating && f->live >= size);
  f->allocating = 1;
  f->live -= size;
  free(block);
  f->allocating = 0;
}
static void lock(struct fixture *f) {
  require(pthread_mutex_lock(&f->mutex) == 0);
}
static void unlock(struct fixture *f) {
  require(pthread_mutex_unlock(&f->mutex) == 0);
}
static void change(struct fixture *f) {
  require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
}
static void signal_change(struct fixture *f) {
  require(pthread_cond_broadcast(&f->changed) == 0);
}
static unsigned integer(luthr_lua_state *L, int index) {
  luthr_lua_integer value = luthr_lua_tointeger(L, index);
  require(value.high == 0);
  return value.low;
}
static void push_integer(luthr_lua_state *L, unsigned value) {
  luthr_lua_pushinteger(L, luthr_lua_integer_make(0, value));
}
static int block(luthr_lua_state *L) {
  struct fixture *f = active;
  lock(f);
  f->entered++;
  signal_change(f);
  while (!f->release)
    change(f);
  unlock(f);
  push_integer(L, 42);
  return 1;
}
static int context_id(luthr_lua_state *L) {
  struct context *context = luthr_lua_runtime_context_from_state(L);
  require(context == luthr_lua_task_context(L));
  require(context != NULL && context->id == integer(L, 1));
  lock(context->fixture);
  context->calls++;
  unlock(context->fixture);
  push_integer(L, 42);
  return 1;
}
static int nested_facade(luthr_lua_state *L) {
  struct fixture *f = active;
  const unsigned char source[] = "context_id(1); while true do end";
  require(luthr_lua_task_context(L) == &f->contexts[0]);
  (void)block(L);
  require(luthr_lua_runtime_run_buffer(f->runtime, source, sizeof(source) - 1,
                                       "=nested-facade", 0, NULL,
                                       0) == LUTHR_LUA_RUNTIME_ERR_LIMIT);
  require(luthr_lua_task_context(L) == &f->contexts[0]);
  push_integer(L, 42);
  return 1;
}
static int big_allocation(luthr_lua_state *L) {
  (void)luthr_lua_newuserdatauv(L, 16384, 0);
  return 0;
}
static int resume_raw(luthr_lua_state *L) {
  luthr_lua_state *child = luthr_lua_tothread(L, 1);
  int results, status;
  require(child != NULL);
  status = luthr_lua_resume(child, L, 0, &results);
  push_integer(L, (unsigned)status);
  return 1;
}
static int pinned(luthr_lua_state *L) {
  luthr_lua_carrier_pin *pin;
  pthread_t carrier = pthread_self();
  int status;
  require(luthr_lua_carrier_pin_new(L, &pin) == LUTHR_LUA_RUNTIME_OK);
  require(luthr_lua_carrier_pin_owner(pin));
  luthr_lua_pushvalue(L, 1);
  status = luthr_lua_pcall(L, 0, 1, 0);
  require(pthread_equal(carrier, pthread_self()) &&
          luthr_lua_carrier_pin_owner(pin));
  require(luthr_lua_carrier_unpin(L, pin) == LUTHR_LUA_RUNTIME_OK);
  if (status != LUTHR_LUA_OK)
    return luthr_lua_error(L);
  return 1;
}
static void trace(void *data, const luthr_lua_runtime_trace_event *event) {
  struct context *context = data;
  struct fixture *f = context->fixture;
  require(event != NULL && event->line > 0 && event->source_size != 0);
  require(luthr_lua_runtime_state(f->runtime) == NULL);
  require(luthr_lua_runtime_set_global_integer(f->runtime, "forbidden", 1) ==
          LUTHR_LUA_RUNTIME_ERR_ARG);
  lock(f);
  context->traces++;
  if (context->block) {
    f->entered++;
    signal_change(f);
    while (!f->release)
      change(f);
  }
  unlock(f);
  if (context->replace) {
    context->replace = 0;
    require(luthr_lua_runtime_set_trace_callback(f->runtime, NULL, NULL) ==
            LUTHR_LUA_RUNTIME_OK);
  }
}
static void execute(struct fixture *f, const char *source) {
  require(luthr_lua_l_dostring(f->state, source) == LUTHR_LUA_OK);
  luthr_lua_settop(f->state, 0);
}
static void init(struct fixture *f, int automatic) {
  luthr_lua_runtime_allocator_config allocator;
  luthr_lua_scheduler_config config;
  memset(&allocator, 0, sizeof(allocator));
  allocator.user = f;
  allocator.alloc_fn = allocate;
  allocator.free_fn = deallocate;
  require(luthr_lua_runtime_new_with_allocator(&f->runtime, &allocator) ==
          LUTHR_LUA_RUNTIME_OK);
  require(luthr_lua_runtime_openlibs(f->runtime) == LUTHR_LUA_RUNTIME_OK);
  f->state = luthr_lua_runtime_state(f->runtime);
  require(f->state != NULL);
  require(pthread_mutex_init(&f->mutex, NULL) == 0);
  require(pthread_cond_init(&f->changed, NULL) == 0);
  active = f;
  f->contexts[0].fixture = f->contexts[1].fixture = f;
  f->contexts[0].id = 0;
  f->contexts[1].id = 1;
  luthr_lua_runtime_set_context(f->runtime, &f->contexts[0]);
  luthr_lua_register(f->state, "block", block);
  luthr_lua_register(f->state, "context_id", context_id);
  luthr_lua_register(f->state, "allocate_big", big_allocation);
  luthr_lua_register(f->state, "resume_raw", resume_raw);
  memset(&config, 0, sizeof(config));
  config.parallelism = 2;
  config.carrier_limit = 4;
  config.native_limit = 2;
  config.callback_limit = 2;
  config.quantum_instructions = 1024;
  require(luthr_lua_scheduler_new(f->state, automatic ? NULL : &config,
                                  &f->scheduler) == LUTHR_LUA_RUNTIME_OK);
}
static void finish(struct fixture *f) {
  require(luthr_lua_scheduler_free(f->scheduler) == LUTHR_LUA_RUNTIME_OK);
  luthr_lua_runtime_free(f->runtime);
  require(f->live == 0);
  require(pthread_cond_destroy(&f->changed) == 0);
  require(pthread_mutex_destroy(&f->mutex) == 0);
}
static void submit(struct fixture *f, unsigned i, const char *source,
                   const luthr_lua_task_options *options) {
  require(luthr_lua_task_submit_buffer(
              f->scheduler, (const unsigned char *)source, strlen(source),
              "=public-task", options, &f->tasks[i]) == LUTHR_LUA_RUNTIME_OK);
}
static luthr_lua_task_result await(struct fixture *f, unsigned i,
                                   luthr_lua_runtime_status expected) {
  luthr_lua_task_result result;
  require(luthr_lua_task_wait(f->tasks[i], 5000, &result) ==
          LUTHR_LUA_RUNTIME_OK);
  if (result.status != expected)
    fprintf(stderr, "task outcome %d, expected %d: %s\n", (int)result.status,
            (int)expected, luthr_lua_task_error(f->tasks[i]));
  require(result.status == expected);
  return result;
}
static void discard(struct fixture *f, unsigned i) {
  luthr_lua_task_result result;
  require(luthr_lua_task_collect(f->tasks[i], NULL, &result) ==
          LUTHR_LUA_RUNTIME_OK);
  require(luthr_lua_task_free(f->tasks[i]) == LUTHR_LUA_RUNTIME_OK);
  f->tasks[i] = NULL;
}
static void release_block(struct fixture *f) {
  lock(f);
  f->release = 1;
  signal_change(f);
  unlock(f);
}
static void entered(struct fixture *f) {
  lock(f);
  while (f->entered == 0)
    change(f);
  unlock(f);
}

static void results(struct fixture *f) {
  luthr_lua_task_result result;
  struct pollfd descriptor;
  const void *identity;
  init(f, 0);
  execute(f, "shared={value=42}; arg='host argument'");
  luthr_lua_getglobal(f->state, "shared");
  identity = luthr_lua_topointer(f->state, -1);
  luthr_lua_pop(f->state, 1);
  submit(f, 0,
         "assert(arg=='host argument'); return shared, function() return "
         "shared end, 42",
         NULL);
  result = await(f, 0, LUTHR_LUA_RUNTIME_OK);
  require(result.result_count == 3);
  descriptor.fd = luthr_lua_scheduler_fd(f->scheduler);
  descriptor.events = POLLIN;
  descriptor.revents = 0;
  require(poll(&descriptor, 1, 1000) == 1 && (descriptor.revents & POLLIN));
  require(luthr_lua_scheduler_drain(f->scheduler) == LUTHR_LUA_RUNTIME_OK);
  require(luthr_lua_task_free(f->tasks[0]) == LUTHR_LUA_RUNTIME_BUSY);
  require(luthr_lua_scheduler_free(f->scheduler) == LUTHR_LUA_RUNTIME_BUSY);
  require(luthr_lua_task_collect(f->tasks[0], f->state, &result) ==
          LUTHR_LUA_RUNTIME_OK);
  require(luthr_lua_topointer(f->state, -3) == identity &&
          integer(f->state, -1) == 42);
  luthr_lua_pop(f->state, 1);
  require(luthr_lua_pcall(f->state, 0, 1, 0) == LUTHR_LUA_OK);
  require(luthr_lua_topointer(f->state, -1) == identity);
  luthr_lua_settop(f->state, 0);
  require(luthr_lua_task_collect(f->tasks[0], NULL, &result) ==
          LUTHR_LUA_RUNTIME_ERR_STATE);
  require(luthr_lua_task_free(f->tasks[0]) == LUTHR_LUA_RUNTIME_OK);
  finish(f);
}

static void prepared(struct fixture *f) {
  luthr_lua_state *child;
  luthr_lua_task_result result;
  luthr_lua_scheduler_stats stats;
  init(f, 0);
  execute(f, "shared={}");
  child = luthr_lua_newthread(f->state);
  require(luthr_lua_l_loadstring(child, "local n,t=...; t.value=n; return t") ==
          LUTHR_LUA_OK);
  push_integer(child, 42);
  luthr_lua_getglobal(f->state, "shared");
  luthr_lua_xmove(f->state, child, 1);
  require(luthr_lua_task_submit(f->scheduler, child, 2, NULL, &f->tasks[0]) ==
          LUTHR_LUA_RUNTIME_OK);
  luthr_lua_pop(f->state, 1);
  (void)await(f, 0, LUTHR_LUA_RUNTIME_OK);
  require(luthr_lua_scheduler_snapshot(f->scheduler, &stats) ==
          LUTHR_LUA_RUNTIME_OK);
  require(stats.finished == 1 && stats.tasks == 1);
  require(luthr_lua_task_collect(f->tasks[0], f->state, &result) ==
          LUTHR_LUA_RUNTIME_OK);
  luthr_lua_getfield(f->state, -1, "value");
  require(integer(f->state, -1) == 42);
  luthr_lua_settop(f->state, 0);
  require(luthr_lua_task_free(f->tasks[0]) == LUTHR_LUA_RUNTIME_OK);
  finish(f);
}

static void budget(struct fixture *f, int raw) {
  luthr_lua_task_options options[2];
  luthr_lua_task_result result;
  init(f, 0);
  memset(options, 0, sizeof(options));
  options[0].instruction_limit = 800;
  options[0].context = &f->contexts[0];
  options[1].context = &f->contexts[1];
  options[0].trace = options[1].trace = trace;
  options[0].trace_context = &f->contexts[0];
  options[1].trace_context = &f->contexts[1];
  if (raw) {
    execute(f, "old_child=coroutine.create(function() while true do end end)");
    submit(f, 0, "context_id(0); return resume_raw(old_child)", &options[0]);
  } else
    submit(f, 0,
           "context_id(0); local c=coroutine.create(function() return "
           "pcall(function() while true do end end) end); return "
           "coroutine.resume(c)",
           &options[0]);
  submit(f, 1, "context_id(1); local n=0; for i=1,2000 do n=n+i end; return n",
         &options[1]);
  result = await(f, 0, LUTHR_LUA_RUNTIME_ERR_LIMIT);
  require(result.lua_status == LUTHR_LUA_ERRRUN &&
          strstr(luthr_lua_task_error(f->tasks[0]), "instruction limit") !=
              NULL);
  (void)await(f, 1, LUTHR_LUA_RUNTIME_OK);
  require(f->contexts[0].calls == 1 && f->contexts[1].calls == 1);
  require(f->contexts[0].traces != 0 && f->contexts[1].traces > 10);
  discard(f, 0);
  discard(f, 1);
  finish(f);
}

static void traces(struct fixture *f, int blocked) {
  luthr_lua_task_options options[2];
  luthr_lua_task_result result;
  init(f, 0);
  memset(options, 0, sizeof(options));
  options[0].trace = options[1].trace = trace;
  options[0].trace_context = &f->contexts[0];
  options[1].trace_context = &f->contexts[1];
  f->contexts[0].replace = !blocked;
  f->contexts[0].block = blocked;
  submit(f, 0, "local n=0; for i=1,100 do n=n+i end; return n", &options[0]);
  if (blocked)
    entered(f);
  submit(f, 1, "local n=0; for i=1,100 do n=n+i end; return n", &options[1]);
  (void)await(f, 1, LUTHR_LUA_RUNTIME_OK);
  if (blocked) {
    require(luthr_lua_task_poll(f->tasks[0], &result) ==
            LUTHR_LUA_RUNTIME_PENDING);
    release_block(f);
  }
  (void)await(f, 0, LUTHR_LUA_RUNTIME_OK);
  require(f->contexts[1].traces > 10);
  if (!blocked)
    require(f->contexts[0].traces == 1);
  discard(f, 0);
  discard(f, 1);
  finish(f);
}

static void defaults(struct fixture *f) {
  init(f, 0);
  require(luthr_lua_runtime_set_instruction_limit(f->runtime, 800) ==
          LUTHR_LUA_RUNTIME_OK);
  submit(f, 0, "block(); context_id(0); while true do end", NULL);
  entered(f);
  require(luthr_lua_runtime_clear_instruction_limit(f->runtime) ==
          LUTHR_LUA_RUNTIME_OK);
  luthr_lua_runtime_set_context(f->runtime, &f->contexts[1]);
  submit(f, 1, "return context_id(1)", NULL);
  (void)await(f, 1, LUTHR_LUA_RUNTIME_OK);
  release_block(f);
  (void)await(f, 0, LUTHR_LUA_RUNTIME_ERR_LIMIT);
  require(strcmp(luthr_lua_runtime_error(f->runtime), "") == 0);
  discard(f, 0);
  discard(f, 1);
  finish(f);
}

static void shutdown_test(struct fixture *f) {
  luthr_lua_task_result result;
  init(f, 0);
  submit(f, 0, "return block()", NULL);
  entered(f);
  result.lua_status = -123;
  result.result_count = -123;
  require(luthr_lua_task_wait(f->tasks[0], 1, &result) ==
          LUTHR_LUA_RUNTIME_PENDING);
  require(result.lua_status == -123 && result.result_count == -123);
  require(luthr_lua_scheduler_shutdown(f->scheduler) ==
          LUTHR_LUA_RUNTIME_PENDING);
  luthr_lua_runtime_free(f->runtime); /* live scheduler keeps the VM owned */
  require(luthr_lua_runtime_state(f->runtime) == f->state);
  release_block(f);
  (void)await(f, 0, LUTHR_LUA_RUNTIME_CANCELLED);
  discard(f, 0);
  require(luthr_lua_scheduler_shutdown(f->scheduler) == LUTHR_LUA_RUNTIME_OK);
  finish(f);
}
static void nested_test(struct fixture *f) {
  luthr_lua_task_options options;
  init(f, 0);
  luthr_lua_register(f->state, "nested_facade", nested_facade);
  luthr_lua_runtime_set_context(f->runtime, &f->contexts[1]);
  require(luthr_lua_runtime_set_instruction_limit(f->runtime, 200) ==
          LUTHR_LUA_RUNTIME_OK);
  memset(&options, 0, sizeof(options));
  options.context = &f->contexts[0];
  options.instruction_limit = 800;
  submit(f, 0, "local value=nested_facade(); context_id(0); return value",
         &options);
  entered(f); /* submission has retired its preparation stack before reentry */
  release_block(f);
  (void)await(f, 0, LUTHR_LUA_RUNTIME_OK);
  require(f->contexts[0].calls == 1 && f->contexts[1].calls == 1);
  discard(f, 0);
  finish(f);
}

static void marshal(luthr_lua_state *L, void *data) {
  struct invocation *call = data;
  push_integer(L, (unsigned)call->argument);
  luthr_lua_call(L, 1, 1);
  call->value = (int)integer(L, -1);
}
static void *provider(void *data) {
  struct invocation *call = data;
  call->status = luthr_lua_callback_call(call->fixture->callback, marshal, call,
                                         &call->lua_status, call->error,
                                         sizeof(call->error));
  return NULL;
}
static int recurse(luthr_lua_state *L) {
  struct invocation call;
  memset(&call, 0, sizeof(call));
  call.fixture = active;
  call.argument = (int)integer(L, 1);
  require(luthr_lua_callback_call(active->callback, marshal, &call,
                                  &call.lua_status, call.error,
                                  sizeof(call.error)) == LUTHR_LUA_RUNTIME_OK);
  push_integer(L, (unsigned)call.value);
  return 1;
}
static int pipe_new(luthr_lua_state *L) {
  luthr_lua_pipe_new(L, 1);
  return 1;
}
static void callbacks(struct fixture *f, int mode) {
  struct invocation call;
  luthr_lua_task_options options;
  pthread_t thread;
  init(f, 0);
  luthr_lua_register(f->state, "recurse", recurse);
  luthr_lua_register(f->state, "newpipe", pipe_new);
  execute(f, "event=newpipe()");
  memset(&options, 0, sizeof(options));
  options.context = &f->contexts[1];
  options.instruction_limit = mode == 1 ? 800 : 0;
  require(luthr_lua_l_loadstring(
              f->state, mode == 4   ? "allocate_big(); return 42"
                        : mode == 1 ? "context_id(1); while true do end"
                        : mode == 2
                            ? "return event:recv()"
                            : "local n=...; context_id(1); if n>0 then return "
                              "recurse(n-1)+1 end; return 40") == LUTHR_LUA_OK);
  require(luthr_lua_callback_new(f->scheduler, f->state, 1, &options,
                                 &f->callback) == LUTHR_LUA_RUNTIME_OK);
  luthr_lua_pop(f->state, 1);
  memset(&call, 0, sizeof(call));
  call.fixture = f;
  call.argument = 2;
  if (mode == 4) {
    f->fail_size = 16000;
    f->failing = 2;
  }
  require(pthread_create(&thread, NULL, provider, &call) == 0);
  if (mode == 2) {
    do {
      execute(f, "receiving=event:stats().waiting_receivers");
      luthr_lua_getglobal(f->state, "receiving");
      mode = integer(f->state, -1) == 0 ? 2 : 3;
      luthr_lua_pop(f->state, 1);
      if (mode == 2)
        sched_yield();
    } while (mode == 2);
    require(luthr_lua_callback_free(f->callback) == LUTHR_LUA_RUNTIME_BUSY);
    require(luthr_lua_scheduler_shutdown(f->scheduler) ==
            LUTHR_LUA_RUNTIME_PENDING);
  }
  require(pthread_join(thread, NULL) == 0);
  require(call.status == (mode == 4   ? LUTHR_LUA_RUNTIME_ERR_ALLOC
                          : mode == 1 ? LUTHR_LUA_RUNTIME_ERR_LIMIT
                          : mode == 3 ? LUTHR_LUA_RUNTIME_CANCELLED
                                      : LUTHR_LUA_RUNTIME_OK));
  if (mode == 0)
    require(call.value == 42 && f->contexts[1].calls == 3);
  require(luthr_lua_callback_revoke(f->callback) == LUTHR_LUA_RUNTIME_OK);
  require(luthr_lua_callback_free(f->callback) == LUTHR_LUA_RUNTIME_OK);
  finish(f);
}

static void pin_test(struct fixture *f, int cancel) {
  luthr_lua_task_result result;
  unsigned receiving;
  init(f, 0);
  luthr_lua_register(f->state, "pinned", pinned);
  luthr_lua_register(f->state, "newpipe", pipe_new);
  execute(f, "event=newpipe()");
  submit(f, 0, "return pinned(function() return tonumber(event:recv()) end)",
         NULL);
  do {
    execute(f, "receiving=event:stats().waiting_receivers");
    luthr_lua_getglobal(f->state, "receiving");
    receiving = integer(f->state, -1);
    luthr_lua_pop(f->state, 1);
    if (!receiving)
      sched_yield();
  } while (!receiving);
  execute(f, "collectgarbage()");
  submit(f, 1, "return 42", NULL);
  (void)await(f, 1, LUTHR_LUA_RUNTIME_OK);
  discard(f, 1);
  if (cancel)
    require(luthr_lua_task_cancel(f->tasks[0]) == LUTHR_LUA_RUNTIME_OK);
  else
    execute(f, "event:send('42')");
  (void)await(f, 0,
              cancel ? LUTHR_LUA_RUNTIME_CANCELLED : LUTHR_LUA_RUNTIME_OK);
  require(luthr_lua_task_collect(f->tasks[0], f->state, &result) ==
          LUTHR_LUA_RUNTIME_OK);
  if (!cancel)
    require(integer(f->state, -1) == 42);
  luthr_lua_settop(f->state, 0);
  require(luthr_lua_task_free(f->tasks[0]) == LUTHR_LUA_RUNTIME_OK);
  finish(f);
}

static void allocation_failure(struct fixture *f, int collecting) {
  luthr_lua_task_result result;
  init(f, 0);
  if (collecting) {
    submit(f, 0,
           "local t={}; for i=1,5000 do t[i]=i end; return table.unpack(t)",
           NULL);
    (void)await(f, 0, LUTHR_LUA_RUNTIME_OK);
  }
  f->fail_size = 16000;
  f->failing = collecting ? 1 : 2;
  if (collecting) {
    result.lua_status = -123;
    require(luthr_lua_task_collect(f->tasks[0], f->state, &result) ==
            LUTHR_LUA_RUNTIME_ERR_ALLOC);
    require(result.lua_status == -123 && f->failed == 1);
    f->failing = 0;
    require(luthr_lua_task_collect(f->tasks[0], f->state, &result) ==
            LUTHR_LUA_RUNTIME_OK);
    require(result.result_count == 5000 && integer(f->state, 1) == 1 &&
            integer(f->state, -1) == 5000);
    luthr_lua_settop(f->state, 0);
    require(luthr_lua_task_free(f->tasks[0]) == LUTHR_LUA_RUNTIME_OK);
  } else {
    submit(f, 0, "allocate_big(); return 42", NULL);
    result = await(f, 0, LUTHR_LUA_RUNTIME_ERR_ALLOC);
    require(result.lua_status == LUTHR_LUA_ERRMEM && f->failed == 2);
    discard(f, 0);
  }
  finish(f);
}
static void automatic(struct fixture *f) {
  luthr_lua_scheduler_stats stats;
  init(f, 1);
  require(luthr_lua_scheduler_snapshot(f->scheduler, &stats) ==
          LUTHR_LUA_RUNTIME_OK);
  require(stats.parallelism != 0 && stats.parallelism <= stats.affinity_cpus);
  require(stats.carrier_limit >= stats.parallelism + stats.native_limit);
  if (stats.quota_milli_cpus != 0)
    require(stats.parallelism <= (stats.quota_milli_cpus + 999) / 1000);
  finish(f);
}
static void load_error(struct fixture *f) {
  init(f, 0);
  require(luthr_lua_task_submit_buffer(
              f->scheduler, (const unsigned char *)"local =", 7, "=bad-task",
              NULL, &f->tasks[0]) == LUTHR_LUA_RUNTIME_ERR_LOAD);
  require(f->tasks[0] == NULL &&
          luthr_lua_type(f->state, -1) == LUTHR_LUA_TSTRING);
  luthr_lua_pop(f->state, 1);
  submit(f, 0, "return 42", NULL);
  (void)await(f, 0, LUTHR_LUA_RUNTIME_OK);
  discard(f, 0);
  finish(f);
}
static void *raw_allocate(void *context, void *old, size_t old_size,
                          size_t size) {
  void *block;
  if (size == 0) {
    if (old != NULL)
      deallocate(context, old, old_size);
    return NULL;
  }
  block = allocate(context, size);
  if (block != NULL && old != NULL) {
    memcpy(block, old, old_size < size ? old_size : size);
    deallocate(context, old, old_size);
  }
  return block;
}
static int raw_context(luthr_lua_state *L) {
  require(luthr_lua_task_context(L) == active);
  push_integer(L, 42);
  return 1;
}
static void raw_vm(struct fixture *f) {
  luthr_lua_scheduler_config config;
  luthr_lua_task_options options;
  memset(&config, 0, sizeof(config));
  config.parallelism = 1;
  config.native_limit = 1;
  config.carrier_limit = 2;
  memset(&options, 0, sizeof(options));
  options.context = f;
  options.instruction_limit = 1000;
  active = f;
  f->state = luthr_lua_newstate(raw_allocate, f, 42);
  require(f->state != NULL);
  luthr_lua_l_openlibs(f->state);
  luthr_lua_register(f->state, "raw_context", raw_context);
  require(luthr_lua_scheduler_new(f->state, &config, &f->scheduler) ==
          LUTHR_LUA_RUNTIME_OK);
  submit(f, 0,
         "debug.sethook(nil); assert(raw_context()==42); return "
         "pcall(function() while true do end end)",
         &options);
  (void)await(f, 0, LUTHR_LUA_RUNTIME_ERR_LIMIT);
  discard(f, 0);
  require(luthr_lua_scheduler_free(f->scheduler) == LUTHR_LUA_RUNTIME_OK);
  luthr_lua_close(f->state);
  require(f->live == 0);
}
int main(int argc, char **argv) {
  struct fixture *f = calloc(1, sizeof(*f));
  require(argc == 2 && f != NULL);
  if (strcmp(argv[1], "results") == 0)
    results(f);
  else if (strcmp(argv[1], "prepared") == 0)
    prepared(f);
  else if (strcmp(argv[1], "budget") == 0)
    budget(f, 0);
  else if (strcmp(argv[1], "raw_policy") == 0)
    budget(f, 1);
  else if (strcmp(argv[1], "traces") == 0)
    traces(f, 0);
  else if (strcmp(argv[1], "blocked_trace") == 0)
    traces(f, 1);
  else if (strcmp(argv[1], "defaults") == 0)
    defaults(f);
  else if (strcmp(argv[1], "nested") == 0)
    nested_test(f);
  else if (strcmp(argv[1], "shutdown") == 0)
    shutdown_test(f);
  else if (strcmp(argv[1], "callback") == 0)
    callbacks(f, 0);
  else if (strcmp(argv[1], "callback_budget") == 0)
    callbacks(f, 1);
  else if (strcmp(argv[1], "callback_shutdown") == 0)
    callbacks(f, 2);
  else if (strcmp(argv[1], "callback_oom") == 0)
    callbacks(f, 4);
  else if (strcmp(argv[1], "pin") == 0)
    pin_test(f, 0);
  else if (strcmp(argv[1], "pin_cancel") == 0)
    pin_test(f, 1);
  else if (strcmp(argv[1], "oom") == 0)
    allocation_failure(f, 0);
  else if (strcmp(argv[1], "collect_oom") == 0)
    allocation_failure(f, 1);
  else if (strcmp(argv[1], "automatic") == 0)
    automatic(f);
  else if (strcmp(argv[1], "load_error") == 0)
    load_error(f);
  else if (strcmp(argv[1], "raw_vm") == 0)
    raw_vm(f);
  else
    abort();
  free(f);
  return 0;
}
