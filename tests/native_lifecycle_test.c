#define _GNU_SOURCE
#include <errno.h>
#include <lapi.h>
#include <lauxlib.h>
#include <lpipe.h>
#include <lscheduler.h>
#include <lualib.h>
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

struct fixture {
  lua_State *main;
  LuaScheduler scheduler;
  LuaScheduledTask tasks[6];
  LuaCallback callback;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  LuaWaiter *waiters;
  pthread_t pinned_carrier;
  unsigned entered, release[2], opened, closed, probes, cpu_on_reserved;
  unsigned callback_started, callback_release, callback_value, callback_parks;
  int callback_status, nested, marshalling_error;
  int opaque_marshalling;
  unsigned cpu_ready, ack_ready, ack_release, generated;
  size_t peak;
  char error[128];
  size_t live;
  unsigned fail_allocations;
};
static struct fixture *active;
static __thread unsigned native_marker;

static void lock(struct fixture *f) {
  require(pthread_mutex_lock(&f->mutex) == 0);
}
static void unlock(struct fixture *f) {
  require(pthread_mutex_unlock(&f->mutex) == 0);
}
static void notify(struct fixture *f) {
  require(pthread_cond_broadcast(&f->changed) == 0);
}
static void change(struct fixture *f) {
  require(pthread_cond_wait(&f->changed, &f->mutex) == 0);
}
static void *allocate(void *context, void *block, size_t old, size_t size) {
  struct fixture *f = context;
  void *replacement;
  if (block == NULL)
    old = 0;
  if (size == 0) {
    f->live -= old;
    free(block);
    return NULL;
  }
  if (size > 4096 && f->fail_allocations != 0) {
    f->fail_allocations--;
    return NULL;
  }
  replacement = realloc(block, size);
  if (replacement != NULL) {
    f->live = f->live - old + size;
    if (f->live > f->peak)
      f->peak = f->live;
  }
  return replacement;
}

static void init(struct fixture *f) {
  LuaSchedulerConfig config = {1, 3, 2, 1, 1};
  active = f;
  f->main = lua_newstate(allocate, f, 42);
  require(f->main != NULL);
  luaL_openlibs(f->main);
  require(pthread_mutex_init(&f->mutex, NULL) == 0);
  require(pthread_cond_init(&f->changed, NULL) == 0);
  require(luaSC_init(f->main, &f->scheduler, &config) == 0);
  luaP_newpipe(f->main, 1);
  lua_setglobal(f->main, "event");
}

static void execute(lua_State *L, const char *source) {
  int status = luaL_loadstring(L, source);
  if (status == LUA_OK)
    status = lua_pcall(L, 0, 0, 0);
  if (status != LUA_OK)
    fprintf(stderr, "host: %s\n", lua_tostring(L, -1));
  require(status == LUA_OK);
}
static void managed(struct fixture *f, const char *name,
                    lua_CFunction function) {
  luaQ_pushmanaged(f->main, function, 0);
  lua_setglobal(f->main, name);
}
static void foreign(struct fixture *f, const char *name,
                    lua_CFunction function) {
  lua_pushcfunction(f->main, function);
  lua_setglobal(f->main, name);
}
static void submit(struct fixture *f, unsigned i, const char *source, int arg) {
  lua_State *L = lua_newthread(f->main);
  require(luaL_loadstring(L, source) == LUA_OK);
  lua_pushinteger(L, arg);
  require(luaSC_submit(&f->scheduler, &f->tasks[i], L, 1) == LUA_OK);
  lua_pop(f->main, 1);
}
static lua_Integer collect(struct fixture *f, unsigned i, int expected) {
  int status, results;
  lua_Integer result;
  luaSC_wait(&f->tasks[i]);
  require(luaSC_collect(f->main, &f->tasks[i], &status, &results) == LUA_OK);
  require(status == expected && results == 1);
  result = lua_tointeger(f->main, -1);
  lua_pop(f->main, 1);
  require(f->tasks[i].affinity == NULL && f->tasks[i].slot == NULL);
  return result;
}
static void parked(struct fixture *f, unsigned i) {
  LuaScheduler *s = &f->scheduler;
  require(pthread_mutex_lock(&s->mutex) == 0);
  while (!f->tasks[i].published)
    require(pthread_cond_wait(&s->changed, &s->mutex) == 0);
  require(f->tasks[i].waiter != NULL && f->tasks[i].slot == NULL);
  require(pthread_mutex_unlock(&s->mutex) == 0);
}
static void queued(struct fixture *f, unsigned i) {
  LuaScheduler *s = &f->scheduler;
  int found = 0;
  require(pthread_mutex_lock(&s->mutex) == 0);
  while (!found) {
    LuaScheduledTask *task;
    require(pthread_mutex_lock(&s->slots[0].mutex) == 0);
    for (task = s->slots[0].head; task != NULL; task = task->next)
      if (task == &f->tasks[i])
        found = 1;
    require(pthread_mutex_unlock(&s->slots[0].mutex) == 0);
    if (!found)
      require(pthread_cond_wait(&s->changed, &s->mutex) == 0);
  }
  require(pthread_mutex_unlock(&s->mutex) == 0);
}
static void release_blockers(struct fixture *f) {
  lock(f);
  f->release[0] = f->release[1] = 1;
  notify(f);
  unlock(f);
}
static void finish(struct fixture *f) {
  require(f->scheduler.callbacks_active == 0);
  require(luaSC_destroy(f->main, &f->scheduler) == 0);
  require(f->main->l_G->tasks == NULL);
  lua_close(f->main);
  require(f->live == 0);
  require(pthread_cond_destroy(&f->changed) == 0);
  require(pthread_mutex_destroy(&f->mutex) == 0);
}

