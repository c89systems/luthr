/* Private scheduled-coroutine ownership. See Copyright Notice in lua.h */
#ifndef ltask_h
#define ltask_h

#include "lstate.h"

struct global_State;
enum { LUA_TK_PENDING = -1, LUA_TK_INVALID = -2,
       LUA_TK_COLLECTED = -3, LUA_TK_STACK = -4, LUA_TK_STOPPED = -5 };
enum { LUA_TASK_UNUSED, LUA_TASK_ADMITTED, LUA_TASK_RUNNING, LUA_TASK_RETURNED,
       LUA_TASK_FINISHING,
       LUA_TASK_FINISHED, LUA_TASK_COLLECTING, LUA_TASK_COLLECTED };

typedef struct LuaTaskRoot {
  struct LuaTaskRoot *next;
  struct LuaTaskRoot **link;
  struct global_State *global;
  lua_State *thread;
  LuaExecution *execution;
  LuaAllocationCache allocation;  /* fallback owner for drivers without slots */
  LuaWaiter *waiters;
  LuaStopWait *stop_wait;  /* world mutex; node outlives every borrowed notice */
  int phase;
  int status;
  int results;
  int stop_requested;  /* atomic, sticky until the record is retired */
  int stop_delivered;  /* atomic diagnostic, not completion acknowledgment */
  int shutdown_notified;  /* world mutex; claims at most one shutdown notice */
} LuaTaskRoot;

/* Probe without delivering. The caller owns the current admission. */
l_sinline LuaTaskRoot *luaTK_pendingstop (lua_State *L) {
  LuaExecution *execution = luaX_execution(L);
  LuaTaskRoot *task;
  if (execution == NULL || L->errorJmp == NULL ||
      L->errorJmp != execution->protected) return NULL;
  task = __atomic_load_n(&execution->task, __ATOMIC_ACQUIRE);
  if (task == NULL || task->global != G(L) ||
      !__atomic_load_n(&task->stop_requested, __ATOMIC_ACQUIRE)) return NULL;
  return task;
}

/* The record and execution are initially zeroed and outlive all entered calls,
** including collection. Immutable global/thread/execution pointers remain as
** identities after collection; phase/outcome/list links/waiters use the world
** mutex.
** Admission uses a fresh/yielded child owned by the embedding caller. The VM
** outlives every operation, and the caller retains a Lua root until admission.
** No allocation or function/result copying is needed. */
LUAI_FUNC int luaTK_admit (lua_State *L, LuaTaskRoot *task,
                           LuaExecution *execution);
/* A registered foreign callback participates in the same cancellation/root
** registry, without owning the callback coroutine's resume/close interface.
** Begin under state ownership inside its protected invocation. End after the
** protected boundary has returned and every wait/notice borrow has retired. */
LUAI_FUNC void luaTK_begincallback (lua_State *, LuaTaskRoot *, LuaExecution *);
LUAI_FUNC void luaTK_endcallback (LuaTaskRoot *);
/* Only the task's quiescent driver may resume/finish. Negative results describe
** admission, never a Lua error on the protected target. Raw embedding API
** sequences still require owner serialization. */
LUAI_FUNC int luaTK_resume (LuaTaskRoot *task, int nargs, int *results);
/* Finishing a yield is invalid. Uncaught errors close the coroutine first,
** preserving the resulting error object/status. Outputs become its outcome. */
LUAI_FUNC int luaTK_finish (LuaTaskRoot *task, int *status, int *results);
/* One collector transfers the outcome without consuming it on stack-growth
** failure. PENDING does not wait for completion or another collector. After
** success no Lua state pointer retained by this record may be dereferenced. */
LUAI_FUNC int luaTK_collect (lua_State *L, LuaTaskRoot *task,
                             int *status, int *results);
/* Wait outside state gates, releasing managed capacity until completion or a
** collector changes the outcome. A task cannot await its own execution.
** The receiver remains rooted by its caller throughout the entered call.
** Owned Lua waits deliver after retirement. A quiescent admitted driver instead
** receives LUA_TK_STOPPED, with outputs and the target outcome unchanged. */
LUAI_FUNC int luaTK_await (lua_State *L, LuaTaskRoot *task,
                           int *status, int *results);
/* Private feasibility boundary. Publish a record to requesters only after
** successful admission. Requests accepted only while admitted/running;
** repeated requests are idempotent. No state gate, allocation or native join.
** Owned registered waits are notified; this never acknowledges native completion. */
LUAI_FUNC int luaTK_requeststop (LuaTaskRoot *task);
/* Private, irreversible VM task-admission gate. Requests stop for every
** admitted/running task, then notifies owned waits without retaining the world
** mutex across resource locks. No state gate, allocation, task join, or result
** discard. PENDING means task roots still exist, including finished outcomes;
** OK means all admitted outcomes have been collected. Repeated/concurrent
** callers may cooperate on notices. Neither result joins other entered calls:
** the VM and task records must still outlive every operation using them. */
LUAI_FUNC int luaTK_shutdown (struct global_State *global);
/* State ownership and the current protected execution boundary are required.
** Delivery uses a fixed error, bypassing message handlers. Finishing/close
** cleanup is exempt. Other VMs entered by a native caller remain opaque work.
** Unprotected sibling calls defer delivery until a protected boundary is owned.
** Never call this while resource bookkeeping still needs retirement. */
LUAI_FUNC void luaTK_checkstop (lua_State *L);
/* The wait has already established eligibility; all its leases are retired. */
LUAI_FUNC l_noret luaTK_throwstop (lua_State *L);

#endif
