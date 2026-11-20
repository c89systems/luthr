#include <laccess.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lmem.h>
#include <lpolicy.h>
#include <lstate.h>
#include <ltable.h>
#include <ltask.h>
#include <lua.h>
#include <lworld.h>
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

static __thread unsigned measuring, lookups;
static LuaPolicy policy;
static unsigned configurations;

NativeStackContext *__real_luaE_getnativestackcontext(void);
NativeStackContext *__wrap_luaE_getnativestackcontext(void) {
  if (measuring)
    lookups++;
  return __real_luaE_getnativestackcontext();
}

static void configure(LuaPolicy *value, lua_State *state) {
  require(value == &policy && value->global == G(state));
  configurations++;
}

static void release_policy(lua_State *state, LuaPolicy *value) {
  (void)state;
  require(value == &policy && value->references == 0);
}

/* Complete operations must retain coherent values and their invocation policy
 * without rediscovering the ambient thread for every helper. */
static void owned(lua_State *state, int managed) {
  NativeStackContext native = *luaE_getnativestackcontext(), *previous;
  LuaExecution execution;
  LuaPolicyScope scope;
  LuaGCFrame frame;
  Table *table;
  TValue key, value, result;
  unsigned i;
  memset(&execution, 0, sizeof(execution));
  native.execution = managed ? &execution : NULL;
  previous = luaE_setnativestackcontext(&native);
  policy.global = G(state);
  policy.references = 1;
  policy.configure = configure;
  policy.release = release_policy;
  luaCP_begin(&policy, &scope);
  lua_gc(state, LUA_GCSTOP);
  lua_newtable(state);
  lua_pushinteger(state, 0);
  lua_rawseti(state, -2, 1);
  luaX_lockstate(state);
  require(luaX_context(state) == &native);
  require(luaX_execution(state) == (managed ? &execution : NULL));
  require(luaX_token(state) ==
          (managed ? (void *)&execution : (void *)&luaE_thread_context.native));
  if (!managed)
    require(luaX_hostcontext(state) == &luaE_thread_context);
  table = hvalue(s2v(state->top.p - 1));
  setivalue(&key, 1);
  luaC_entergc(state, &frame);
  lookups = 0;
  measuring = 1;
  for (i = 0; i < 2000; i++) {
    LuaObjectScope outer;
    luaA_begin(state, obj2gco(table), &outer);
    require(luaA_tryscan(state, obj2gco(table)));
    luaA_endscan(state, obj2gco(table));
    setivalue(&value, i);
    luaH_assignowned(state, table, &key, &value, 1);
    require(luaH_getint(state, table, 1, &result) == LUA_VNUMINT);
    require(ivalue(&result) == i);
    require(luaC_ingc(state));
    luaTK_checkstop(state);
    luaCP_apply(state);
    luaA_end(state, &outer);
  }
  measuring = 0;
  require(lookups == 0 && configurations >= 2000);
  luaC_leavegc(state, &frame);
  luaX_unlockstate(state);
  require(state->stack_gate.context == NULL && state->stack_gate.word == 0);
  luaCP_end(&scope);
  luaX_lockstate(state);
  luaCP_bind(state, NULL);
  luaX_unlockstate(state);
  luaCP_release(state, &policy);
  luaE_setnativestackcontext(previous);
  require(execution.managed_depth == 0);
}

static void reservations(lua_State *state, int paired) {
  lua_State *other = lua_newthread(state);
  NativeStackContext native = *luaE_getnativestackcontext(), *previous;
  LuaExecution execution;
  int reservation;
  memset(&execution, 0, sizeof(execution));
  native.execution = &execution;
  previous = luaE_setnativestackcontext(&native);
  if (paired)
    luaX_lockpair(state, other);
  else
    luaX_lockstate(state);
  reservation = luaX_tryreserve(other);
  require(reservation == (paired ? 2 : 1));
  require(luaX_context(other) == &native &&
          luaX_execution(other) == &execution);
  require(luaX_token(other) == &execution && luaX_token(state) == &execution);
  luaX_endreserve(other, reservation);
  require((other->stack_gate.context != NULL) == paired);
  require((other->stack_gate.word != 0) == paired);
  require(luaX_execution(state) == &execution);
  if (paired)
    luaX_unlockpair(other, state);
  else
    luaX_unlockstate(state);
  require(state->stack_gate.context == NULL &&
          other->stack_gate.context == NULL);
  require(execution.managed_depth == 0);
  luaE_setnativestackcontext(previous);
}

