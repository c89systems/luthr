/* Released module acceptance using the shared private scheduler fixture. */
#include <errno.h>
#include <time.h>

enum {
  BINDING_RECORDS = 2048,
  BINDING_PAYLOAD = 192,
  BINDING_HEAP_LIMIT = 256 * 1024
};
struct binding_fixture {
  struct pool *pool;
  const char *mode;
  unsigned arrived, seen[4], reads, callbacks, deny, denied;
  unsigned consumed, phase;
  unsigned constructors, cold_initializers;
  int release_constructor;
  pthread_mutex_t constructor_mutex;
  pthread_cond_t constructor_changed;
  LuaWaiter *constructor_waiters;
  LuaOwner constructor_owner;
  unsigned constructor_depth;
  int mdf;
  pthread_t carriers[4];
  size_t output_bytes, input_bytes, expected_size, expected_offset;
  char payload[BINDING_PAYLOAD + 1], expected[BINDING_PAYLOAD + 64];
  int input[2];
};
static struct binding_fixture *binding_active;
static void binding_wait_change(struct pool *pool) {
  struct timespec deadline;
  int result;
  require(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
  deadline.tv_sec += 60;
  result = pthread_cond_timedwait(&pool->changed, &pool->mutex, &deadline);
  if (result == ETIMEDOUT) {
    fprintf(stderr,
            "native module stalled: arrived=%u constructors=%u cold=%u done=%u "
            "managed=%u native=%u\n",
            binding_active->arrived, binding_active->constructors,
            binding_active->cold_initializers, pool->done, pool->managed,
            pool->native);
    for (unsigned n = 0; n < MAX_TASKS; ++n) {
      struct task *task = &pool->tasks[n];
      if (task->state != NULL)
        fprintf(stderr,
                "task=%u worker=%d target=%d permit=%d native=%d pinned=%d "
                "finished=%d reason=%d waits=%u migrations=%u\n",
                n, task->worker, task->target, task->permit, task->native,
                task->pinned, task->finished, task->reason,
                task->resource_waits, task->migrations);
    }
  }
  require(result == 0);
}
int luaopen_lonejson_core(lua_State *);
int luaopen_libmdf_core(lua_State *);
int __real_lua_getfield(lua_State *, int, const char *);
int __wrap_lua_getfield(lua_State *state, int index, const char *key) {
  int result = __real_lua_getfield(state, index, key);
  struct binding_fixture *test = binding_active;
  if (test != NULL && test->mdf && strstr(test->mode, "init") != NULL &&
      index == LUA_REGISTRYINDEX && strcmp(key, "libmdf.owner_map") == 0 &&
      result == LUA_TNIL) {
    lock(test->pool);
    test->cold_initializers++;
    require(test->cold_initializers == 1 ||
            (strstr(test->mode, "owner_stop") != NULL &&
             test->release_constructor));
    signal_change(test->pool);
    while (!test->release_constructor)
      binding_wait_change(test->pool);
    unlock(test->pool);
  }
  return result;
}
/* Private integration metadata: MDF's two constructors share lazy registry
 * initialization. Guard those calls, leaving receiver rendering independent.
 * Waiters park without VM-object ownership; protected entry releases the gate
 * before rethrowing Lua errors, including stop and allocation failure. */
static int binding_mdf_constructor(lua_State *state) {
  struct binding_fixture *test = binding_active;
  int arguments = lua_gettop(state);
  int status;
  lua_pushvalue(state, lua_upvalueindex(1));
  lua_insert(state, 1);
  if (strstr(test->mode, "init") != NULL) {
    lock(test->pool);
    test->constructors++;
    signal_change(test->pool);
    unlock(test->pool);
  }
  check(pthread_mutex_lock(&test->constructor_mutex));
  while (test->constructor_depth != 0 &&
         !luaQ_isowner(&test->constructor_owner)) {
    if (luaQ_waitstop(state, &test->constructor_mutex,
                      &test->constructor_changed, &test->constructor_waiters)) {
      check(pthread_mutex_unlock(&test->constructor_mutex));
      luaX_lockstate(state);
      luaTK_throwstop(state);
    }
  }
  test->constructor_depth++;
  luaQ_setowner(&test->constructor_owner);
  check(pthread_mutex_unlock(&test->constructor_mutex));
  status = lua_pcall(state, arguments, LUA_MULTRET, 0);
  check(pthread_mutex_lock(&test->constructor_mutex));
  require(test->constructor_depth != 0 &&
          luaQ_isowner(&test->constructor_owner));
  if (--test->constructor_depth == 0)
    luaQ_wake(&test->constructor_changed, &test->constructor_waiters);
  check(pthread_mutex_unlock(&test->constructor_mutex));
  if (status != LUA_OK)
    return lua_error(state);
  return lua_gettop(state);
}
static void binding_mdf_metadata(lua_State *state) {
  const char *constructors[] = {"new", "document_stream"};
  int module = lua_absindex(state, -1);
  for (unsigned n = 0; n < sizeof(constructors) / sizeof(constructors[0]);
       ++n) {
    lua_getfield(state, module, constructors[n]);
    luaQ_pushmanaged(state, binding_mdf_constructor, 1);
    lua_setfield(state, module, constructors[n]);
  }
}
ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *buffer, size_t size) {
  struct binding_fixture *test = binding_active;
  if (test != NULL && test->input[0] >= 0 &&
      luaQ_current() == &test->pool->tasks[2].fiber.execution) {
    require(luaQ_current()->native_depth != 0);
    lock(test->pool);
    test->reads++;
    signal_change(test->pool);
    unlock(test->pool);
  }
  return __real_read(fd, buffer, size);
}
static void *binding_allocate(void *context, void *old, size_t old_size,
                              size_t size) {
  struct pool *pool = context;
  struct binding_fixture *test = binding_active;
  if (old == NULL)
    old_size = 0;
  if (size > old_size) {
    if (luaQ_current() == &pool->tasks[2].fiber.execution && test->deny != 0) {
      test->deny--;
      test->denied++;
      return NULL;
    }
    if (size - old_size > BINDING_HEAP_LIMIT - pool->stream_live)
      return NULL;
  }
  return utf8_allocate(context, old, old_size, size);
}
static void *binding_host_allocate(void *context, size_t size) {
  return binding_allocate(context, NULL, 0, size);
}
static void binding_host_free(void *context, void *memory, size_t size) {
  (void)binding_allocate(context, memory, size, 0);
}
static int binding_task_id(lua_State *state) {
  struct task *task = luaQ_current()->context;
  lua_pushinteger(state, task->id);
  return 1;
}
static int binding_fault(lua_State *state) {
  struct task *task = luaQ_current()->context;
  const char *mode = binding_active->mode;
  lua_pushstring(state, task->id != 2           ? "normal"
                        : strstr(mode, "error") ? "error"
                        : strstr(mode, "oom")   ? "oom"
                                                : "normal");
  return 1;
}
static int binding_oom(lua_State *state) {
  binding_active->deny = 2;
  lua_newuserdatauv(state, 4096, 0);
  return luaL_error(state, "LoneJSON allocation denial did not fail");
}
static int binding_checkpoint(lua_State *state) {
  struct binding_fixture *test = binding_active;
  struct task *task = luaQ_current()->context;
  unsigned id = (unsigned)luaL_checkinteger(state, 1);
  if (id != (unsigned)task->id || task->state != state ||
      task->fiber.execution.native_depth == 0)
    fprintf(
        stderr,
        "native module callback id=%u task=%d state=%p expected=%p native=%u "
        "migrations=%u\n",
        id, task->id, (void *)state, (void *)task->state,
        task->fiber.execution.native_depth, task->migrations);
  require(id == (unsigned)task->id && id >= 2 && id <= 3 &&
          task->state == state && task->fiber.execution.native_depth != 0);
  lock(test->pool);
  if (!test->seen[id]) {
    test->seen[id] = 1;
    test->carriers[id] = pthread_self();
    test->arrived++;
    signal_change(test->pool);
    while (!test->pool->release[0])
      binding_wait_change(test->pool);
  }
  require(pthread_equal(test->carriers[id], pthread_self()));
  unlock(test->pool);
  return 0;
}
static int binding_stream_callback(lua_State *state) {
  struct binding_fixture *test = binding_active;
  struct task *task = luaQ_current()->context;
  require(task->id == 2 && state == task->state &&
          task->fiber.execution.native_depth != 0);
  lock(test->pool);
  test->callbacks++;
  unlock(test->pool);
  return 0;
}
static int binding_read(lua_State *state) {
  char chunk[256];
  lua_Integer requested = luaL_checkinteger(state, 1);
  ssize_t count;
  size_t size;
  require(requested > 0);
  size = (lua_Unsigned)requested < sizeof(chunk) ? (size_t)requested
                                                 : sizeof(chunk);
  do {
    count = __wrap_read(binding_active->input[0], chunk, size);
  } while (count < 0 && errno == EINTR);
  require(count >= 0);
  if (count == 0)
    lua_pushnil(state);
  else
    lua_pushlstring(state, chunk, (size_t)count);
  return 1;
}
static void binding_expected(struct binding_fixture *test) {
  int size = test->mdf
                 ? snprintf(test->expected, sizeof(test->expected),
                            "%srow-%u %s\n", test->consumed == 0 ? "" : "\n",
                            test->consumed + 1, test->payload)
                 : snprintf(test->expected, sizeof(test->expected),
                            "{\"id\":%u,\"value\":\"%s\"}",
                            (test->consumed + 1) * 2, test->payload);
  require(size > 0 && (size_t)size < sizeof(test->expected));
  test->expected_size = (size_t)size;
  test->expected_offset = 0;
}
static int binding_accept(lua_State *state) {
  struct binding_fixture *test = binding_active;
  size_t size, i;
  const char *chunk = luaL_checklstring(state, 1, &size);
  require(size != 0 && size <= 4096);
  lock(test->pool);
  for (i = 0; i < size; ++i) {
    char c = chunk[i];
    if (test->mdf) {
      require(test->consumed < BINDING_RECORDS &&
              c == test->expected[test->expected_offset++]);
      if (test->expected_offset == test->expected_size) {
        test->consumed++;
        binding_expected(test);
        signal_change(test->pool);
      }
    } else if (test->phase == 0) {
      require(c == '[');
      binding_expected(test);
      test->phase = 1;
    } else if (test->phase == 1) {
      require(c == test->expected[test->expected_offset++]);
      if (test->expected_offset == test->expected_size) {
        test->consumed++;
        test->phase = 2;
        signal_change(test->pool);
      }
    } else if (test->phase == 2 && c == ',') {
      binding_expected(test);
      test->phase = 1;
    } else {
      require(test->phase == 2 && c == ']' &&
              test->consumed == BINDING_RECORDS);
      test->phase = 3;
    }
    test->output_bytes++;
  }
  unlock(test->pool);
  return 0;
}
static void binding_write(struct binding_fixture *test, const char *data,
                          size_t size) {
  while (size != 0) {
    ssize_t count = write(test->input[1], data, size);
    if (count < 0 && errno == EINTR)
      continue;
    require(count > 0);
    data += count;
    size -= (size_t)count;
    test->input_bytes += (size_t)count;
  }
}
static void binding_write_record(struct binding_fixture *test, unsigned id) {
  char record[BINDING_PAYLOAD + 64];
  int size =
      test->mdf
          ? snprintf(record, sizeof(record), "row-%u %s\n\n", id, test->payload)
          : snprintf(record, sizeof(record), "%s{\"id\":%u,\"value\":\"%s\"}",
                     id == 1   ? "["
                     : id == 2 ? ""
                               : ",",
                     id, test->payload);
  require(size > 0 && (size_t)size < sizeof(record));
  binding_write(test, record, (size_t)size);
}
static void binding_globals(lua_State *state) {
  int status;
  const char *directory =
      binding_active->mdf ? LUTHRUM_MDF_LUA_DIR : LUTHRUM_LONEJSON_LUA_DIR;
  lua_pushfstring(state, "%s/?.lua;%s/?/init.lua", directory, directory);
  lua_getglobal(state, "package");
  lua_insert(state, -2);
  lua_setfield(state, -2, "path");
  lua_pop(state, 1);
  luaL_requiref(
      state, binding_active->mdf ? "libmdf.core" : "lonejson.core",
      binding_active->mdf ? luaopen_libmdf_core : luaopen_lonejson_core, 0);
  lua_pop(state, 1);
  lua_getglobal(state, "require");
  lua_pushstring(state, binding_active->mdf ? "libmdf" : "lonejson");
  status = lua_pcall(state, 1, 1, 0);
  if (status != LUA_OK)
    fprintf(stderr, "native module load: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
  if (binding_active->mdf)
    binding_mdf_metadata(state);
  lua_pop(state, 1);
  luaQ_pushmanaged(state, binding_task_id, 0);
  lua_setglobal(state, "binding_task_id");
  luaQ_pushmanaged(state, binding_fault, 0);
  lua_setglobal(state, "binding_fault");
  luaQ_pushmanaged(state, binding_oom, 0);
  lua_setglobal(state, "binding_oom");
  luaQ_pushmanaged(state, binding_checkpoint, 0);
  lua_setglobal(state, "binding_checkpoint");
  luaQ_pushmanaged(state, binding_stream_callback, 0);
  lua_setglobal(state, "binding_stream_callback");
  luaQ_pushmanaged(state, binding_accept, 0);
  lua_setglobal(state, "binding_accept");
  lua_pushcfunction(state, binding_read);
  lua_setglobal(state, "binding_read");
}
static void binding_run(struct task *task) {
  task->status = luaTK_resume(&task->root, 0, &task->results);
  if (task->status != LUA_OK && !strstr(binding_active->mode, "stop") &&
      !strstr(binding_active->mode, "cancel") &&
      !strstr(binding_active->mode, "shutdown")) {
    lock(task->pool);
    fprintf(stderr, "native module task %d failed after %u streamed items\n",
            task->id, binding_active->consumed);
    unlock(task->pool);
    require(lua_checkstack(task->state, LUA_MINSTACK));
    luaL_traceback(task->state, task->state, lua_tostring(task->state, -1), 1);
    fprintf(stderr, "native module task %d traceback: %s\n", task->id,
            lua_tostring(task->state, -1));
    lua_pop(task->state, 1);
    require(task->status == LUA_OK);
  }
}
static int exercise_module(const char *mode) {
  struct binding_fixture *test = calloc(1, sizeof(*test));
  struct pool *pool = calloc(1, sizeof(*pool));
  pthread_t workers[MAX_WORKERS];
  const int stream_ids[] = {0, 2, 3, 6}, normal_ids[] = {2, 3, 6};
  int stream = strstr(mode, "stream") != NULL;
  int stopped = strstr(mode, "stop") != NULL && strstr(mode, "init") == NULL;
  int shutdown = strstr(mode, "shutdown") != NULL;
  int initialization = strstr(mode, "init") != NULL;
  int cancel_constructor = initialization && strstr(mode, "cancel") != NULL;
  int stop_constructor_owner =
      initialization && strstr(mode, "owner_stop") != NULL;
  const int *ids = stream ? stream_ids : normal_ids;
  unsigned count = stream ? 4u : initialization ? 3u : 2u;
  require(test != NULL && pool != NULL);
  fixture = pool;
  binding_active = test;
  test->pool = pool;
  test->mode = mode;
  test->mdf = strncmp(mode, "mdf_", 4) == 0;
  test->input[0] = test->input[1] = -1;
  check(pthread_mutex_init(&test->constructor_mutex, NULL));
  check(pthread_cond_init(&test->constructor_changed, NULL));
  memset(test->payload, 'x', BINDING_PAYLOAD);
  pool->managed_limit = 2;
  check(pthread_mutex_init(&pool->mutex, NULL));
  check(pthread_cond_init(&pool->changed, NULL));
  if (strstr(mode, "facade")) {
    static const unsigned char capture[] = "require('fixture')";
    luthrum_lua_runtime_allocator_config config = {0};
    config.user = pool;
    config.alloc_fn = binding_host_allocate;
    config.realloc_fn = binding_allocate;
    config.free_fn = binding_host_free;
    config.max_bytes = BINDING_HEAP_LIMIT;
    require(luthrum_lua_runtime_new_with_allocator(&pool->runtime, &config) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_openlibs(pool->runtime) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_register_c_module(pool->runtime, "fixture",
                                                  open_fixture) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_run_buffer(
                pool->runtime, capture, sizeof(capture) - 1, "native_module", 0,
                NULL, 0) == LUTHRUM_LUA_RUNTIME_OK);
  } else {
    pool->main = lua_newstate(binding_allocate, pool, 0);
    require(pool->main != NULL);
    luaL_openlibs(pool->main);
  }
  if (strstr(mode, "generational"))
    lua_gc(pool->main, LUA_GCGEN);
  if (stream) {
    /* Reserve headroom for ordinary collection/finalizers before the hard cap.
     * Emergency collection alone deliberately cannot run native finalizers. */
    lua_gc(pool->main, LUA_GCPARAM, LUA_GCPPAUSE, 100);
    lua_gc(pool->main, LUA_GCPARAM, LUA_GCPSTEPMUL, 1000);
    lua_gc(pool->main, LUA_GCPARAM, LUA_GCPMINORMUL, 10);
  }
  binding_globals(pool->main);
  if (stream) {
    require(pipe(test->input) == 0);
    luaP_newpipe(pool->main, 1);
    lua_setglobal(pool->main, "q");
    lua_pushinteger(pool->main, test->input[0]);
    lua_setglobal(pool->main, "binding_input_fd");
    lua_pushinteger(pool->main, BINDING_PAYLOAD);
    lua_setglobal(pool->main, "binding_payload_size");
    lua_pushcfunction(pool->main, block_binding);
    lua_setglobal(pool->main, "block");
    if (test->mdf)
      binding_expected(test);
  }
  for (unsigned n = 0; n < count; ++n) {
    int id = ids[n];
    struct task *task = &pool->tasks[id];
    task->state = lua_newthread(pool->main);
    if (id == 2 || (!stream && id == 3))
      require(luaL_loadfile(task->state,
                            test->mdf ? (stream ? LUTHRUM_MDF_STREAM_SCRIPT
                                                : LUTHRUM_MDF_SCRIPT)
                                      : (stream ? LUTHRUM_LONEJSON_STREAM_SCRIPT
                                                : LUTHRUM_LONEJSON_SCRIPT)) ==
              LUA_OK);
    else
      require(
          luaL_loadstring(
              task->state,
              id == 0 ? "return block(...)"
              : id == 3
                  ? "while true do local s=q:recv(); if s==nil then break end; "
                    "binding_accept(s) end; return 42"
                  : "local sum=0; for n=1,4000 do sum=sum+n end; return 42") ==
          LUA_OK);
    if (id == 0) {
      unsigned *payload;
      lua_pushinteger(task->state, 0);
      payload = lua_newuserdatauv(task->state, sizeof(*payload), 0);
      *payload = 12345;
    }
    init_task(pool, task, id, NULL);
    if (id == 2 || (!stream && id == 3))
      task->run = binding_run;
    task->target = stream ? (id == 0 ? 0 : id == 2 ? 2 : 3) : -1;
    lua_settop(pool->main, 0);
  }
  for (int n = 0; n < MAX_WORKERS; ++n) {
    pool->workers[n].pool = pool;
    check(pthread_create(&workers[n], NULL, worker_entry, &pool->workers[n]));
  }
  if (!stream) {
    submit(pool, 2);
    if (initialization) {
      lock(pool);
      while (test->cold_initializers != 1)
        binding_wait_change(pool);
      unlock(pool);
    }
    submit(pool, 3);
    if (initialization) {
      lock(pool);
      while (test->cold_initializers != 1 || test->constructors != 2 ||
             pool->tasks[3].resource_waits + pool->tasks[2].resource_waits == 0)
        binding_wait_change(pool);
      unlock(pool);
      submit(pool, 6);
      lock(pool);
      while (!pool->tasks[6].finished)
        binding_wait_change(pool);
      require(!test->release_constructor && test->cold_initializers == 1);
      unlock(pool);
      if (cancel_constructor) {
        require(luaTK_requeststop(&pool->tasks[3].root) == LUA_OK);
        lock(pool);
        while (!pool->tasks[3].finished)
          binding_wait_change(pool);
        require(!test->release_constructor);
        unlock(pool);
      }
      if (stop_constructor_owner) {
        int status = -99, results = -99;
        require(luaTK_requeststop(&pool->tasks[2].root) == LUA_OK);
        require(luaTK_collect(pool->main, &pool->tasks[2].root, &status,
                              &results) == LUA_TK_PENDING);
        require(status == -99 && results == -99);
      }
      lua_gc(pool->main, LUA_GCCOLLECT);
      lock(pool);
      test->release_constructor = 1;
      signal_change(pool);
      unlock(pool);
    }
    lock(pool);
    while (test->arrived !=
           (cancel_constructor || stop_constructor_owner ? 1u : 2u))
      binding_wait_change(pool);
    require(pool->native ==
            (cancel_constructor || stop_constructor_owner ? 1u : 2u));
    unlock(pool);
    lua_gc(pool->main, LUA_GCCOLLECT);
    lua_gc(pool->main, LUA_GCCOLLECT);
    if (shutdown)
      require(luaTK_shutdown(G(pool->main)) == LUA_TK_PENDING);
    else if (stopped)
      require(luaTK_requeststop(&pool->tasks[2].root) == LUA_OK);
    if (stopped || shutdown) {
      int status = -99, results = -99;
      require(luaTK_collect(pool->main, &pool->tasks[2].root, &status,
                            &results) == LUA_TK_PENDING);
      require(status == -99 && results == -99);
    }
    lock(pool);
    if (strstr(mode, "migrate")) {
      pool->tasks[2].migrate_any = pool->tasks[3].migrate_any = 1;
    }
    pool->release[0] = 1;
    signal_change(pool);
    unlock(pool);
  } else {
    submit(pool, 0);
    lock(pool);
    while (pool->entered != 1)
      binding_wait_change(pool);
    unlock(pool);
    submit(pool, 2);
    fprintf(stderr, "native module stream: waiting for native read\n");
    lock(pool);
    while (test->reads == 0)
      binding_wait_change(pool);
    require(pool->native == 2);
    unlock(pool);
    binding_write_record(test, 1);
    if (!test->mdf)
      binding_write(test, ",", 1); /* first JSON item delimiter */
    fprintf(stderr, "native module stream: waiting for sink backpressure\n");
    lock(pool);
    while (pool->tasks[2].pinned_resource_waits == 0)
      binding_wait_change(pool);
    require(pool->native == 2 && !pool->release[0]);
    unlock(pool);
    lua_gc(pool->main, LUA_GCCOLLECT);
    lua_gc(pool->main, LUA_GCCOLLECT);
    submit(pool, 6);
    lock(pool);
    while (!pool->tasks[6].finished)
      binding_wait_change(pool);
    require(pool->native == 2 && !pool->release[0]);
    unlock(pool);
    submit(pool, 3);
    fprintf(stderr,
            "native module stream: waiting for first output and next read\n");
    lock(pool);
    while ((test->mdf ? test->output_bytes == 0 : test->consumed == 0) ||
           test->reads < 2)
      binding_wait_change(pool);
    require((test->mdf ? test->output_bytes != 0 : test->consumed == 1) &&
            !pool->tasks[2].finished && pool->native == 2);
    unlock(pool);
    if (stopped) {
      int status = -99, results = -99;
      require(luaTK_requeststop(&pool->tasks[2].root) == LUA_OK);
      require(luaTK_collect(pool->main, &pool->tasks[2].root, &status,
                            &results) == LUA_TK_PENDING);
      require(status == -99 && results == -99);
      lua_gc(pool->main, LUA_GCCOLLECT);
    } else {
      for (unsigned n = 2; n <= BINDING_RECORDS; ++n)
        binding_write_record(test, n);
      if (!test->mdf)
        binding_write(test, "]", 1);
    }
    require(close(test->input[1]) == 0);
    test->input[1] = -1;
    lock(pool);
    while (!pool->tasks[2].finished)
      binding_wait_change(pool);
    unlock(pool);
    if (stopped) {
      lua_getglobal(pool->main, "q");
      lua_getfield(pool->main, -1, "close");
      lua_pushvalue(pool->main, -2);
      require(lua_pcall(pool->main, 1, 0, 0) == LUA_OK);
      lua_pop(pool->main, 1);
    }
    lock(pool);
    pool->release[0] = 1;
    signal_change(pool);
    unlock(pool);
  }
  lock(pool);
  while (pool->done != count)
    binding_wait_change(pool);
  require(pool->native == 0 && pool->managed == 0 && pool->native_peak == 2 &&
          pool->managed_peak <= 2);
  pool->stop = 1;
  signal_change(pool);
  unlock(pool);
  for (int n = 0; n < MAX_WORKERS; ++n)
    check(pthread_join(workers[n], NULL));
  for (unsigned n = 0; n < count; ++n) {
    int id = ids[n], status = -99, results = -99;
    int failed = shutdown || ((stopped || stop_constructor_owner) && id == 2) ||
                 (cancel_constructor && id == 3);
    require(luaTK_collect(pool->main, &pool->tasks[id].root, &status,
                          &results) == LUA_OK);
    if (status != (failed ? LUA_ERRRUN : LUA_OK))
      fprintf(stderr, "native module task %d: %s\n", id,
              lua_tostring(pool->main, -1));
    require(status == (failed ? LUA_ERRRUN : LUA_OK) && results == 1);
    if (failed)
      require(strstr(lua_tostring(pool->main, -1), "task stopped") != NULL);
    else if (id == 2 && stream)
      require(lua_tointeger(pool->main, -1) ==
              (test->mdf ? 42 : BINDING_RECORDS));
    else if (id != 0)
      require(lua_tointeger(pool->main, -1) == 42);
    lua_pop(pool->main, 1);
    test_fiber_destroy(&pool->tasks[id].fiber);
  }
  if (strstr(mode, "migrate"))
    require(pool->tasks[2].migrations + pool->tasks[3].migrations != 0);
  if (strstr(mode, "oom"))
    require(test->denied == (test->mdf ? 4u : 2u) && test->deny == 0);
  if (stream) {
    if (test->mdf && stopped)
      require(test->consumed == 0 && test->expected_offset != 0);
    else {
      require(test->consumed == (stopped ? 1u : BINDING_RECORDS));
      require(test->mdf ? test->expected_offset == 0
                        : test->phase == (stopped ? 2u : 3u));
    }
    if (!stopped)
      require(test->input_bytes > BINDING_HEAP_LIMIT &&
              test->output_bytes > BINDING_HEAP_LIMIT);
    require(pool->tasks[2].pinned_resource_waits != 0 && test->callbacks != 0);
  }
  require(G(pool->main)->tasks == NULL &&
          pool->stream_peak <= BINDING_HEAP_LIMIT);
  if (shutdown)
    require(luaTK_shutdown(G(pool->main)) == LUA_OK);
  lua_gc(pool->main, LUA_GCCOLLECT);
  lua_gc(pool->main, LUA_GCCOLLECT);
  if (stream)
    require(close(test->input[0]) == 0);
  if (pool->runtime)
    luthrum_lua_runtime_free(pool->runtime);
  else
    lua_close(pool->main);
  require(pool->stream_live == 0);
  check(pthread_cond_destroy(&pool->changed));
  check(pthread_mutex_destroy(&pool->mutex));
  require(test->constructor_depth == 0 && test->constructor_waiters == NULL);
  check(pthread_cond_destroy(&test->constructor_changed));
  check(pthread_mutex_destroy(&test->constructor_mutex));
  binding_active = NULL;
  free(pool);
  free(test);
  return 0;
}