static int block(lua_State *L) {
  struct fixture *f = active;
  unsigned i = (unsigned)lua_tointeger(L, 1);
  require(i < 2 && native_marker == 0);
  lock(f);
  f->entered++;
  notify(f);
  while (!f->release[i])
    change(f);
  unlock(f);
  lua_pushinteger(L, 100 + i);
  return 1;
}
static int probe(lua_State *L) {
  require(native_marker == 0);
  __atomic_fetch_add(&active->probes, 1, __ATOMIC_RELAXED);
  lua_pushinteger(L, 42);
  return 1;
}
static int cpu_probe(lua_State *L) {
  require(native_marker == 0xCAFE);
  require(pthread_equal(pthread_self(), active->pinned_carrier));
  active->cpu_on_reserved++;
  lua_pushinteger(L, 42);
  return 1;
}

static int pinned_call(lua_State *L) {
  struct fixture *f = active;
  LuaCarrierPin outer = {0}, inner = {0};
  int status;
  require(luaQ_pincarrier(&outer) == LUA_OK);
  require(luaQ_pincarrier(&inner) == LUA_OK);
  require(native_marker == 0);
  native_marker = 0xCAFE;
  f->pinned_carrier = pthread_self();
  f->opened++;
  lua_pushvalue(L, 1);
  status = lua_pcall(L, 0, 1, 0);
  require(luaQ_pinowner(&outer) && luaQ_pinowner(&inner));
  require(native_marker == 0xCAFE);
  require(pthread_equal(pthread_self(), f->pinned_carrier));
  require(luaQ_unpincarrier(&outer) == LUA_OK && luaQ_pinowner(&inner));
  native_marker = 0;
  f->closed++;
  require(luaQ_unpincarrier(&inner) == LUA_OK);
  if (status != LUA_OK)
    return lua_error(L);
  return 1;
}

static void affinity(struct fixture *f, int cancel, int opaque) {
  init(f);
  if (opaque)
    foreign(f, "pinned", pinned_call);
  else
    managed(f, "pinned", pinned_call);
  foreign(f, "block", block);
  foreign(f, "probe", probe);
  managed(f, "cpu_probe", cpu_probe);
  submit(f, 0,
         "return pinned(function() local r=event:recv(); assert(r=='wake'); "
         "return 42 end)",
         0);
  parked(f, 0);
  lua_gc(f->main, LUA_GCCOLLECT);
  submit(f, 1, "return block(...)", 0);
  if (!opaque)
    submit(f, 2, "return block(...)", 1);
  lock(f);
  while (f->entered != (opaque ? 1u : 2u))
    change(f);
  unlock(f);
  if (!opaque) {
    submit(f, 3, "local t={}; for i=1,1000 do t[i]=i end; return cpu_probe()",
           0);
    require(collect(f, 3, LUA_OK) == 42);
    require(f->cpu_on_reserved == 1);
    submit(f, 4, "return probe()", 0);
    queued(f, 4);
    require(f->probes == 0); /* the only idle carrier owns the pin */
  }
  if (cancel)
    require(luaTK_requeststop(&f->tasks[0].root) == LUA_OK);
  else
    execute(f->main, "event:send('wake')");
  (void)collect(f, 0, cancel ? LUA_ERRRUN : LUA_OK);
  require(f->opened == 1 && f->closed == 1);
  release_blockers(f);
  require(collect(f, 1, LUA_OK) == 100);
  if (!opaque) {
    require(collect(f, 2, LUA_OK) == 101);
    require(collect(f, 4, LUA_OK) == 42);
    require(f->probes == 1);
  }
  finish(f);
}

