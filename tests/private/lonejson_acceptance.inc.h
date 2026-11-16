/* Released module acceptance using the shared private scheduler fixture. */
#include <errno.h>

enum { JSON_RECORDS = 2048, JSON_PAYLOAD = 192, JSON_HEAP_LIMIT = 256 * 1024 };
struct json_fixture {
  struct pool *pool;
  const char *mode;
  unsigned arrived, seen[4], reads, callbacks, deny, denied;
  unsigned consumed, phase;
  pthread_t carriers[4];
  size_t output_bytes, input_bytes, expected_size, expected_offset;
  char payload[JSON_PAYLOAD + 1], expected[JSON_PAYLOAD + 64];
  int input[2];
};
static struct json_fixture *json_active;
int luaopen_lonejson_core(lua_State *);
ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *buffer, size_t size) {
  struct json_fixture *test = json_active;
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
static void *json_allocate(void *context, void *old, size_t old_size,
                           size_t size) {
  struct pool *pool = context;
  struct json_fixture *test = json_active;
  if (old == NULL)
    old_size = 0;
  if (size > old_size) {
    if (luaQ_current() == &pool->tasks[2].fiber.execution && test->deny != 0) {
      test->deny--;
      test->denied++;
      return NULL;
    }
    if (size - old_size > JSON_HEAP_LIMIT - pool->stream_live)
      return NULL;
  }
  return utf8_allocate(context, old, old_size, size);
}
static void *json_host_allocate(void *context, size_t size) {
  return json_allocate(context, NULL, 0, size);
}
static void json_host_free(void *context, void *memory, size_t size) {
  (void)json_allocate(context, memory, size, 0);
}
static int json_task_id(lua_State *state) {
  struct task *task = luaQ_current()->context;
  lua_pushinteger(state, task->id);
  return 1;
}
static int json_fault(lua_State *state) {
  struct task *task = luaQ_current()->context;
  const char *mode = json_active->mode;
  lua_pushstring(state, task->id != 2           ? "normal"
                        : strstr(mode, "error") ? "error"
                        : strstr(mode, "oom")   ? "oom"
                                                : "normal");
  return 1;
}
static int json_oom(lua_State *state) {
  json_active->deny = 2;
  lua_newuserdatauv(state, 4096, 0);
  return luaL_error(state, "LoneJSON allocation denial did not fail");
}
static int json_checkpoint(lua_State *state) {
  struct json_fixture *test = json_active;
  struct task *task = luaQ_current()->context;
  unsigned id = (unsigned)luaL_checkinteger(state, 1);
  if (id != (unsigned)task->id || task->state != state ||
      task->fiber.execution.native_depth == 0)
    fprintf(stderr,
            "LoneJSON callback id=%u task=%d state=%p expected=%p native=%u "
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
      wait_change(test->pool);
  }
  require(pthread_equal(test->carriers[id], pthread_self()));
  unlock(test->pool);
  return 0;
}
static int json_stream_callback(lua_State *state) {
  struct json_fixture *test = json_active;
  struct task *task = luaQ_current()->context;
  require(task->id == 2 && state == task->state &&
          task->fiber.execution.native_depth != 0);
  lock(test->pool);
  test->callbacks++;
  unlock(test->pool);
  return 0;
}
static void json_expected(struct json_fixture *test) {
  int size = snprintf(test->expected, sizeof(test->expected),
                      "{\"id\":%u,\"value\":\"%s\"}", (test->consumed + 1) * 2,
                      test->payload);
  require(size > 0 && (size_t)size < sizeof(test->expected));
  test->expected_size = (size_t)size;
  test->expected_offset = 0;
}
static int json_accept(lua_State *state) {
  struct json_fixture *test = json_active;
  size_t size, i;
  const char *chunk = luaL_checklstring(state, 1, &size);
  require(size != 0 && size <= 4096);
  lock(test->pool);
  for (i = 0; i < size; ++i) {
    char c = chunk[i];
    if (test->phase == 0) {
      require(c == '[');
      json_expected(test);
      test->phase = 1;
    } else if (test->phase == 1) {
      require(c == test->expected[test->expected_offset++]);
      if (test->expected_offset == test->expected_size) {
        test->consumed++;
        test->phase = 2;
        signal_change(test->pool);
      }
    } else if (test->phase == 2 && c == ',') {
      json_expected(test);
      test->phase = 1;
    } else {
      require(test->phase == 2 && c == ']' && test->consumed == JSON_RECORDS);
      test->phase = 3;
    }
    test->output_bytes++;
  }
  unlock(test->pool);
  return 0;
}
static void json_write(struct json_fixture *test, const char *data,
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
static void json_write_record(struct json_fixture *test, unsigned id) {
  char record[JSON_PAYLOAD + 64];
  int size = snprintf(record, sizeof(record), "%s{\"id\":%u,\"value\":\"%s\"}",
                      id == 1   ? "["
                      : id == 2 ? ""
                                : ",",
                      id, test->payload);
  require(size > 0 && (size_t)size < sizeof(record));
  json_write(test, record, (size_t)size);
}
static void json_globals(lua_State *state) {
  int status;
  lua_pushfstring(state, "%s/?.lua;%s/?/init.lua", LUTHRUM_LONEJSON_LUA_DIR,
                  LUTHRUM_LONEJSON_LUA_DIR);
  lua_getglobal(state, "package");
  lua_insert(state, -2);
  lua_setfield(state, -2, "path");
  lua_pop(state, 1);
  luaL_requiref(state, "lonejson.core", luaopen_lonejson_core, 0);
  lua_pop(state, 1);
  lua_getglobal(state, "require");
  lua_pushliteral(state, "lonejson");
  status = lua_pcall(state, 1, 1, 0);
  if (status != LUA_OK)
    fprintf(stderr, "LoneJSON module: %s\n", lua_tostring(state, -1));
  require(status == LUA_OK);
  lua_pop(state, 1);
  luaQ_pushmanaged(state, json_task_id, 0);
  lua_setglobal(state, "json_task_id");
  luaQ_pushmanaged(state, json_fault, 0);
  lua_setglobal(state, "json_fault");
  luaQ_pushmanaged(state, json_oom, 0);
  lua_setglobal(state, "json_oom");
  luaQ_pushmanaged(state, json_checkpoint, 0);
  lua_setglobal(state, "json_checkpoint");
  luaQ_pushmanaged(state, json_stream_callback, 0);
  lua_setglobal(state, "json_stream_callback");
  luaQ_pushmanaged(state, json_accept, 0);
  lua_setglobal(state, "json_accept");
}
static void json_run(struct task *task) {
  task->status = luaTK_resume(&task->root, 0, &task->results);
  if (task->status != LUA_OK && !strstr(json_active->mode, "stop") &&
      !strstr(json_active->mode, "shutdown")) {
    lock(task->pool);
    fprintf(stderr, "LoneJSON task %d failed after %u streamed items\n",
            task->id, json_active->consumed);
    unlock(task->pool);
    require(lua_checkstack(task->state, LUA_MINSTACK));
    luaL_traceback(task->state, task->state, lua_tostring(task->state, -1), 1);
    fprintf(stderr, "LoneJSON task %d traceback: %s\n", task->id,
            lua_tostring(task->state, -1));
    lua_pop(task->state, 1);
    require(task->status == LUA_OK);
  }
}
static int exercise_lonejson(const char *mode) {
  struct json_fixture *test = calloc(1, sizeof(*test));
  struct pool *pool = calloc(1, sizeof(*pool));
  pthread_t workers[MAX_WORKERS];
  const int stream_ids[] = {0, 2, 3, 6}, normal_ids[] = {2, 3};
  int stream = strstr(mode, "stream") != NULL;
  int stopped = strstr(mode, "stop") != NULL;
  int shutdown = strstr(mode, "shutdown") != NULL;
  const int *ids = stream ? stream_ids : normal_ids;
  unsigned count = stream ? 4u : 2u;
  require(test != NULL && pool != NULL);
  fixture = pool;
  json_active = test;
  test->pool = pool;
  test->mode = mode;
  test->input[0] = test->input[1] = -1;
  memset(test->payload, 'x', JSON_PAYLOAD);
  pool->managed_limit = 2;
  check(pthread_mutex_init(&pool->mutex, NULL));
  check(pthread_cond_init(&pool->changed, NULL));
  if (strstr(mode, "facade")) {
    static const unsigned char capture[] = "require('fixture')";
    luthrum_lua_runtime_allocator_config config = {0};
    config.user = pool;
    config.alloc_fn = json_host_allocate;
    config.realloc_fn = json_allocate;
    config.free_fn = json_host_free;
    config.max_bytes = JSON_HEAP_LIMIT;
    require(luthrum_lua_runtime_new_with_allocator(&pool->runtime, &config) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_openlibs(pool->runtime) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_register_c_module(pool->runtime, "fixture",
                                                  open_fixture) ==
            LUTHRUM_LUA_RUNTIME_OK);
    require(luthrum_lua_runtime_run_buffer(pool->runtime, capture,
                                           sizeof(capture) - 1, "lonejson", 0,
                                           NULL, 0) == LUTHRUM_LUA_RUNTIME_OK);
  } else {
    pool->main = lua_newstate(json_allocate, pool, 0);
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
  json_globals(pool->main);
  if (stream) {
    require(pipe(test->input) == 0);
    luaP_newpipe(pool->main, 1);
    lua_setglobal(pool->main, "q");
    lua_pushinteger(pool->main, test->input[0]);
    lua_setglobal(pool->main, "json_input_fd");
    lua_pushinteger(pool->main, JSON_PAYLOAD);
    lua_setglobal(pool->main, "json_payload_size");
    lua_pushcfunction(pool->main, block_binding);
    lua_setglobal(pool->main, "block");
  }
  for (unsigned n = 0; n < count; ++n) {
    int id = ids[n];
    struct task *task = &pool->tasks[id];
    task->state = lua_newthread(pool->main);
    if (id == 2 || (!stream && id == 3))
      require(luaL_loadfile(task->state, stream ? LUTHRUM_LONEJSON_STREAM_SCRIPT
                                                : LUTHRUM_LONEJSON_SCRIPT) ==
              LUA_OK);
    else
      require(
          luaL_loadstring(
              task->state,
              id == 0 ? "return block(...)"
              : id == 3
                  ? "while true do local s=q:recv(); if s==nil then break end; "
                    "json_accept(s) end; return 42"
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
      task->run = json_run;
    task->target = stream ? (id == 0 ? 0 : id == 2 ? 2 : 3) : -1;
    lua_settop(pool->main, 0);
  }
  for (int n = 0; n < MAX_WORKERS; ++n) {
    pool->workers[n].pool = pool;
    check(pthread_create(&workers[n], NULL, worker_entry, &pool->workers[n]));
  }
  if (!stream) {
    submit(pool, 2);
    submit(pool, 3);
    lock(pool);
    while (test->arrived != 2)
      wait_change(pool);
    require(pool->native == 2);
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
    if (strstr(mode, "migrate"))
      pool->force_migrate = pool->stop_waiting = 1;
    pool->release[0] = 1;
    signal_change(pool);
    unlock(pool);
  } else {
    submit(pool, 0);
    lock(pool);
    while (pool->entered != 1)
      wait_change(pool);
    unlock(pool);
    submit(pool, 2);
    fprintf(stderr, "LoneJSON stream: waiting for native read\n");
    lock(pool);
    while (test->reads == 0)
      wait_change(pool);
    require(pool->native == 2);
    unlock(pool);
    json_write_record(test, 1);
    json_write(test, ",",
               1); /* item delimiter, with the array still incomplete */
    fprintf(stderr, "LoneJSON stream: waiting for sink backpressure\n");
    lock(pool);
    while (pool->tasks[2].pinned_resource_waits == 0)
      wait_change(pool);
    require(pool->native == 2 && !pool->release[0]);
    unlock(pool);
    lua_gc(pool->main, LUA_GCCOLLECT);
    lua_gc(pool->main, LUA_GCCOLLECT);
    submit(pool, 6);
    lock(pool);
    while (!pool->tasks[6].finished)
      wait_change(pool);
    require(pool->native == 2 && !pool->release[0]);
    unlock(pool);
    submit(pool, 3);
    fprintf(stderr,
            "LoneJSON stream: waiting for first output and next read\n");
    lock(pool);
    while (test->consumed == 0 || test->reads < 2)
      wait_change(pool);
    require(test->consumed == 1 && !pool->tasks[2].finished &&
            pool->native == 2);
    unlock(pool);
    if (stopped) {
      int status = -99, results = -99;
      require(luaTK_requeststop(&pool->tasks[2].root) == LUA_OK);
      require(luaTK_collect(pool->main, &pool->tasks[2].root, &status,
                            &results) == LUA_TK_PENDING);
      require(status == -99 && results == -99);
      lua_gc(pool->main, LUA_GCCOLLECT);
    } else {
      for (unsigned n = 2; n <= JSON_RECORDS; ++n)
        json_write_record(test, n);
      json_write(test, "]", 1);
    }
    require(close(test->input[1]) == 0);
    test->input[1] = -1;
    lock(pool);
    while (!pool->tasks[2].finished)
      wait_change(pool);
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
    wait_change(pool);
  require(pool->native == 0 && pool->managed == 0 && pool->native_peak == 2 &&
          pool->managed_peak <= 2);
  pool->stop = 1;
  signal_change(pool);
  unlock(pool);
  for (int n = 0; n < MAX_WORKERS; ++n)
    check(pthread_join(workers[n], NULL));
  for (unsigned n = 0; n < count; ++n) {
    int id = ids[n], status = -99, results = -99;
    int failed = shutdown || (stopped && id == 2);
    require(luaTK_collect(pool->main, &pool->tasks[id].root, &status,
                          &results) == LUA_OK);
    if (status != (failed ? LUA_ERRRUN : LUA_OK))
      fprintf(stderr, "LoneJSON task %d: %s\n", id,
              lua_tostring(pool->main, -1));
    require(status == (failed ? LUA_ERRRUN : LUA_OK) && results == 1);
    if (failed)
      require(strstr(lua_tostring(pool->main, -1), "task stopped") != NULL);
    else if (id == 2 && stream)
      require(lua_tointeger(pool->main, -1) == JSON_RECORDS);
    else if (id != 0)
      require(lua_tointeger(pool->main, -1) == 42);
    lua_pop(pool->main, 1);
    test_fiber_destroy(&pool->tasks[id].fiber);
  }
  if (strstr(mode, "migrate"))
    require(pool->tasks[2].migrations + pool->tasks[3].migrations != 0);
  if (strstr(mode, "oom"))
    require(test->denied == 2 && test->deny == 0);
  if (stream) {
    require(test->consumed == (stopped ? 1u : JSON_RECORDS) &&
            test->phase == (stopped ? 2u : 3u));
    if (!stopped)
      require(test->input_bytes > JSON_HEAP_LIMIT &&
              test->output_bytes > JSON_HEAP_LIMIT);
    require(pool->tasks[2].pinned_resource_waits != 0 && test->callbacks != 0);
  }
  require(G(pool->main)->tasks == NULL && pool->stream_peak <= JSON_HEAP_LIMIT);
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
  json_active = NULL;
  free(pool);
  free(test);
  return 0;
}
