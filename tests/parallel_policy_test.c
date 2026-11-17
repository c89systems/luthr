#define _GNU_SOURCE
#include <lauxlib.h>
#include <lgc.h>
#include <lstate.h>
#include <lua.h>
#include <lualib.h>
#include <luthr/lua_runtime.h>
#include <luthr_hook_policy.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum mode {
  BUDGET,
  BUDGET_TRACE,
  TRACE_REPLACE,
  TRACE_BLOCKED,
  WRAPPERS,
  CAP,
  WARNINGS,
  EXTERNAL_FAILURE
};
struct fixture;
struct actor {
  struct fixture *fixture;
  lua_State *state;
  int polled;
  int status;
  int results;
};
struct observer {
  struct fixture *fixture;
  unsigned kind;
  unsigned calls;
  unsigned worker_calls;
};
struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_barrier_t managed;
  pthread_barrier_t ready;
  luthr_lua_runtime *runtime;
  lua_Hook hook;
  struct actor actors[2];
  struct observer observers[2];
  enum mode mode;
  unsigned accepted;
  unsigned managed_waiting;
  unsigned ticks[2];
  unsigned warning_calls[2];
  int measured;
  int launched;
  int trace_busy;
  int entered;
  int release;
  int rejected;
  int swapped;
  int driver_calls;
  int allocator_active;
  unsigned allocations;
  size_t live_bytes;
  size_t peak_bytes;
  lua_State *child;
  int child_done;
  pthread_t failing_carrier;
  unsigned failures_left;
  unsigned rejected_headers;
  unsigned external_releases;
  char *external;
  lua_State *external_state;
};
static struct fixture *active_fixture;

static void check(int status) {
  if (status != 0)
    abort();
}

void __real_lua_sethook(lua_State *, lua_Hook, int, int);

static void counted_hook(lua_State *state, lua_Debug *record) {
  struct fixture *fixture = active_fixture;
  fixture->hook(state, record);
  if (fixture->measured && record->event == LUA_HOOKCOUNT) {
    check(pthread_mutex_lock(&fixture->mutex));
    fixture->accepted++;
    check(pthread_mutex_unlock(&fixture->mutex));
  }
}

void __wrap_lua_sethook(lua_State *state, lua_Hook hook, int mask, int count) {
  struct fixture *fixture = active_fixture;
  if (fixture != NULL && hook != NULL && hook != counted_hook &&
      (mask & LUTHR_LUA_RUNTIME_MASK_FINALIZERS)) {
    if (fixture->hook == NULL)
      fixture->hook = hook; /* first publication precedes worker creation */
    if (fixture->hook != hook || count < 0)
      abort();
    hook = counted_hook;
  }
  __real_lua_sethook(state, hook, mask, count);
}

static void *allocate(void *user, size_t size) {
  struct fixture *fixture = user;
  void *pointer;
  if (fixture->allocator_active)
    abort();
  fixture->allocator_active = 1;
  fixture->allocations++;
  if (fixture->mode == EXTERNAL_FAILURE) {
    int reject;
    check(pthread_mutex_lock(&fixture->mutex));
    reject = fixture->failures_left != 0 &&
             pthread_equal(fixture->failing_carrier, pthread_self());
    if (reject) {
      fixture->rejected_headers++;
    }
    check(pthread_mutex_unlock(&fixture->mutex));
    if (reject) {
      fixture->allocator_active = 0;
      return NULL;
    }
  }
  pointer = malloc(size);
  if (pointer != NULL) {
    fixture->live_bytes += size;
    if (fixture->live_bytes > fixture->peak_bytes)
      fixture->peak_bytes = fixture->live_bytes;
  }
  fixture->allocator_active = 0;
  return pointer;
}

static void release(void *user, void *pointer, size_t size) {
  struct fixture *fixture = user;
  if (fixture->allocator_active)
    abort();
  fixture->allocator_active = 1;
  fixture->live_bytes -= size;
  free(pointer);
  fixture->allocator_active = 0;
}

static void *resize(void *user, void *pointer, size_t old_size, size_t size) {
  struct fixture *fixture = user;
  void *replacement;
  if (fixture->allocator_active)
    abort();
  fixture->allocator_active = 1;
  fixture->allocations++;
  replacement = realloc(pointer, size);
  if (replacement != NULL) {
    fixture->live_bytes = fixture->live_bytes - old_size + size;
    if (fixture->live_bytes > fixture->peak_bytes)
      fixture->peak_bytes = fixture->live_bytes;
  }
  fixture->allocator_active = 0;
  return replacement;
}

