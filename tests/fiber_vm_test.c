#define _GNU_SOURCE
#include "private/context.h"
#include <fenv.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lroots.h>
#include <lstring.h>
#include <lua.h>
#include <lualib.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum { TASKS = 8, CARRIERS = 2 };
enum task_state { RUNNABLE, RUNNING, FINISHED };
struct pool;
struct task {
  struct pool *pool;
  struct test_fiber fiber;
  lua_State *state;
  enum task_state status;
  LuaWorldScope world;
  uintptr_t parked_stack;
  int id;
  int worker;
  int target;
  int last_worker;
  int first_park;
  int terminal;
  int lua_status;
  int results;
  int rounding;
  unsigned polls;
  unsigned migrations;
  unsigned native_skips;
  unsigned native_checks;
};
struct worker {
  struct pool *pool;
  struct test_context context;
  int id;
  int cpu;
  unsigned dispatches;
};
struct pool {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_barrier_t managed;
  struct task tasks[TASKS];
  struct worker workers[CARRIERS];
  lua_State *main;
  int parked;
  int run;
  int done;
  int stop;
  size_t live_bytes;
  size_t allocations;
  pthread_t failing_carrier;
  unsigned failures_left;
  unsigned failures;
};
static __thread int native_marker;

static void verify_rounding(int expected) {
  unsigned mxcsr;
  unsigned short control;
  __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
  __asm__ volatile("fnstcw %0" : "=m"(control));
  if ((control & 0xc00U) != (unsigned)expected ||
      ((mxcsr >> 3) & 0xc00U) != (unsigned)expected)
    abort();
}

static void check(int status) {
  if (status != 0)
    abort();
}

static void *allocate(void *user, void *pointer, size_t old_size, size_t size) {
  struct pool *pool = user;
  void *replacement;
  int reject;
  if (pointer == NULL)
    old_size = 0;
  if (size == 0) {
    pool->live_bytes -= old_size;
    free(pointer);
    return NULL;
  }
  check(pthread_mutex_lock(&pool->mutex));
  reject = pool->failures_left != 0 && size > old_size &&
           pthread_equal(pool->failing_carrier, pthread_self());
  if (reject) {
    pool->failures_left--;
    pool->failures++;
  }
  check(pthread_mutex_unlock(&pool->mutex));
  if (reject)
    return NULL;
  replacement = malloc(size);
  if (replacement != NULL) {
    if (pointer != NULL) {
      memcpy(replacement, pointer, old_size < size ? old_size : size);
      free(pointer);
    }
    pool->live_bytes = pool->live_bytes - old_size + size;
    pool->allocations++;
  }
  return replacement;
}

static int migratable(lua_State *state) {
  CallInfo *frame;
  if (state->native_depth != 0 || state->world_depth != 0)
    return 0;
  for (frame = state->ci; frame != &state->base_ci; frame = frame->previous)
    if (!isLua(frame))
      return 0;
  return 1;
}

static void poll(lua_State *state, void *user) {
  struct task *task = user;
  verify_rounding(task->rounding);
  if (!migratable(state)) {
    task->native_skips++;
    return;
  }
  if (task->last_worker >= 0 && task->last_worker != task->worker)
    task->migrations++;
  task->last_worker = task->worker;
  task->polls++;
  if (task->polls == 1) {
    int status;
    task->parked_stack = (uintptr_t)state->stack.p;
    if (task->id < CARRIERS) {
      /* These two dispatchers retain their respective state
       * gates/participation. */
      status = pthread_barrier_wait(&task->pool->managed);
      if (status != 0 && status != PTHREAD_BARRIER_SERIAL_THREAD)
        abort();
    }
  }
  task->world = luaW_detach(state);
  check(test_register_pause(&task->fiber));
  luaW_attach(state, task->world);
  luaD_rebindcarrier(state);
  if (state->luthrum_stack_context != &task->fiber.native)
    abort();
  verify_rounding(task->rounding);
}