struct foreign {
  lua_State *state;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  int entered, release;
  unsigned native_entries, native_leaves;
};

static void enter_native(LuaExecution *execution, lua_State *state) {
  struct foreign *test = execution->context;
  require(state == test->state && execution->native_depth == 0);
  test->native_entries++;
}

static void leave_native(LuaExecution *execution, lua_State *state) {
  struct foreign *test = execution->context;
  require(state == test->state && execution->native_depth == 0);
  test->native_leaves++;
}

static void *first_owner(void *user) {
  struct foreign *test = user;
  luaX_lockstate(test->state);
  require(luaX_context(test->state) == &luaE_thread_context.native);
  require(luaX_token(test->state) == &luaE_thread_context.native);
  require(pthread_mutex_lock(&test->mutex) == 0);
  test->entered = 1;
  require(pthread_cond_broadcast(&test->changed) == 0);
  while (!test->release)
    require(pthread_cond_wait(&test->changed, &test->mutex) == 0);
  require(pthread_mutex_unlock(&test->mutex) == 0);
  require(luaX_context(test->state) == &luaE_thread_context.native);
  luaX_unlockstate(test->state);
  return NULL;
}

static void *second_owner(void *user) {
  struct foreign *test = user;
  luaX_lockstate(test->state);
  require(luaX_context(test->state) == &luaE_thread_context.native);
  require(luaX_hostcontext(test->state) == &luaE_thread_context);
  require(luaX_token(test->state) == &luaE_thread_context.native);
  luaX_unlockstate(test->state);
  return NULL;
}

static void foreign(lua_State *state) {
  struct foreign test;
  NativeStackContext native = *luaE_getnativestackcontext(), *previous;
  LuaExecution execution;
  pthread_t first, second;
  memset(&test, 0, sizeof(test));
  memset(&execution, 0, sizeof(execution));
  test.state = state;
  require(pthread_mutex_init(&test.mutex, NULL) == 0);
  require(pthread_cond_init(&test.changed, NULL) == 0);
  require(pthread_create(&first, NULL, first_owner, &test) == 0);
  require(pthread_mutex_lock(&test.mutex) == 0);
  while (!test.entered)
    require(pthread_cond_wait(&test.changed, &test.mutex) == 0);
  require(pthread_mutex_unlock(&test.mutex) == 0);
  /* Native admission belongs to the ambient driver even while another thread
   * owns the supplied Lua state. It cannot consult that state's binding. */
  execution.context = &test;
  execution.native_enter = enter_native;
  execution.native_leave = leave_native;
  native.execution = &execution;
  previous = luaE_setnativestackcontext(&native);
  luaQ_beginnative(state, luaQ_current());
  require(execution.native_depth == 1 && test.native_entries == 1);
  luaQ_endnative(state, luaQ_current());
  require(execution.native_depth == 0 && test.native_leaves == 1);
  luaE_setnativestackcontext(previous);
  require(pthread_create(&second, NULL, second_owner, &test) == 0);
  while ((__atomic_load_n(&state->stack_gate.word, __ATOMIC_ACQUIRE) &
          LUA_GATE_WAITING) == 0)
    sched_yield();
  require(luaX_tryreserve(state) == 0);
  require(pthread_mutex_lock(&test.mutex) == 0);
  test.release = 1;
  require(pthread_cond_broadcast(&test.changed) == 0);
  require(pthread_mutex_unlock(&test.mutex) == 0);
  require(pthread_join(first, NULL) == 0);
  require(pthread_join(second, NULL) == 0);
  require(state->stack_gate.context == NULL && state->stack_gate.word == 0);
  lua_pushinteger(state, 42);
  require(lua_tointeger(state, -1) == 42);
  require(pthread_cond_destroy(&test.changed) == 0);
  require(pthread_mutex_destroy(&test.mutex) == 0);
}

