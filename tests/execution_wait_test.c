#define _GNU_SOURCE
#include <lexec.h>
#include <lstate.h>
#include <pthread.h>
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
struct actor {
  struct fixture *fixture;
  LuaExecution execution;
  unsigned entries, leaves, wakes;
  int kind, ready;
};
struct fixture {
  pthread_mutex_t mutex;
  pthread_cond_t changed, observed, scheduled;
  pthread_barrier_t early_enter, early_release;
  LuaWaiter *waiters;
  struct actor actors[2];
  unsigned entered, finished, broadcasts;
  int released, early;
};
static struct fixture *active;

int __real_pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int __real_pthread_cond_broadcast(pthread_cond_t *);
int __wrap_pthread_cond_wait(pthread_cond_t *condition,
                             pthread_mutex_t *mutex) {
  if (condition == &active->changed) {
    require(mutex == &active->mutex);
    active->entered++;
    require(pthread_cond_broadcast(&active->observed) == 0);
  }
  return __real_pthread_cond_wait(condition, mutex);
}
int __wrap_pthread_cond_broadcast(pthread_cond_t *condition) {
  if (condition == &active->changed)
    active->broadcasts++;
  return __real_pthread_cond_broadcast(condition);
}
static void meet(pthread_barrier_t *barrier) {
  int result = pthread_barrier_wait(barrier);
  require(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
}
static void enter(LuaExecution *execution, lua_State *state) {
  struct actor *actor = execution->context;
  require(state == NULL);
  actor->entries++;
}
static void leave(LuaExecution *execution) {
  struct actor *actor = execution->context;
  actor->leaves++;
}
static void wait_scheduled(LuaExecution *execution, pthread_mutex_t *mutex,
                           LuaWaiter *waiter) {
  struct actor *actor = execution->context;
  struct fixture *test = actor->fixture;
  require(mutex == &test->mutex && waiter->execution == execution &&
          waiter->link != NULL && execution->managed_depth == 0);
  test->entered++;
  require(pthread_cond_broadcast(&test->observed) == 0);
  if (test->early) {
    require(pthread_mutex_unlock(mutex) == 0);
    meet(&test->early_enter);
    meet(&test->early_release);
    require(pthread_mutex_lock(mutex) == 0);
  }
  while (!actor->ready)
    require(pthread_cond_wait(&test->scheduled, mutex) == 0);
  require(waiter->link == NULL);
  actor->ready = 0;
  require(pthread_mutex_unlock(mutex) == 0);
}
static void wake_scheduled(LuaExecution *execution, LuaWaiter *waiter) {
  struct actor *actor = execution->context;
  require(waiter->execution == execution && waiter->link == NULL &&
          !actor->ready);
  actor->wakes++;
  actor->ready = 1;
  require(pthread_cond_broadcast(&actor->fixture->scheduled) == 0);
}
static void *worker(void *context) {
  struct actor *actor = context;
  struct fixture *test = actor->fixture;
  NativeStackContext native = {0};
  NativeStackContext *previous = luaE_setnativestackcontext(&native);
  if (actor->kind != 0) {
    native.execution = &actor->execution;
    actor->execution.context = actor;
    if (actor->kind == 2) {
      actor->execution.managed_depth = 1;
      actor->execution.managed_enter = enter;
      actor->execution.managed_leave = leave;
      actor->execution.wait = wait_scheduled;
      actor->execution.wake = wake_scheduled;
    }
  }
  require(pthread_mutex_lock(&test->mutex) == 0);
  while (!test->released)
    luaQ_wait(NULL, &test->mutex, &test->changed, &test->waiters);
  require(actor->kind != 2 || actor->execution.managed_depth == 1);
  test->finished++;
  require(pthread_cond_broadcast(&test->observed) == 0);
  require(pthread_mutex_unlock(&test->mutex) == 0);
  luaE_setnativestackcontext(previous);
  return NULL;
}
int main(int argc, char **argv) {
  struct fixture *test = calloc(1, sizeof(*test));
  pthread_t threads[2];
  unsigned count = 2, expected;
  require(test != NULL && argc == 2);
  active = test;
  require(pthread_mutex_init(&test->mutex, NULL) == 0);
  require(pthread_cond_init(&test->changed, NULL) == 0);
  require(pthread_cond_init(&test->observed, NULL) == 0);
  require(pthread_cond_init(&test->scheduled, NULL) == 0);
  require(pthread_barrier_init(&test->early_enter, NULL, 2) == 0);
  require(pthread_barrier_init(&test->early_release, NULL, 2) == 0);
  if (strcmp(argv[1], "empty") == 0) {
    require(pthread_mutex_lock(&test->mutex) == 0);
    for (unsigned n = 0; n < 10000; ++n)
      luaQ_wake(&test->changed, &test->waiters);
    require(test->broadcasts == 0 && test->waiters == NULL);
    require(pthread_mutex_unlock(&test->mutex) == 0);
    count = 0;
  } else {
    int kind = strcmp(argv[1], "fallback") == 0  ? 1
               : strcmp(argv[1], "managed") == 0 ? 2
                                                 : 0;
    test->early = strcmp(argv[1], "early") == 0;
    if (test->early)
      count = 1;
    for (unsigned n = 0; n < count; ++n) {
      test->actors[n].fixture = test;
      test->actors[n].kind = test->early ? 2 : kind;
      if (strcmp(argv[1], "mixed") == 0 && n == 1)
        test->actors[n].kind = 2;
      require(pthread_create(&threads[n], NULL, worker, &test->actors[n]) == 0);
    }
    if (test->early)
      meet(&test->early_enter);
    require(pthread_mutex_lock(&test->mutex) == 0);
    while (test->entered != count)
      require(pthread_cond_wait(&test->observed, &test->mutex) == 0);
    expected = 0;
    for (LuaWaiter *waiter = test->waiters; waiter; waiter = waiter->next)
      expected++;
    require(expected == count && test->finished == 0);
    if (strcmp(argv[1], "spurious") == 0) {
      require(pthread_cond_broadcast(&test->changed) == 0);
      while (test->entered != 2 * count)
        require(pthread_cond_wait(&test->observed, &test->mutex) == 0);
      require(test->waiters != NULL && test->finished == 0);
      expected = 0;
      for (LuaWaiter *waiter = test->waiters; waiter; waiter = waiter->next)
        require(++expected <= count);
      require(expected == count);
    }
    test->released = 1;
    if (strcmp(argv[1], "signal") == 0) {
      require(pthread_cond_broadcast(&test->changed) == 0);
      require(test->waiters != NULL);
    } else {
      luaQ_wake(&test->changed, &test->waiters);
      require(test->waiters == NULL);
    }
    require(test->broadcasts == (strcmp(argv[1], "spurious") == 0 ? 2u : 1u));
    require(pthread_mutex_unlock(&test->mutex) == 0);
    if (test->early)
      meet(&test->early_release);
    for (unsigned n = 0; n < count; ++n) {
      require(pthread_join(threads[n], NULL) == 0);
      if (test->actors[n].kind == 2)
        require(test->actors[n].entries == 1 && test->actors[n].leaves == 1 &&
                test->actors[n].wakes == 1);
    }
    require(test->finished == count && test->waiters == NULL);
  }
  require(pthread_barrier_destroy(&test->early_enter) == 0);
  require(pthread_barrier_destroy(&test->early_release) == 0);
  require(pthread_cond_destroy(&test->scheduled) == 0);
  require(pthread_cond_destroy(&test->observed) == 0);
  require(pthread_cond_destroy(&test->changed) == 0);
  require(pthread_mutex_destroy(&test->mutex) == 0);
  free(test);
  return 0;
}