static int pinned(lua_State *state) {
  struct task *task = state->state_poll_context;
  long carrier = syscall(SYS_gettid);
  native_marker = task->id + 1;
  lua_pushvalue(state, 1);
  if (lua_pcall(state, 0, 1, 0) != LUA_OK ||
      lua_tointeger(state, -1) != 8002000 || syscall(SYS_gettid) != carrier ||
      native_marker != task->id + 1)
    abort();
  task->native_checks++;
  return 1;
}

static int fail_allocation(lua_State *state) {
  struct task *task = state->state_poll_context;
  check(pthread_mutex_lock(&task->pool->mutex));
  task->pool->failing_carrier = pthread_self();
  task->pool->failures_left = 2;
  check(pthread_mutex_unlock(&task->pool->mutex));
  (void)lua_newuserdatauv(state, 4096, 0);
  abort();
}

static void task_entry(void *user) {
  struct task *task = user;
  LuaNativeRoot root;
  TValue value;
  Udata *sentinel;
  unsigned *payload;
  check(fesetround(task->rounding));
  /* This object has no Lua stack/registry reference; its only root is on the
   * retained native stack through collection, migration and error recovery. */
  luaX_lockstate(task->state);
  luaR_pushroot(task->state, &root);
  sentinel = luaS_newudata(task->state, 2 * sizeof(unsigned), 0);
  payload = (unsigned *)getudatamem(sentinel);
  payload[0] = 0x1234abcdU;
  payload[1] = (unsigned)task->id;
  setuvalue(task->state, &value, sentinel);
  luaR_setroot(task->state, &root, &value);
  luaX_unlockstate(task->state);
  task->lua_status = lua_resume(task->state, NULL, 1, &task->results);
  luaX_lockstate(task->state);
  payload = (unsigned *)getudatamem(uvalue(&root.value));
  if (task->state->native_roots != &root || payload[0] != 0x1234abcdU ||
      payload[1] != (unsigned)task->id)
    abort();
  luaR_poproot(task->state, &root);
  luaX_unlockstate(task->state);
  verify_rounding(task->rounding);
  if (task->state->native_depth != 0 || task->state->native_roots != NULL ||
      task->state->object_scopes != NULL || task->fiber.native.overflow != 0)
    abort();
  task->terminal = 1;
  test_fiber_pause(&task->fiber);
  abort();
}

static void *worker_entry(void *user) {
  struct worker *worker = user;
  struct pool *pool = worker->pool;
  unsigned cursor = 0;
  int original_rounding = fegetround();
  if (worker->cpu >= 0) {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    CPU_SET(worker->cpu, &allowed);
    check(pthread_setaffinity_np(pthread_self(), sizeof(allowed), &allowed));
  }
  for (;;) {
    struct task *task = NULL;
    unsigned searched;
    check(pthread_mutex_lock(&pool->mutex));
    while (!pool->stop && task == NULL) {
      for (searched = 0; searched < TASKS; ++searched) {
        struct task *candidate = &pool->tasks[(cursor + searched) % TASKS];
        if (candidate->status == RUNNABLE && candidate->target == worker->id &&
            (pool->run || !candidate->first_park)) {
          task = candidate;
          cursor = (cursor + searched + 1) % TASKS;
          task->status = RUNNING;
          task->worker = worker->id;
          break;
        }
      }
      if (task == NULL)
        check(pthread_cond_wait(&pool->changed, &pool->mutex));
    }
    if (pool->stop) {
      check(pthread_mutex_unlock(&pool->mutex));
      break;
    }
    check(pthread_mutex_unlock(&pool->mutex));
    worker->dispatches++;
    test_fiber_resume(&task->fiber, &worker->context);
    verify_rounding(original_rounding);
    if (luaE_getnativestackcontext() != &luaE_native_stack_context)
      abort();
    /* Publish only after the complete context has returned to this carrier. */
    check(pthread_mutex_lock(&pool->mutex));
    if (task->terminal) {
      task->status = FINISHED;
      if (++pool->done == TASKS)
        pool->stop = 1;
    } else {
      if (!task->first_park) {
        task->first_park = 1;
        pool->parked++;
      }
      task->target = 1 - worker->id;
      task->status = RUNNABLE;
    }
    check(pthread_cond_broadcast(&pool->changed));
    check(pthread_mutex_unlock(&pool->mutex));
  }
  return NULL;
}

