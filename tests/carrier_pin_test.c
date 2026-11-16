#include <lauxlib.h>
#include <ldo.h>
#include <ltask.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define require(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                  \
      abort();                                                                 \
    }                                                                          \
  } while (0)
struct fixture {
  LuaExecution execution;
  LuaCarrierPin outer, inner;
  unsigned bound, released;
  int accept;
};
static int bind_carrier(LuaExecution *execution) {
  struct fixture *test = execution->context;
  require(execution->pin_depth == 0);
  test->bound++;
  return test->accept;
}
static void unbind_carrier(LuaExecution *execution) {
  struct fixture *test = execution->context;
  require(execution->pin_depth == 0);
  test->released++;
}
static void *foreign(void *context) {
  struct fixture *test = context;
  NativeStackContext native = *luaE_getnativestackcontext(), *previous;
  LuaCarrierPin other = {0};
  require(!luaQ_pinowner(&test->outer));
  require(luaQ_unpincarrier(&test->outer) == LUA_PIN_INVALID);
  native.execution = &test->execution;
  previous = luaE_setnativestackcontext(&native);
  require(!luaQ_pinowner(&test->outer));
  require(luaQ_unpincarrier(&test->outer) == LUA_PIN_INVALID);
  if (test->execution.pin_depth != 0)
    require(luaQ_pincarrier(&other) == LUA_PIN_INVALID && !other.active);
  luaE_setnativestackcontext(previous);
  return NULL;
}
static void *run(void *context) {
  struct fixture *test = context;
  NativeStackContext native = *luaE_getnativestackcontext(), *previous;
  LuaExecution other = {0};
  LuaCarrierPin spare = {0};
  pthread_t thread;
  lua_State *state, *child;
  LuaTaskRoot task = {0};
  require(luaQ_pincarrier(&test->outer) == LUA_OK);
  require(luaQ_pinowner(&test->outer));
  require(pthread_create(&thread, NULL, foreign, test) == 0);
  require(pthread_join(thread, NULL) == 0);
  require(luaQ_unpincarrier(&test->outer) == LUA_OK);
  require(!luaQ_pinowner(&test->outer));
  require(luaQ_unpincarrier(&test->outer) == LUA_PIN_INVALID);
  native.execution = &test->execution;
  previous = luaE_setnativestackcontext(&native);
  require(luaQ_pincarrier(&test->outer) == LUA_PIN_UNAVAILABLE);
  test->execution.pin_carrier = bind_carrier;
  require(luaQ_pincarrier(&test->outer) == LUA_PIN_UNAVAILABLE);
  require(test->bound == 0 && !test->outer.active);
  test->execution.unpin_carrier = unbind_carrier;
  require(luaQ_pincarrier(&test->outer) == LUA_PIN_UNAVAILABLE);
  require(test->bound == 1 && test->released == 0 && !test->outer.active &&
          test->execution.pin_depth == 0);
  test->accept = 1;
  require(luaQ_pincarrier(&test->outer) == LUA_OK);
  require(luaQ_pincarrier(&test->outer) == LUA_PIN_INVALID);
  require(luaQ_pincarrier(&test->inner) == LUA_OK);
  require(test->bound == 2 && test->execution.pin_depth == 2 &&
          test->released == 0);
  /* Logical execution identity is required even on the same physical thread. */
  native.execution = &other;
  require(!luaQ_pinowner(&test->outer));
  require(luaQ_unpincarrier(&test->outer) == LUA_PIN_INVALID);
  native.execution = &test->execution;
  require(pthread_create(&thread, NULL, foreign, test) == 0);
  require(pthread_join(thread, NULL) == 0);
  require(test->execution.pin_depth == 2 && test->released == 0);
  test->execution.pin_depth = ~0u;
  require(luaQ_pincarrier(&spare) == LUA_PIN_OVERFLOW && !spare.active);
  test->execution.pin_depth = 2;
  /* A different task cannot adopt an execution with live affinity regions. */
  state = luaL_newstate();
  require(state != NULL);
  child = lua_newthread(state);
  require(luaL_loadstring(child, "return 42") == LUA_OK);
  require(luaTK_admit(child, &task, &test->execution) == LUA_TK_INVALID);
  require(task.global == NULL && child->task_owner == NULL);
  /* Independent resources can close in either order. */
  require(luaQ_unpincarrier(&test->outer) == LUA_OK);
  require(luaQ_pinowner(&test->inner) && test->execution.pin_depth == 1 &&
          test->released == 0);
  require(luaQ_unpincarrier(&test->inner) == LUA_OK);
  require(test->execution.pin_depth == 0 && test->released == 1);
  lua_close(state);
  require(luaQ_pincarrier(&spare) == LUA_OK);
  require(luaQ_unpincarrier(&spare) == LUA_OK);
  require(test->bound == 3 && test->released == 2);
  luaE_setnativestackcontext(previous);
  return NULL;
}
static void forbidden_native(LuaExecution *execution, lua_State *state) {
  (void)execution;
  (void)state;
  _exit(3);
}
static void *wrong_carrier(void *context) {
  NativeStackContext native = *luaE_getnativestackcontext();
  native.execution = context;
  luaE_setnativestackcontext(&native);
  luaQ_beginnative(NULL);
  _exit(2);
}
static void rejected_driver(void) {
  pid_t child = fork();
  int status;
  require(child >= 0);
  if (child == 0) {
    struct fixture test = {0};
    struct rlimit limit = {0, 0};
    NativeStackContext native = *luaE_getnativestackcontext();
    pthread_t thread;
    require(setrlimit(RLIMIT_CORE, &limit) == 0);
    test.accept = 1;
    test.execution.context = &test;
    test.execution.pin_carrier = bind_carrier;
    test.execution.unpin_carrier = unbind_carrier;
    test.execution.native_enter = forbidden_native;
    native.execution = &test.execution;
    luaE_setnativestackcontext(&native);
    require(luaQ_pincarrier(&test.outer) == LUA_OK);
    require(pthread_create(&thread, NULL, wrong_carrier, &test.execution) == 0);
    require(pthread_join(thread, NULL) == 0);
    _exit(1);
  }
  require(waitpid(child, &status, 0) == child);
  require(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
int main(int argc, char **argv) {
  struct fixture test = {0};
  pthread_t worker;
  (void)argv;
  require(argc == 1 || argc == 2);
  if (argc == 2) {
    rejected_driver();
    return 0;
  }
  test.execution.context = &test;
  require(pthread_create(&worker, NULL, run, &test) == 0);
  require(pthread_join(worker, NULL) == 0);
  return 0;
}