static void marshal(lua_State *L, void *context) {
  struct fixture *f = context;
  LuaExecution *execution = luaQ_current();
  pthread_t carrier = pthread_self();
  if (f->opaque_marshalling) {
    lock(f);
    f->callback_started = 1;
    notify(f);
    while (!f->callback_release)
      change(f);
    unlock(f);
  }
  if (f->marshalling_error)
    luaL_error(L, "marshalling failed");
  lua_pushinteger(L, f->nested ? 2 : 0);
  lua_call(L, 1, 1);
  f->callback_value = (unsigned)lua_tointeger(L, -1);
  f->callback_parks = ((LuaScheduledTask *)execution->context)->parks;
  require(pthread_equal(carrier, pthread_self()));
}
static int nested(lua_State *L) {
  struct fixture *f = active;
  int saved = f->nested, status;
  unsigned old = f->callback_value;
  char error[64];
  f->nested = 0;
  status = luaSC_callback(&f->scheduler, &f->callback, marshal, f, error,
                          sizeof(error), NULL);
  require(status == LUA_OK);
  lua_pushinteger(L, f->callback_value);
  f->callback_value = old;
  f->nested = saved;
  return 1;
}
static int await_callback(lua_State *L) {
  struct fixture *f = active;
  lock(f);
  f->callback_started = 1;
  notify(f);
  while (!f->callback_release) {
    if (luaQ_waitstop(L, &f->mutex, &f->changed, &f->waiters)) {
      unlock(f);
      lua_lock(L);
      luaTK_throwstop(L);
    }
  }
  unlock(f);
  return 0;
}
static int allocation_failure(lua_State *L) {
  active->fail_allocations = 2; /* fail the body request and emergency retry */
  (void)lua_newuserdatauv(L, 8192, 0);
  return 0;
}
static int check_slot(lua_State *L) {
  LuaExecution *execution = luaQ_current();
  LuaScheduledTask *task = execution->context;
  require(execution->native_depth != 0 && execution->trusted != NULL);
  require(task->slot != NULL &&
          execution->allocation_cache == &task->slot->allocation);
  lua_pushinteger(L, 42);
  require(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);
  require(task->slot != NULL &&
          execution->allocation_cache == &task->slot->allocation);
  return 0;
}
static void register_callback(struct fixture *f, const char *source) {
  require(luaL_loadstring(f->main, source) == LUA_OK);
  require(luaCB_init(f->main, &f->callback, 1) == LUA_OK);
  lua_pop(f->main, 1);
}
static void *provider(void *context) {
  struct fixture *f = context;
  require(luaQ_current() == NULL);
  f->callback_status = luaSC_callback(&f->scheduler, &f->callback, marshal, f,
                                      f->error, sizeof(f->error), NULL);
  require(luaQ_current() == NULL);
  return NULL;
}