static int exercise_vm(int generational) {
  static const char source[] =
      "local token=7; return function(id) "
      "if id<2 then while shared.ready~=true do end end; "
      "local sum=0; for i=1,1500 do "
      "local t={i,id}; sum=sum+t[1]; shared[id]=sum; "
      "if id%2==0 then token=i%2==0 and 7 or 'word' end; "
      "local v=token; assert(v==7 or v=='word') end; "
      "assert(pinned(function() local n=0; for i=1,4000 do n=n+i end; "
      "return n end)==8002000); "
      "if id==6 then shared.ready=true end; "
      "if id==2 then error('expected migrated task error') end; "
      "if id==3 then fail_allocation() end; "
      "if id==7 then local t; t=setmetatable({}, {__index=function(self,k) "
      "return self[k]+1 end}); return t.missing end; return sum,id end";
  struct pool *pool = calloc(1, sizeof(*pool));
  pthread_t carriers[CARRIERS];
  cpu_set_t allowed;
  int cpus[CARRIERS] = {-1, -1};
  int found = 0, i, failed = 0;
  if (pool == NULL)
    abort();
  check(pthread_mutex_init(&pool->mutex, NULL));
  check(pthread_cond_init(&pool->changed, NULL));
  check(pthread_barrier_init(&pool->managed, NULL, CARRIERS));
  pool->main = lua_newstate(allocate, pool, 0);
  if (pool->main == NULL)
    abort();
  luaL_openlibs(pool->main);
  lua_pushcfunction(pool->main, pinned);
  lua_setglobal(pool->main, "pinned");
  lua_pushcfunction(pool->main, fail_allocation);
  lua_setglobal(pool->main, "fail_allocation");
  lua_newtable(pool->main);
  lua_setglobal(pool->main, "shared");
  if (generational)
    (void)lua_gc(pool->main, LUA_GCGEN);
  (void)lua_gc(pool->main, LUA_GCSTOP);
  if (luaL_loadstring(pool->main, source) != LUA_OK ||
      lua_pcall(pool->main, 0, 1, 0) != LUA_OK)
    abort();
  for (i = 0; i < TASKS; ++i) {
    struct task *task = &pool->tasks[i];
    task->pool = pool;
    task->id = i;
    task->target = i % CARRIERS;
    task->last_worker = -1;
    task->rounding = i % 2 ? FE_DOWNWARD : FE_UPWARD;
    task->state = lua_newthread(pool->main);
    lua_pushvalue(pool->main, 1);
    lua_xmove(pool->main, task->state, 1);
    lua_pushinteger(task->state, i);
    luaX_lockstate(task->state);
    if (!luaD_growstack(task->state, 4096, 0))
      abort();
    luaX_unlockstate(task->state);
    task->state->state_poll = poll;
    task->state->state_poll_context = task;
    task->state->poll_remaining = 1;
    check(test_fiber_create(&task->fiber, task_entry, task));
  }
  if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0)
    for (i = 0; i < CPU_SETSIZE && found < CARRIERS; ++i)
      if (CPU_ISSET(i, &allowed))
        cpus[found++] = i;
  for (i = 0; i < CARRIERS; ++i) {
    pool->workers[i].pool = pool;
    pool->workers[i].id = i;
    pool->workers[i].cpu = cpus[i];
    check(pthread_create(&carriers[i], NULL, worker_entry, &pool->workers[i]));
  }
  check(pthread_mutex_lock(&pool->mutex));
  while (pool->parked != TASKS)
    check(pthread_cond_wait(&pool->changed, &pool->mutex));
  check(pthread_mutex_unlock(&pool->mutex));
  (void)lua_gc(pool->main, LUA_GCCOLLECT);
  for (i = 0; i < TASKS; ++i)
    if (pool->tasks[i].state->native_depth != 1 ||
        (uintptr_t)pool->tasks[i].state->stack.p == pool->tasks[i].parked_stack)
      abort();
  (void)lua_gc(pool->main, LUA_GCRESTART);
  check(pthread_mutex_lock(&pool->mutex));
  pool->run = 1;
  check(pthread_cond_broadcast(&pool->changed));
  check(pthread_mutex_unlock(&pool->mutex));
  for (i = 0; i < CARRIERS; ++i)
    check(pthread_join(carriers[i], NULL));
  for (i = 0; i < TASKS; ++i) {
    struct task *task = &pool->tasks[i];
    int expected =
        i == 3 ? LUA_ERRMEM : (i == 2 || i == 7 ? LUA_ERRRUN : LUA_OK);
    failed |= task->lua_status != expected || task->migrations == 0 ||
              task->migrations + 1 != task->polls || task->polls < 2 ||
              task->native_skips == 0 || task->native_checks != 1;
    if (task->lua_status == LUA_OK)
      failed |= task->results != 2 ||
                lua_tointeger(task->state, -2) != 1125750 ||
                lua_tointeger(task->state, -1) != i;
    else if (i == 2)
      failed |= strstr(lua_tostring(task->state, -1),
                       "expected migrated task error") == NULL;
    else if (i == 7)
      failed |=
          strstr(lua_tostring(task->state, -1), "C stack overflow") == NULL;
    if (failed)
      fprintf(stderr,
              "task %d: status=%d polls=%u migrations=%u native skips=%u "
              "checks=%u\n",
              i, task->lua_status, task->polls, task->migrations,
              task->native_skips, task->native_checks);
    task->state->state_poll = NULL;
    task->state->state_poll_context = NULL;
    (void)lua_closethread(task->state, NULL);
    lua_settop(task->state, 0);
    if (luaL_loadstring(task->state, "return 42") != LUA_OK ||
        lua_resume(task->state, NULL, 0, &task->results) != LUA_OK ||
        lua_tointeger(task->state, -1) != 42 ||
        task->state->luthrum_stack_context == &task->fiber.native)
      abort();
    test_fiber_destroy(&task->fiber);
  }
  failed |= pool->failures != 2 || pool->failures_left != 0;
  for (i = 0; i < CARRIERS; ++i)
    failed |= pool->workers[i].dispatches <= TASKS / CARRIERS;
  lua_getglobal(pool->main, "shared");
  for (i = 0; i < TASKS; ++i) {
    lua_rawgeti(pool->main, -1, i);
    failed |= lua_tointeger(pool->main, -1) != 1125750;
    lua_pop(pool->main, 1);
  }
  lua_close(pool->main);
  failed |= pool->live_bytes != 0;
  check(pthread_barrier_destroy(&pool->managed));
  check(pthread_cond_destroy(&pool->changed));
  check(pthread_mutex_destroy(&pool->mutex));
  free(pool);
  if (!failed)
    printf("fiber VM: eight tasks, two reused carriers, forced migration and "
           "recovery\n");
  return failed;
}

static int exercise_guards(void) {
  struct test_fiber fiber;
  struct rlimit cores = {0, 0};
  int side;
  check(test_fiber_create(&fiber, NULL, NULL));
  check(setrlimit(RLIMIT_CORE, &cores));
  for (side = 0; side < 2; ++side) {
    pid_t child = fork();
    int status;
    if (child == 0) {
      volatile char *guard =
          side ? fiber.stack + fiber.stack_size : fiber.stack - 1;
      *guard = 1;
      _exit(0);
    }
    if (child < 0 || waitpid(child, &status, 0) != child ||
        !WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV)
      abort();
  }
  test_fiber_destroy(&fiber);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  if (strcmp(argv[1], "guards") == 0)
    return exercise_guards();
  if (strcmp(argv[1], "incremental") == 0)
    return exercise_vm(0);
  if (strcmp(argv[1], "generational") == 0)
    return exercise_vm(1);
  return 2;
}