static struct fixture *from_state(lua_State *state) {
  struct fixture *fixture = luthr_lua_runtime_context_from_state(state);
  if (fixture != active_fixture)
    abort();
  return fixture;
}

static void trace_b(void *, const luthr_lua_runtime_trace_event *);

static void trace(void *user, const luthr_lua_runtime_trace_event *event,
                  unsigned kind) {
  struct observer *observer = user;
  struct fixture *fixture = observer->fixture;
  int worker = event->source_size == strlen("@worker-a.lua") &&
               memcmp(event->source, "@worker-a.lua", event->source_size) == 0;
  if (observer->kind != kind || event->source == NULL || event->line <= 0 ||
      fixture->trace_busy)
    abort();
  fixture->trace_busy = 1; /* the runtime serializes this legacy observer */
  observer->calls++;
  if (fixture->launched && worker) {
    observer->worker_calls++;
    if (luthr_lua_runtime_context(fixture->runtime) != fixture ||
        luthr_lua_runtime_set_instruction_limit(fixture->runtime, 1) !=
            LUTHR_LUA_RUNTIME_ERR_ARG)
      abort();
    if (fixture->mode == TRACE_BLOCKED && observer->worker_calls == 1) {
      check(pthread_mutex_lock(&fixture->mutex));
      fixture->entered = 1;
      check(pthread_cond_broadcast(&fixture->changed));
      while (!fixture->release)
        check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
      check(pthread_mutex_unlock(&fixture->mutex));
    } else if (fixture->mode == TRACE_REPLACE ||
               fixture->mode == BUDGET_TRACE) {
      if (kind == 0) {
        if (luthr_lua_runtime_set_trace_callback(fixture->runtime, trace_b,
                                                 &fixture->observers[1]) !=
            LUTHR_LUA_RUNTIME_OK)
          abort();
      } else if (observer->worker_calls == 20) {
        if (luthr_lua_runtime_set_trace_callback(fixture->runtime, NULL,
                                                 NULL) != LUTHR_LUA_RUNTIME_OK)
          abort();
      }
    }
  }
  fixture->trace_busy = 0;
}

static void trace_a(void *user, const luthr_lua_runtime_trace_event *event) {
  trace(user, event, 0);
}

static void trace_b(void *user, const luthr_lua_runtime_trace_event *event) {
  trace(user, event, 1);
}

static void warning(void *user, const char *message, int continuation,
                    unsigned kind) {
  struct observer *context = user;
  struct fixture *fixture = context->fixture;
  if (context->kind != kind || continuation != 0 || strcmp(message, "paired"))
    abort();
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->warning_calls[kind]++;
  check(pthread_mutex_unlock(&fixture->mutex));
}

static void warning_a(void *user, const char *message, int continuation) {
  warning(user, message, continuation, 0);
}

static void warning_b(void *user, const char *message, int continuation) {
  warning(user, message, continuation, 1);
}

