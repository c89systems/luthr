/* Persistent scheduled-coroutine roots and admission. See lua.h for license. */
#define ltask_c
#define LUA_CORE
#include "lprefix.h"

#include "lapi.h"
#include "ldo.h"
#include "lgc.h"
#include "ltask.h"

static void notifywait (LuaWorld *world, LuaStopWait *wait) {
  if (wait == NULL) return;
  luaQ_interrupt(wait);
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(wait->observers != 0);
  if (--wait->observers == 0)
    luaQ_wake(&world->changed, &world->waiters);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  /* No access to wait after its final borrow is published. */
}

static void registertask (lua_State *L, LuaTaskRoot *task,
                          LuaExecution *execution, int phase) {
  task->global = G(L);
  task->thread = L;
  task->execution = execution;
  task->phase = phase;
  task->next = G(L)->tasks;
  task->link = &G(L)->tasks;
  if (task->next != NULL) task->next->link = &task->next;
  G(L)->tasks = task;
}

/* The world mutex serializes requests with retirement. Publish the task's
** flag before making the request visible to the no-request check. */
static void requeststop (LuaTaskRoot *task) {
  if (!__atomic_load_n(&task->stop_requested, __ATOMIC_RELAXED)) {
    __atomic_store_n(&task->stop_requested, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&task->global->task_stops, 1, __ATOMIC_RELEASE);
  }
}

static void retirestop (LuaTaskRoot *task) {
  if (__atomic_load_n(&task->stop_requested, __ATOMIC_RELAXED))
    __atomic_fetch_sub(&task->global->task_stops, 1, __ATOMIC_RELEASE);
}

static void unregistertask (LuaTaskRoot *task) {
  *task->link = task->next;
  if (task->next != NULL) task->next->link = task->link;
  task->next = NULL;
  task->link = NULL;
  task->phase = LUA_TASK_COLLECTED;
  __atomic_store_n(&task->execution->task, NULL, __ATOMIC_RELEASE);
}