static void *publication_entry(void *user) {
  lua_State *state = user;
  lua_pushinteger(state, 42);
  return NULL;
}

/* A stopped world freezes allocation records, not gate admission. An API
 * entrant can bind its context while the collector publishes that record. */
static void publication(lua_State *state) {
  lua_State *other = lua_newthread(state);
  pthread_t entrant;
  l_mem debt;
  unsigned i;
  luaX_lockstate(state);
  luaW_stop(state);
  debt = luaE_getdebt(G(state));
  /* Model an unpublished producer record before the late entrant binds. */
  luaW_trackpublisher(other);
  other->allocation_debt = 1234;
  require(!other->root_waiting);
  require(pthread_create(&entrant, NULL, publication_entry, other) == 0);
  require(pthread_mutex_lock(&G(state)->world.mutex) == 0);
  while (!other->root_waiting)
    luaQ_wait(state, &G(state)->world.mutex, &G(state)->world.changed,
              &G(state)->world.waiters);
  require(pthread_mutex_unlock(&G(state)->world.mutex) == 0);
  /* Binding a gate while stopped grants no permission to touch the frozen
   * allocation record. The collector alone publishes it below. */
  require(other->allocation_debt == 1234);
  require(luaE_getdebt(G(state)) == debt);
  for (i = 0; i < 2000; i++) {
    luaC_publish(state);
    sched_yield();
  }
  require(other->allocation_debt == 0);
  require(luaE_getdebt(G(state)) == debt + 1234);
  other->allocation_debt = -1234; /* undo the injected accounting record */
  luaM_flushdebt(other);
  require(luaE_getdebt(G(state)) == debt);
  luaW_start(state);
  luaX_unlockstate(state);
  require(pthread_join(entrant, NULL) == 0);
  require(lua_tointeger(other, -1) == 42);
  require(other->stack_gate.context == NULL && other->stack_gate.word == 0);
}

static void count_poll(lua_State *state, void *context) {
  unsigned *polls = context;
  require(state->stack_gate.context != NULL);
  (*polls)++;
}

static void native_work(lua_State *state) {
  unsigned work = 0, polls = 0;
  lua_gc(state, LUA_GCSTOP);
  state->state_poll = count_poll;
  state->state_poll_context = &polls;
  for (unsigned i = 0; i < 1023; i++)
    luaQ_work(state, &work);
  require(work == 1023 && polls == 0);
  luaQ_work(state, &work);
  require(work == 0 && polls == 1);
  luaQ_workn(state, &work, 2048 + 7);
  require(work == 7 && polls == 3);
  luaQ_workn(state, &work, 1016);
  luaQ_workn(state, &work, 0);
  require(work == 1023 && polls == 3);
  luaQ_work(state, &work);
  require(work == 0 && polls == 4);
  state->state_poll = NULL;
  state->state_poll_context = NULL;
}

int main(int argc, char **argv) {
  lua_State *state = luaL_newstate();
  require(state != NULL && argc == 2);
  if (strcmp(argv[1], "host") == 0)
    owned(state, 0);
  else if (strcmp(argv[1], "managed") == 0)
    owned(state, 1);
  else if (strcmp(argv[1], "borrow") == 0)
    reservations(state, 0);
  else if (strcmp(argv[1], "pair") == 0)
    reservations(state, 1);
  else if (strcmp(argv[1], "publication") == 0)
    publication(state);
  else if (strcmp(argv[1], "native_work") == 0)
    native_work(state);
  else {
    require(strcmp(argv[1], "foreign") == 0);
    foreign(state);
  }
  lua_close(state);
  return 0;
}