static void callbacks(struct fixture *f, const char *mode) {
  pthread_t thread;
  int wait = strcmp(mode, "wait") == 0 || strcmp(mode, "shutdown") == 0 ||
             strcmp(mode, "revoke") == 0 || strcmp(mode, "late") == 0;
  int shutdown = strcmp(mode, "shutdown") == 0 || strcmp(mode, "late") == 0;
  init(f);
  managed(f, "await_callback", await_callback);
  managed(f, "allocate_body", allocation_failure);
  managed(f, "check_slot", check_slot);
  foreign(f, "nested", nested);
  foreign(f, "block", block);
  if (strcmp(mode, "nested") == 0) {
    f->nested = 1;
    register_callback(
        f, "local n=...; if n>0 then return nested()+2 end; return 40");
  } else if (strcmp(mode, "late") == 0) {
    register_callback(f, "return 42");
    f->opaque_marshalling = 1;
  } else if (wait)
    register_callback(f, "await_callback(); return 42");
  else if (strcmp(mode, "oom") == 0) {
    register_callback(f, "allocate_body(); return 42");
  } else {
    register_callback(f, "check_slot(); return 42");
    f->marshalling_error = strcmp(mode, "marshal_error") == 0;
  }
  submit(f, 0, "return block(...)", 0);
  submit(f, 1, "return block(...)", 1);
  lock(f);
  while (f->entered != 2)
    change(f);
  unlock(f);
  require(pthread_create(&thread, NULL, provider, f) == 0);
  if (wait) {
    char error[64] = "unchanged";
    lock(f);
    while (!f->callback_started)
      change(f);
    unlock(f);
    require(luaSC_callback(&f->scheduler, &f->callback, marshal, f, error,
                           sizeof(error), NULL) == LUA_CB_CAPACITY);
    require(error[0] == '\0');
    luaCB_revoke(&f->callback);
    require(luaCB_destroy(f->main, &f->callback) == LUA_CB_PENDING);
    require(luaSC_destroy(f->main, &f->scheduler) == EBUSY);
    lua_gc(f->main, LUA_GCCOLLECT);
    if (shutdown) {
      require(luaSC_shutdown(&f->scheduler) == LUA_TK_PENDING);
      if (f->opaque_marshalling) {
        require(luaCB_destroy(f->main, &f->callback) == LUA_CB_PENDING);
        require(luaSC_destroy(f->main, &f->scheduler) == EBUSY);
        lock(f);
        f->callback_release = 1;
        notify(f);
        unlock(f);
      }
    } else {
      lock(f);
      f->callback_release = 1;
      luaQ_wake(&f->changed, &f->waiters);
      unlock(f);
    }
  }
  require(pthread_join(thread, NULL) == 0);
  if (shutdown || f->marshalling_error) {
    require(f->callback_status == LUA_ERRRUN);
    require(strstr(f->error,
                   shutdown ? "task stopped" : "marshalling failed") != NULL);
  } else if (strcmp(mode, "oom") == 0) {
    require(f->callback_status == LUA_ERRMEM && f->fail_allocations == 0);
  } else
    require(f->callback_status == LUA_OK && f->callback_value == 42);
  require(f->scheduler.callbacks_active == 0 &&
          f->scheduler.callbacks_peak == 1);
  require(f->scheduler.native_peak == 2 && f->scheduler.created == 3);
  luaCB_revoke(&f->callback);
  require(luaCB_destroy(f->main, &f->callback) == LUA_OK);
  release_blockers(f);
  (void)collect(f, 0, shutdown ? LUA_ERRRUN : LUA_OK);
  (void)collect(f, 1, shutdown ? LUA_ERRRUN : LUA_OK);
  if (shutdown)
    require(luaSC_shutdown(&f->scheduler) == LUA_OK);
  finish(f);
}

static void cpu_poll(lua_State *L, void *context) {
  struct fixture *f = context;
  (void)L;
  if (!f->cpu_ready) {
    lock(f);
    f->cpu_ready = 1;
    notify(f);
    unlock(f);
  }
}
static int call_registered(lua_State *L) {
  struct fixture *f = active;
  char error[64];
  int status = luaSC_callback(&f->scheduler, &f->callback, marshal, f, error,
                              sizeof(error), NULL);
  require(status == LUA_OK);
  lua_pushinteger(L, f->callback_value);
  return 1;
}
static void callback_fairness(struct fixture *f, int opaque) {
  pthread_t thread;
  init(f);
  execute(f->main, "ready={}");
  foreign(f, "block", block);
  foreign(f, "call_registered", call_registered);
  register_callback(f,
                    "local n=0; while not ready.value do n=n+1 end; return 42");
  lua_lock(f->callback.thread);
  f->callback.thread->state_poll = cpu_poll;
  f->callback.thread->state_poll_context = f;
  lua_unlock(f->callback.thread);
  submit(f, 0, "return block(...)", 0);
  if (!opaque)
    submit(f, 1, "return block(...)", 1);
  lock(f);
  while (f->entered != (opaque ? 1u : 2u))
    change(f);
  unlock(f);
  if (opaque)
    submit(f, 2, "return call_registered()", 0);
  else
    require(pthread_create(&thread, NULL, provider, f) == 0);
  lock(f);
  while (!f->cpu_ready)
    change(f);
  unlock(f);
  submit(f, 3, "ready.value=true; return 42", 0);
  require(collect(f, 3, LUA_OK) == 42);
  if (opaque)
    require(collect(f, 2, LUA_OK) == 42);
  else {
    require(pthread_join(thread, NULL) == 0);
    require(f->callback_status == LUA_OK && f->callback_value == 42);
  }
  luaCB_revoke(&f->callback);
  require(luaCB_destroy(f->main, &f->callback) == LUA_OK);
  release_blockers(f);
  (void)collect(f, 0, LUA_OK);
  if (!opaque)
    (void)collect(f, 1, LUA_OK);
  finish(f);
}