static int tick(lua_State *state) {
  struct fixture *fixture = from_state(state);
  int role = (int)lua_tointeger(state, 1) - 1;
  if (role < 0 || role > 1)
    abort();
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->ticks[role]++;
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static int ready(lua_State *state) {
  struct fixture *fixture = from_state(state);
  int status;
  (void)tick(state);
  status = pthread_barrier_wait(&fixture->ready);
  if (status != 0 && status != PTHREAD_BARRIER_SERIAL_THREAD)
    abort();
  return 0;
}

static int progress(lua_State *state) {
  struct fixture *fixture = from_state(state);
  check(pthread_mutex_lock(&fixture->mutex));
  while (!fixture->entered)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  check(pthread_mutex_unlock(&fixture->mutex));
  if (luthr_lua_runtime_set_trace_callback(fixture->runtime, trace_b,
                                           &fixture->observers[1]) !=
      LUTHR_LUA_RUNTIME_ERR_ARG)
    abort();
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->rejected = 1;
  fixture->release = 1;
  check(pthread_cond_broadcast(&fixture->changed));
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static int await_observer(lua_State *state) {
  struct fixture *fixture = from_state(state);
  check(pthread_mutex_lock(&fixture->mutex));
  while (!fixture->entered)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static int set_warning(lua_State *state) {
  struct fixture *fixture = from_state(state);
  int kind = (int)lua_tointeger(state, 1) % 2;
  luthr_lua_runtime_set_warning_callback(fixture->runtime,
                                         kind ? warning_b : warning_a,
                                         &fixture->observers[kind]);
  return 0;
}

static void *release_external(void *user, void *pointer, size_t old_size,
                              size_t size) {
  struct fixture *fixture = user;
  if (pointer != fixture->external || old_size != 128 || size != 0 ||
      luaW_scope(fixture->external_state).heap == 0)
    abort();
  fixture->external_releases++;
  free(pointer);
  fixture->external = NULL;
  return NULL;
}

static void *release_padding(void *user, void *pointer, size_t old_size,
                             size_t size) {
  (void)user;
  (void)pointer;
  (void)old_size;
  (void)size;
  return NULL;
}
static int external_padding(lua_State *state) {
  static const char bytes[128] = "retained external header";
  lua_pushexternalstring(state, bytes, 127, release_padding, NULL);
  return 1;
}
static int fail_external(lua_State *state) {
  struct fixture *fixture = from_state(state);
  int status = LUA_OK;
  unsigned attempts;
  if (!lua_checkstack(state, 1028))
    abort();
  fixture->external_state = state;
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->failing_carrier = pthread_self();
  fixture->failures_left = 2;
  check(pthread_mutex_unlock(&fixture->mutex));
  /* Keep headers alive until refusing backing allocations actually exhausts
   * local storage, then test cleanup of the caller-owned external buffer. */
  for (attempts = 0; attempts < 1024 && status == LUA_OK; ++attempts) {
    lua_pushcfunction(state, external_padding);
    status = lua_pcall(state, 0, 1, 0);
  }
  if (status != LUA_ERRMEM)
    abort();
  lua_pop(state, 1);
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->failures_left = 2;
  fixture->rejected_headers = 0;
  check(pthread_mutex_unlock(&fixture->mutex));
  (void)lua_pushexternalstring(state, fixture->external, 127, release_external,
                               fixture);
  abort();
}

static int publish_child(lua_State *state) {
  struct fixture *fixture = from_state(state);
  lua_State *child = lua_tothread(state, 1);
  if (child == NULL)
    abort();
  lua_pushvalue(state, 1);
  lua_setglobal(state, "fixture_child"); /* retain the public handle */
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->child = child;
  check(pthread_cond_broadcast(&fixture->changed));
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static int child_done(lua_State *state) {
  struct fixture *fixture = from_state(state);
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->child_done = 1;
  check(pthread_cond_broadcast(&fixture->changed));
  while (!fixture->swapped)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static int read_child(lua_State *state) {
  struct fixture *fixture = from_state(state);
  lua_State *child;
  unsigned reads = 0;
  int done;
  check(pthread_mutex_lock(&fixture->mutex));
  while (fixture->child == NULL)
    check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
  child = fixture->child;
  check(pthread_mutex_unlock(&fixture->mutex));
  do {
    if (luthr_lua_runtime_context_from_state(child) != fixture)
      abort();
    reads++;
    check(pthread_mutex_lock(&fixture->mutex));
    done = fixture->child_done;
    check(pthread_mutex_unlock(&fixture->mutex));
  } while (!done || reads < 300);
  check(pthread_mutex_lock(&fixture->mutex));
  fixture->swapped = 1;
  check(pthread_cond_broadcast(&fixture->changed));
  check(pthread_mutex_unlock(&fixture->mutex));
  return 0;
}

static void managed_poll(lua_State *state, void *user) {
  struct actor *actor = user;
  if (!actor->polled) {
    int status;
    LuaWorldScope scope;
    actor->polled = 1;
    /* A native barrier must withdraw managed participation. Otherwise a peer
     * requesting GC before its first poll waits for this blocked participant.
     */
    scope = luaW_detach(state);
    if (actor->fixture->mode == CAP) {
      struct fixture *fixture = actor->fixture;
      check(pthread_mutex_lock(&fixture->mutex));
      if (actor == &fixture->actors[0]) {
        fixture->managed_waiting = 1;
        check(pthread_cond_broadcast(&fixture->changed));
      } else {
        while (!fixture->managed_waiting)
          check(pthread_cond_wait(&fixture->changed, &fixture->mutex));
      }
      check(pthread_mutex_unlock(&fixture->mutex));
      if (actor == &fixture->actors[1]) {
        luaW_attach(state, scope);
        luaC_fullgc(state, 0);
        scope = luaW_detach(state);
      }
    }
    status = pthread_barrier_wait(&actor->fixture->managed);
    if (status != 0 && status != PTHREAD_BARRIER_SERIAL_THREAD)
      abort();
    luaW_attach(state, scope);
  }
}

static void *run_actor(void *user) {
  struct actor *actor = user;
  actor->status = lua_resume(actor->state, NULL, 0, &actor->results);
  return NULL;
}

static int run(lua_State *state) {
  struct fixture *fixture = from_state(state);
  pthread_t carriers[2];
  int i;
  fixture->driver_calls++;
  for (i = 0; i < 2; ++i) {
    struct actor *actor = &fixture->actors[i];
    int results;
    actor->fixture = fixture;
    actor->state = lua_newthread(state);
    lua_pushvalue(state, i + 1);
    lua_xmove(state, actor->state, 1);
    if (lua_resume(actor->state, state, 0, &results) != LUA_YIELD ||
        results != 0)
      abort();
    actor->state->state_poll = managed_poll;
    actor->state->state_poll_context = actor;
    actor->state->poll_remaining = 1;
  }
  fixture->launched = 1;
  for (i = 0; i < 2; ++i)
    check(pthread_create(&carriers[i], NULL, run_actor, &fixture->actors[i]));
  for (i = 0; i < 2; ++i)
    check(pthread_join(carriers[i], NULL));
  if (fixture->mode == CAP) {
    lua_State *failed = fixture->actors[0].state;
    lua_xmove(failed, state, 1);
    return lua_error(state);
  }
  return 0;
}

static int open_fixture(void *opaque) {
  static const luaL_Reg functions[] = {{"run", run},
                                       {"ready", ready},
                                       {"tick", tick},
                                       {"progress", progress},
                                       {"await_observer", await_observer},
                                       {"set_warning", set_warning},
                                       {"publish_child", publish_child},
                                       {"child_done", child_done},
                                       {"read_child", read_child},
                                       {"fail_external", fail_external},
                                       {NULL, NULL}};
  lua_State *state = opaque;
  (void)from_state(state);
  luaL_newlib(state, functions);
  return 1;
}

static const char *actor_source(struct fixture *fixture, int role) {
  if (fixture->mode == BUDGET || fixture->mode == BUDGET_TRACE)
    return role == 0 ? "coroutine.yield(); fixture.ready(1); "
                       "while true do fixture.tick(1) end"
                     : "coroutine.yield(); fixture.ready(2); "
                       "while true do fixture.tick(2) end";
  if (fixture->mode == CAP)
    return "coroutine.yield(); local keep={}; for i=1,80 do "
           "keep[i]=string.rep('x',8192)..tostring(i) end; return #keep";
  if (fixture->mode == WRAPPERS) {
    if (role == 0)
      return "coroutine.yield(); local co=coroutine.create(function() "
             "for i=1,300 do coroutine.yield(i) end; return true end); "
             "fixture.publish_child(co); for i=1,300 do "
             "local ok,v=coroutine.resume(co); assert(ok and v==i); "
             "local f=coroutine.wrap(function() return i end); "
             "assert(f()==i) end; assert(coroutine.close(co)); "
             "fixture.child_done(); return true";
    return "coroutine.yield(); for i=1,300 do "
           "local f=coroutine.wrap(function() return i end); "
           "assert(f()==i) end; fixture.read_child(); return true";
  }
  if (fixture->mode == WARNINGS)
    return role == 0 ? "coroutine.yield(); for i=1,600 do "
                       "fixture.set_warning(i) end; return true"
                     : "coroutine.yield(); for i=1,600 do warn('paired') end; "
                       "return true";
  if (fixture->mode == EXTERNAL_FAILURE && role == 0)
    return "coroutine.yield(); fixture.fail_external()";
  if (fixture->mode == TRACE_BLOCKED && role == 1)
    return "coroutine.yield(); fixture.await_observer(); local sum=0; "
           "for i=1,300 do local t={i}; sum=sum+t[1]; "
           "if i%31==0 then collectgarbage('collect') end end; "
           "assert(sum==45150); fixture.progress(); return true";
  return "coroutine.yield(); local sum=0\n"
         "for i=1,100 do local t={i}; sum=sum+t[1] end\n"
         "assert(sum==5050); return true";
}

int main(int argc, char **argv) {
  struct fixture fixture;
  luthr_lua_runtime_allocator_config allocator;
  luthr_lua_runtime_status status;
  char *source;
  size_t capacity;
  const char *left, *right, *right_loader;
  int i, length, limited, failed = 0;
  if (argc != 2)
    return 2;
  memset(&fixture, 0, sizeof(fixture));
  if (strncmp(argv[1], "budget_", 7) == 0)
    fixture.mode = strcmp(argv[1], "budget_trace") == 0 ? BUDGET_TRACE : BUDGET;
  else if (strcmp(argv[1], "trace_replace") == 0)
    fixture.mode = TRACE_REPLACE;
  else if (strcmp(argv[1], "trace_blocked") == 0)
    fixture.mode = TRACE_BLOCKED;
  else if (strcmp(argv[1], "wrappers") == 0)
    fixture.mode = WRAPPERS;
  else if (strcmp(argv[1], "allocation_cap") == 0)
    fixture.mode = CAP;
  else if (strcmp(argv[1], "warning_pairs") == 0)
    fixture.mode = WARNINGS;
  else if (strcmp(argv[1], "external_failure") == 0)
    fixture.mode = EXTERNAL_FAILURE;
  else
    return 2;
  active_fixture = &fixture;
  if (fixture.mode == EXTERNAL_FAILURE) {
    fixture.external = malloc(128);
    if (fixture.external == NULL)
      abort();
    memset(fixture.external, 'e', 127);
    fixture.external[127] = '\0';
  }
  check(pthread_mutex_init(&fixture.mutex, NULL));
  check(pthread_cond_init(&fixture.changed, NULL));
  check(pthread_barrier_init(&fixture.managed, NULL, 2));
  check(pthread_barrier_init(&fixture.ready, NULL, 2));
  memset(&allocator, 0, sizeof(allocator));
  allocator.user = &fixture;
  allocator.alloc_fn = allocate;
  allocator.realloc_fn = resize;
  allocator.free_fn = release;
  allocator.max_bytes = fixture.mode == CAP ? 512 * 1024 : 0;
  if (luthr_lua_runtime_new_with_allocator(&fixture.runtime, &allocator) !=
          LUTHR_LUA_RUNTIME_OK ||
      luthr_lua_runtime_openlibs(fixture.runtime) != LUTHR_LUA_RUNTIME_OK)
    abort();
  luthr_lua_runtime_set_context(fixture.runtime, &fixture);
  if (luthr_lua_runtime_register_c_module(fixture.runtime, "fixture",
                                          open_fixture) != LUTHR_LUA_RUNTIME_OK)
    abort();
  if (strstr(argv[1], "generational") != NULL) {
    static const unsigned char gc[] = "collectgarbage('generational')";
    if (luthr_lua_runtime_run_buffer(fixture.runtime, gc, sizeof(gc) - 1,
                                     "gc-mode", 0, NULL,
                                     0) != LUTHR_LUA_RUNTIME_OK)
      abort();
  }
  for (i = 0; i < 2; ++i) {
    fixture.observers[i].fixture = &fixture;
    fixture.observers[i].kind = (unsigned)i;
  }
  luthr_lua_runtime_set_warning_callback(fixture.runtime, warning_a,
                                         &fixture.observers[0]);
  if (fixture.mode == BUDGET_TRACE || fixture.mode == TRACE_REPLACE ||
      fixture.mode == TRACE_BLOCKED)
    if (luthr_lua_runtime_set_trace_callback(fixture.runtime, trace_a,
                                             &fixture.observers[0]) !=
        LUTHR_LUA_RUNTIME_OK)
      abort();
  limited = fixture.mode == BUDGET || fixture.mode == BUDGET_TRACE;
  if (limited && luthr_lua_runtime_set_instruction_limit(
                     fixture.runtime, 4000) != LUTHR_LUA_RUNTIME_OK)
    abort();
  left = actor_source(&fixture, 0);
  right = actor_source(&fixture, 1);
  right_loader = fixture.mode == TRACE_BLOCKED
                     ? "assert(load(string.dump(assert(load([=[%s]=], "
                       "'@worker-b.lua')),true)))"
                     : "assert(load([=[%s]=], '@worker-b.lua'))";
  capacity = strlen(left) + strlen(right) + strlen(right_loader) + 256;
  source = malloc(capacity);
  if (source == NULL)
    abort();
  length = snprintf(source, capacity,
                    "fixture=require('fixture'); local a=assert(load([=[%s]=], "
                    "'@worker-a.lua')); "
                    "local b=",
                    left);
  if (length < 0 || (size_t)length >= capacity)
    abort();
  i = snprintf(source + length, capacity - (size_t)length, right_loader, right);
  if (i < 0 || (size_t)i >= capacity - (size_t)length)
    abort();
  length += i;
  i = snprintf(source + length, capacity - (size_t)length,
               "; fixture.run(a,b)");
  if (i < 0 || (size_t)i >= capacity - (size_t)length)
    abort();
  length += i;
  fixture.measured = 1;
  status =
      luthr_lua_runtime_run_buffer(fixture.runtime, (unsigned char *)source,
                                   (size_t)length, "@root.lua", 0, NULL, 0);
  fixture.measured = 0;
  free(source);
  failed |= fixture.driver_calls != 1;
  for (i = 0; i < 2; ++i) {
    int expected = limited ? LUA_ERRRUN : LUA_OK;
    if (fixture.mode == EXTERNAL_FAILURE && i == 0)
      expected = LUA_ERRMEM;
    failed |= !fixture.actors[i].polled;
    if (fixture.mode == CAP)
      failed |= fixture.actors[i].status != LUA_ERRMEM &&
                fixture.actors[i].status != LUA_ERRRUN;
    else
      failed |= fixture.actors[i].status != expected;
  }
  if (limited) {
    failed |= status != LUTHR_LUA_RUNTIME_ERR_LIMIT ||
              fixture.accepted != 3999 || fixture.ticks[0] == 0 ||
              fixture.ticks[1] == 0;
  } else if (fixture.mode == CAP)
    failed |= status != LUTHR_LUA_RUNTIME_ERR_ALLOC ||
              fixture.peak_bytes > allocator.max_bytes;
  else
    failed |= status != LUTHR_LUA_RUNTIME_OK;
  if (fixture.mode == TRACE_REPLACE || fixture.mode == BUDGET_TRACE)
    failed |= fixture.observers[0].worker_calls != 1 ||
              fixture.observers[1].worker_calls != 20;
  if (fixture.mode == TRACE_BLOCKED)
    failed |= !fixture.entered || !fixture.rejected;
  if (fixture.mode == WARNINGS)
    failed |= fixture.warning_calls[0] + fixture.warning_calls[1] != 600;
  if (fixture.mode == WRAPPERS)
    failed |= !fixture.child_done || !fixture.swapped;
  if (fixture.mode == EXTERNAL_FAILURE) {
    failed |= fixture.external != NULL || fixture.external_releases != 1 ||
              fixture.rejected_headers < 2 || fixture.failures_left != 2;
    fixture.failures_left = 0;
  }
  if (limited) {
    static const unsigned char recovery[] = "assert(2+2==4)";
    if (luthr_lua_runtime_run_buffer(fixture.runtime, recovery,
                                     sizeof(recovery) - 1, "@recovery.lua", 0,
                                     NULL, 0) != LUTHR_LUA_RUNTIME_OK)
      failed = 1;
  }
  luthr_lua_runtime_free(fixture.runtime);
  failed |= fixture.live_bytes != 0 || fixture.allocations == 0;
  check(pthread_barrier_destroy(&fixture.ready));
  check(pthread_barrier_destroy(&fixture.managed));
  check(pthread_cond_destroy(&fixture.changed));
  check(pthread_mutex_destroy(&fixture.mutex));
  if (failed)
    fprintf(stderr,
            "%s: status=%d workers=%d,%d accepted=%u traces=%u,%u peak=%zu "
            "bytes=%zu\n",
            argv[1], status, fixture.actors[0].status, fixture.actors[1].status,
            fixture.accepted, fixture.observers[0].worker_calls,
            fixture.observers[1].worker_calls, fixture.peak_bytes,
            fixture.live_bytes);
  else
    printf("parallel C89 policy %s: shared accounting, callbacks and cleanup\n",
           argv[1]);
  return failed;
}
