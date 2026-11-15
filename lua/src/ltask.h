/* Private scheduled-coroutine ownership. See Copyright Notice in lua.h */
#ifndef ltask_h
#define ltask_h

#include "lexec.h"

struct global_State;
enum { LUA_TK_PENDING = -1, LUA_TK_INVALID = -2,
       LUA_TK_COLLECTED = -3, LUA_TK_STACK = -4 };
enum { LUA_TASK_UNUSED, LUA_TASK_ADMITTED, LUA_TASK_RUNNING, LUA_TASK_RETURNED,
       LUA_TASK_FINISHING,
       LUA_TASK_FINISHED, LUA_TASK_COLLECTING, LUA_TASK_COLLECTED };

typedef struct LuaTaskRoot {
  struct LuaTaskRoot *next;
  struct LuaTaskRoot **link;
  struct global_State *global;
  lua_State *thread;
  LuaExecution *execution;
  int phase;
  int status;
  int results;
} LuaTaskRoot;

/* The record and execution are initially zeroed and outlive all entered calls,
** including collection. Immutable global/thread/execution pointers remain as
** identities after collection; phase/outcome/list links use the world mutex.
** Admission uses a fresh/yielded child owned by the embedding caller. The VM
** outlives every operation, and the caller retains a Lua root until admission.
** No allocation or function/result copying is needed. */
LUAI_FUNC int luaTK_admit (lua_State *L, LuaTaskRoot *task,
                           LuaExecution *execution);
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

#endif
