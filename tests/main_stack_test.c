#define _GNU_SOURCE
#include <lauxlib.h>
#include <lstate.h>
#include <lualib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define require(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      abort();                                                                 \
    }                                                                          \
  } while (0)

static int worker_bounds(void) {
  pthread_attr_t attributes;
  void *base;
  size_t size;
  require(pthread_getattr_np(pthread_self(), &attributes) == 0);
  require(pthread_attr_getstack(&attributes, &base, &size) == 0);
  require(pthread_attr_destroy(&attributes) == 0);
  require(size > 32768);
  require(luaE_initnativestack() == (uintptr_t)base + 32768);
  return 0;
}

static void *worker(void *argument) {
  int fork_worker = *(int *)argument;
  if (fork_worker) {
    int status;
    pid_t child = fork();
    require(child >= 0);
    if (child == 0)
      _exit(worker_bounds());
    require(waitpid(child, &status, 0) == child);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  } else
    (void)worker_bounds();
  return NULL;
}

int main(int argc, char **argv) {
  require(argc == 2);
  if (strcmp(argv[1], "worker") == 0 || strcmp(argv[1], "fork_worker") == 0) {
    pthread_t thread;
    int fork_worker = strcmp(argv[1], "fork_worker") == 0;
    require(pthread_create(&thread, NULL, worker, &fork_worker) == 0);
    require(pthread_join(thread, NULL) == 0);
  } else {
    lua_State *state;
    if (strcmp(argv[1], "limit") == 0) {
      struct rlimit limit;
      require(getrlimit(RLIMIT_STACK, &limit) == 0);
      if (limit.rlim_cur > 512 * 1024) {
        limit.rlim_cur = 512 * 1024;
        require(setrlimit(RLIMIT_STACK, &limit) == 0);
      }
    } else
      require(strcmp(argv[1], "main") == 0);
    state = luaL_newstate();
    require(state != NULL);
    luaL_openlibs(state);
    require(luaL_dostring(state,
                          "local function pipeline(n) if n==0 then return "
                          "coroutine.wrap(function(v) return v end) end; "
                          "local previous=pipeline(n-1); return "
                          "coroutine.wrap(function(v) return previous(v)+1 "
                          "end) end; assert(pipeline(30)(12)==42)") == LUA_OK);
    require(luaL_dostring(state,
                          "local function nested() return "
                          "coroutine.wrap(nested)() end; "
                          "local ok,err=pcall(nested); assert(not ok and "
                          "err:find('C stack overflow',1,true)); "
                          "assert(math.abs(-42)==42)") == LUA_OK);
    lua_close(state);
  }
  return 0;
}