static int await_ack(lua_State *L) {
  struct fixture *f = active;
  lock(f);
  f->ack_ready = 1;
  notify(f);
  while (!f->ack_release) {
    if (luaQ_waitstop(L, &f->mutex, &f->changed, &f->waiters)) {
      unlock(f);
      lua_lock(L);
      luaTK_throwstop(L);
    }
  }
  unlock(f);
  return 0;
}
static int generated(lua_State *L) {
  (void)L;
  __atomic_fetch_add(&active->generated, 1, __ATOMIC_RELAXED);
  return 0;
}
static int validate_chunk(lua_State *L) {
  size_t size, i;
  unsigned work = 0;
  const unsigned char *chunk =
      (const unsigned char *)lua_tolstring(L, 1, &size);
  unsigned count = (unsigned)lua_tointeger(L, 2);
  LuaExecution *execution = luaQ_current();
  LuaScheduledTask *task = execution->context;
  require(task->slot != NULL && execution->trusted != NULL);
  require(size == 1024);
  for (i = 0; i < size; i++)
    require(chunk[i] == count % 251);
  luaQ_workn(L, &work, size); /* bounded validation participates in fairness */
  return 0;
}
static lua_Integer pipe_stat(struct fixture *f, const char *pipe,
                             const char *field) {
  lua_Integer value;
  lua_getglobal(f->main, pipe);
  lua_getfield(f->main, -1, "stats");
  lua_pushvalue(f->main, -2);
  require(lua_pcall(f->main, 1, 1, 0) == LUA_OK);
  lua_getfield(f->main, -1, field);
  value = lua_tointeger(f->main, -1);
  lua_pop(f->main, 3);
  return value;
}
static void wait_pipe(struct fixture *f, const char *pipe, const char *field) {
  /* The predicate is the real bounded transport, not a task-count surrogate. */
  while (pipe_stat(f, pipe, field) != 1)
    sched_yield();
}
static void streaming(struct fixture *f, int cancel, int shutdown) {
  const unsigned chunks = 1024, total = chunks * 1024;
  size_t baseline;
  pthread_t thread;
  init(f);
  managed(f, "await_callback", await_callback);
  managed(f, "await_ack", await_ack);
  managed(f, "generated", generated);
  managed(f, "validate_chunk", validate_chunk);
  foreign(f, "block", block);
  luaP_newpipe(f->main, 1);
  lua_setglobal(f->main, "response");
  register_callback(
      f, "await_callback(); local count=0; while true do local "
         "chunk=event:recv(); "
         "if not chunk then break end; count=count+1; assert(#chunk==1024); "
         "validate_chunk(chunk,count); "
         "response:send(tostring(count)) end; response:close(); return "
         "count*1024");
  submit(f, 0, "return block(...)", 0);
  submit(f, 1, "return block(...)", 1);
  lock(f);
  while (f->entered != 2)
    change(f);
  unlock(f);
  require(pthread_create(&thread, NULL, provider, f) == 0);
  lock(f);
  while (!f->callback_started)
    change(f);
  unlock(f);
  lua_gc(f->main, LUA_GCCOLLECT);
  baseline = f->peak =
      f->live; /* no allocator caller runs in these parked owners */
  submit(f, 2,
         "local count=1024; for first=1,count,2 do for i=first,first+1 do "
         "local chunk=string.rep(string.char(i%251),1024); generated(); "
         "event:send(chunk) end; if first==1 then await_ack() end; "
         "for i=first,first+1 do assert(response:recv()==tostring(i)) end end; "
         "event:close(); assert(response:recv()==nil); return count*1024",
         0);
  wait_pipe(f, "event", "waiting_senders");
  require(__atomic_load_n(&f->generated, __ATOMIC_RELAXED) == 2);
  require(pipe_stat(f, "event", "buffered_bytes") == 1024);
  lua_gc(f->main, LUA_GCCOLLECT);
  lock(f);
  f->callback_release = 1;
  luaQ_wake(&f->changed, &f->waiters);
  unlock(f);
  lock(f);
  while (!f->ack_ready)
    change(f);
  unlock(f);
  wait_pipe(f, "response", "waiting_senders");
  require(pipe_stat(f, "response", "sent_bytes") == 1);
  require(pipe_stat(f, "response", "buffered_bytes") == 1);
  lua_gc(f->main, LUA_GCCOLLECT);
  if (shutdown) {
    require(luaSC_shutdown(&f->scheduler) == LUA_TK_PENDING);
    require(!f->tasks[0].complete && !f->tasks[1].complete);
  } else if (cancel) {
    require(luaTK_requeststop(&f->tasks[2].root) == LUA_OK);
    (void)collect(f, 2, LUA_ERRRUN);
    execute(f->main, "event:close(); response:close()");
  } else {
    lock(f);
    f->ack_release = 1;
    luaQ_wake(&f->changed, &f->waiters);
    unlock(f);
  }
  if (!cancel || shutdown)
    require(collect(f, 2, shutdown ? LUA_ERRRUN : LUA_OK) ==
            (shutdown ? 0 : total));
  require(pthread_join(thread, NULL) == 0);
  require(f->callback_status == (cancel || shutdown ? LUA_ERRRUN : LUA_OK));
  if (!cancel && !shutdown) {
    require(f->callback_value == total);
    require(pipe_stat(f, "event", "sent_bytes") == total);
    require(pipe_stat(f, "event", "received_bytes") == total);
    require(__atomic_load_n(&f->generated, __ATOMIC_RELAXED) == chunks);
    require(f->callback_parks != 0 && f->tasks[2].parks != 0);
  }
  require(pipe_stat(f, "event", "peak_bytes") == 1024);
  require(pipe_stat(f, "response", "peak_bytes") <= 4);
  require(pipe_stat(f, "event", "waiting_senders") == 0);
  require(pipe_stat(f, "response", "waiting_senders") == 0);
  require(f->peak < baseline + total / 2); /* cannot retain the whole message */
  require(f->scheduler.created == 3 && f->scheduler.native_peak == 2);
  luaCB_revoke(&f->callback);
  require(luaCB_destroy(f->main, &f->callback) == LUA_OK);
  release_blockers(f);
  (void)collect(f, 0, shutdown ? LUA_ERRRUN : LUA_OK);
  (void)collect(f, 1, shutdown ? LUA_ERRRUN : LUA_OK);
  finish(f);
}

int main(int argc, char **argv) {
  struct fixture *f = calloc(1, sizeof(*f));
  require(f != NULL && argc == 2);
  if (strcmp(argv[1], "affinity") == 0)
    affinity(f, 0, 0);
  else if (strcmp(argv[1], "affinity_cancel") == 0)
    affinity(f, 1, 0);
  else if (strcmp(argv[1], "opaque_wait") == 0)
    affinity(f, 0, 1);
  else if (strcmp(argv[1], "opaque_cancel") == 0)
    affinity(f, 1, 1);
  else if (strcmp(argv[1], "foreign_fairness") == 0)
    callback_fairness(f, 0);
  else if (strcmp(argv[1], "opaque_fairness") == 0)
    callback_fairness(f, 1);
  else if (strcmp(argv[1], "stream") == 0)
    streaming(f, 0, 0);
  else if (strcmp(argv[1], "stream_cancel") == 0)
    streaming(f, 1, 0);
  else if (strcmp(argv[1], "stream_shutdown") == 0)
    streaming(f, 0, 1);
  else
    callbacks(f, argv[1]);
  free(f);
  return 0;
}