void luaTK_begincallback (lua_State *L, LuaTaskRoot *task,
                          LuaExecution *execution) {
  LuaWorld *world = &G(L)->world;
  lua_assert(task->phase == LUA_TASK_UNUSED && execution == luaQ_current() &&
    L->errorJmp != NULL && L->errorJmp == execution->protected);
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(__atomic_load_n(&execution->task, __ATOMIC_RELAXED) == NULL);
  __atomic_store_n(&execution->task, task, __ATOMIC_RELEASE);
  registertask(L, task, execution, LUA_TASK_RUNNING);
  /* An entry admitted by the scheduler before shutdown can reach this
  ** protected boundary after the admission gate closes. It must still unwind
  ** through its own carrier, with a live cancellation/root record. */
  if (G(L)->task_admission_closed) requeststop(task);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

void luaTK_endcallback (LuaTaskRoot *task) {
  LuaWorld *world;
  if (task->phase == LUA_TASK_UNUSED) return;
  world = &task->global->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  lua_assert(task->phase == LUA_TASK_RUNNING && task->stop_wait == NULL &&
    task->execution->protected == NULL && task->execution->pin_depth == 0);
  retirestop(task);
  unregistertask(task);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
}

int luaTK_requeststop (LuaTaskRoot *task) {
  LuaWorld *world;
  LuaStopWait *wait = NULL;
  int status = LUA_TK_INVALID;
  if (task->global == NULL) return status;
  world = &task->global->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  if (task->phase == LUA_TASK_ADMITTED || task->phase == LUA_TASK_RUNNING) {
    requeststop(task);
    wait = task->stop_wait;
    if (wait != NULL) wait->observers++;
    status = LUA_OK;
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  notifywait(world, wait);
  return status;
}

int luaTK_shutdown (global_State *global) {
  LuaWorld *world = &global->world;
  LuaTaskRoot *task;
  int status;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  global->task_admission_closed = 1;
  for (task = global->tasks; task != NULL; task = task->next)
    if (task->phase == LUA_TASK_ADMITTED || task->phase == LUA_TASK_RUNNING)
      requeststop(task);
  for (;;) {
    LuaStopWait *wait = NULL;
    for (task = global->tasks; task != NULL; task = task->next)
      if (!task->shutdown_notified && task->stop_wait != NULL) {
        task->shutdown_notified = 1;
        wait = task->stop_wait;
        wait->observers++;
        break;
      }
    if (wait == NULL) break;
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
    notifywait(world, wait);
    /* Collection can retire any record while the world is unlocked. Rescan
    ** from the root, never from a previously borrowed task/list link. */
    luaX_checksync(pthread_mutex_lock(&world->mutex));
  }
  status = global->tasks == NULL ? LUA_OK : LUA_TK_PENDING;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  return status;
}

void luaTK_throwstop (lua_State *L) {
  LuaExecution *execution = luaX_execution(L);
  LuaTaskRoot *task = __atomic_load_n(&execution->task, __ATOMIC_ACQUIRE);
  lua_assert(L->errorJmp != NULL && L->errorJmp == execution->protected);
  lua_assert(task != NULL && task->global == G(L));
  lua_assert(L->top.p < L->stack_last.p + EXTRA_STACK);
  __atomic_store_n(&task->stop_delivered, 1, __ATOMIC_RELEASE);
  setsvalue2s(L, L->top.p, G(L)->stoperrmsg);
  L->top.p++;
  luaD_throw(L, LUA_ERRRUN);
}

void __attribute__((noinline)) luaTK_deliverstop (lua_State *L) {
  LuaTaskRoot *task = luaTK_pendingon(L, luaX_execution(L));
  int deliver;
  if (task == NULL) return;
  luaX_checksync(pthread_mutex_lock(&task->global->world.mutex));
  deliver = __atomic_load_n(&task->stop_requested, __ATOMIC_ACQUIRE) &&
            task->phase == LUA_TASK_RUNNING && L->close_active == 0;
  luaX_checksync(pthread_mutex_unlock(&task->global->world.mutex));
  if (deliver) luaTK_throwstop(L);
}

void luaTK_checkstop (lua_State *L) {
  if (l_unlikely(luaTK_hasstops(L))) luaTK_deliverstop(L);
}

static int driver (LuaTaskRoot *task) {
  LuaExecution *execution = luaQ_current();
  return execution == task->execution && execution != NULL &&
         __atomic_load_n(&execution->task, __ATOMIC_ACQUIRE) == task &&
         execution->managed_depth == 0 && execution->native_depth == 0 &&
         execution->protected == NULL && execution->trusted == NULL &&
         luaE_getnativestackcontext()->coroutine_scopes == NULL;
}

static int driverstopped (lua_State *L) {
  LuaExecution *execution = luaQ_current();
  LuaTaskRoot *task;
  int stopped;
  if (execution == NULL) return 0;
  task = __atomic_load_n(&execution->task, __ATOMIC_ACQUIRE);
  if (task == NULL || task->global != G(L) || task->thread != L || !driver(task))
    return 0;
  luaX_checksync(pthread_mutex_lock(&task->global->world.mutex));
  stopped = task->phase == LUA_TASK_ADMITTED &&
            __atomic_load_n(&task->stop_requested, __ATOMIC_ACQUIRE);
  luaX_checksync(pthread_mutex_unlock(&task->global->world.mutex));
  return stopped;
}

int luaTK_admit (lua_State *L, LuaTaskRoot *task, LuaExecution *execution) {
  LuaWorld *world = &G(L)->world;
  int status = LUA_TK_INVALID;
  if (task->phase != LUA_TASK_UNUSED || execution == NULL ||
      execution->managed_depth != 0 || execution->native_depth != 0 ||
      execution->pin_depth != 0 ||
      execution->protected != NULL || execution->trusted != NULL)
    return status;
  lua_lock(L);
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  if (!G(L)->task_admission_closed &&
      L != mainthread(G(L)) && L->task_owner == NULL &&
      L->callback_owner == NULL && L->operation == NULL &&
      L->owned_operations == 0 && L->resume_active == 0 && L->close_active == 0 &&
      (L->status == LUA_YIELD ||
       (L->status == LUA_OK && L->ci == &L->base_ci &&
        L->top.p > L->ci->func.p + 1 &&
        ttisfunction(s2v(L->ci->func.p + 1))))) {
    LuaTaskRoot *expected = NULL;
    if (!__atomic_compare_exchange_n(&execution->task, &expected, task, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      luaX_checksync(pthread_mutex_unlock(&world->mutex));
      lua_unlock(L);
      return LUA_TK_INVALID;
    }
    /* Initial root discovery can precede this admission while the mark
    ** handshake still prevents stack snapshots. The caller can drop its
    ** stack reference as soon as this root is published. */
    luaC_traceobject(L, obj2gco(L));
    registertask(L, task, execution, LUA_TASK_ADMITTED);
    __atomic_store_n(&L->task_owner, task, __ATOMIC_RELAXED);
    L->task_nursery_closed = 0;
    status = LUA_OK;
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  lua_unlock(L);
  return status;
}

int luaTK_resume (LuaTaskRoot *task, int nargs, int *results) {
  LuaWorld *world;
  int status;
  if (task->global == NULL || results == NULL || nargs < 0 || !driver(task))
    return LUA_TK_INVALID;
  world = &task->global->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  status = task->phase == LUA_TASK_ADMITTED ? LUA_OK : LUA_TK_INVALID;
  if (status == LUA_OK) task->phase = LUA_TASK_RUNNING;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  if (status != LUA_OK) return status;
  status = lua_resume(task->thread, NULL, nargs, results);
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  task->status = status;
  task->results = *results;
  task->phase = status == LUA_YIELD ? LUA_TASK_ADMITTED : LUA_TASK_RETURNED;
  if (status != LUA_YIELD) retirestop(task);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  return status;
}

int luaTK_finish (LuaTaskRoot *task, int *status, int *results) {
  lua_State *L;
  LuaWorld *world;
  int valid;
  if (task->global == NULL || status == NULL || results == NULL ||
      *status < LUA_OK || *status > LUA_ERRERR || *status == LUA_YIELD ||
      *results < 0 || !driver(task))
    return LUA_TK_INVALID;
  world = &task->global->world;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  valid = task->phase == LUA_TASK_RETURNED && task->status == *status &&
          task->results == *results;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  if (!valid) return LUA_TK_INVALID;
  L = task->thread;
  lua_lock(L);
  valid = L->status == *status && L->resume_active == 0 && L->close_active == 0 &&
          L->errorJmp == NULL && L->native_depth == 0 &&
          L->top.p - (L->ci->func.p + 1) >= *results;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  valid = valid && L->owned_operations == 0;
  if (valid) task->phase = LUA_TASK_FINISHING;
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  if (valid && luaC_hastaskheap(L)) {
    if (*status == LUA_OK) luaC_endtask(L);
    else luaC_sealtask(L);
  }
  if (valid) luaC_freetaskroots(L);
  lua_unlock(L);
  if (!valid) return LUA_TK_INVALID;
  if (*status != LUA_OK) {
    *status = lua_closethread(L, NULL);
    *results = *status == LUA_OK ? 0 : 1;
  }
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  task->status = *status;
  task->results = *results;
  task->phase = LUA_TASK_FINISHED;
  luaQ_wake(&world->changed, &task->waiters);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  return LUA_OK;
}

static int collect (lua_State *L, LuaTaskRoot *task, int *status, int *results) {
  LuaWorld *world = &G(L)->world;
  int outcome, count, result;
  if (task->global != G(L) || task->thread == L ||
      status == NULL || results == NULL)
    return LUA_TK_INVALID;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  if (task->phase == LUA_TASK_COLLECTED) result = LUA_TK_COLLECTED;
  else if (task->phase != LUA_TASK_FINISHED) result = LUA_TK_PENDING;
  else {
    task->phase = LUA_TASK_COLLECTING;
    outcome = task->status;
    count = task->results;
    result = LUA_OK;
  }
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  if (result != LUA_OK) return result;
  if (!lua_checkstack(L, count)) {
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    task->phase = LUA_TASK_FINISHED;
    luaQ_wake(&world->changed, &task->waiters);
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
    return LUA_TK_STACK;
  }
  lua_xmove(task->thread, L, count);
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  luaC_retireseal(task->thread);
  __atomic_store_n(&task->thread->task_owner, NULL, __ATOMIC_RELAXED);
  unregistertask(task);
  luaQ_wake(&world->changed, &task->waiters);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  *status = outcome;
  *results = count;
  return LUA_OK;
}

int luaTK_collect (lua_State *L, LuaTaskRoot *task, int *status, int *results) {
  LuaWorldScope world = luaW_suspendapi();
  int result = collect(L, task, status, results);
  luaW_restoreapi(world);
  return result;
}

static int await (lua_State *L, LuaTaskRoot *task, int *status, int *results) {
  LuaWorld *world = &G(L)->world;
  int result;
  if (task->global != G(L) || task->thread == L ||
      task->execution == luaQ_current() || status == NULL || results == NULL)
    return LUA_TK_INVALID;
  /* Retire capacity granted to an initial driver dispatch, too. Ordinary Lua
  ** C activations publish their roots and detach at the operation boundary. */
  for (;;) {
    int stopped = 0;
    lua_lock(L);
    luaTK_checkstop(L);
    lua_unlock(L);
    if (driverstopped(L)) return LUA_TK_STOPPED;
    result = luaTK_collect(L, task, status, results);
    if (result != LUA_TK_PENDING) return result;
    luaX_checksync(pthread_mutex_lock(&world->mutex));
    while (task->phase != LUA_TASK_FINISHED &&
           task->phase != LUA_TASK_COLLECTED)
      if (luaQ_waitstop(L, &world->mutex, &world->changed, &task->waiters)) {
        stopped = 1;
        break;
      }
    luaX_checksync(pthread_mutex_unlock(&world->mutex));
    if (stopped) {
      if (driverstopped(L)) return LUA_TK_STOPPED;
      lua_lock(L);
      luaTK_throwstop(L);
    }
  }
}

int luaTK_await (lua_State *L, LuaTaskRoot *task, int *status, int *results) {
  LuaWorldScope world = luaW_suspendapi();
  int result = await(L, task, status, results);
  luaW_restoreapi(world);
  return result;
}
