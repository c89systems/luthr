/* Private production task engine. See lua.h for license. */
#ifndef lscheduler_h
#define lscheduler_h

#include "lcontext.h"
#include "ltask.h"

typedef struct LuaScheduler LuaScheduler;
typedef struct LuaScheduledTask LuaScheduledTask;
typedef struct LuaExecutionSlot LuaExecutionSlot;
typedef struct LuaCarrier LuaCarrier;

typedef struct LuaSchedulerConfig {
  unsigned slots;
  unsigned carriers;
  unsigned native_limit;
  unsigned quantum_polls;
} LuaSchedulerConfig;

struct LuaExecutionSlot {
  LuaScheduledTask *owner;  /* atomic managed permission */
  pthread_mutex_t mutex;
  LuaScheduledTask *head, *tail;
  unsigned count;
  LuaAllocationCache allocation;
};

struct LuaScheduledTask {
  LuaFiber fiber;
  LuaTaskRoot root;
  LuaScheduler *scheduler;
  LuaScheduledTask *next;  /* local runnable queue, never more than one */
  LuaScheduledTask *registered_next;
  LuaScheduledTask **registered_link;
  LuaExecutionSlot *slot;
  LuaWaiter *waiter;
  LuaCarrier *carrier;  /* current dispatch; changes only after publication */
  unsigned home, polls;
  unsigned dispatches, handoffs, migrations, parks, early_wakes;
  int nargs, reason, native, complete, notified, published;
};

struct LuaCarrier {
  LuaScheduler *scheduler;
  pthread_t thread;
  LuaContext context;
  unsigned cursor;
};

struct LuaScheduler {
  global_State *global;
  LuaSchedulerConfig config;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_mutex_t native_mutex;
  pthread_cond_t native_changed;
  LuaWaiter *native_waiters;
  LuaExecutionSlot *slots;
  LuaCarrier *carriers;
  LuaScheduledTask *tasks;
  unsigned runnable;  /* atomic; no-request polls only read this word */
  unsigned reentry_waiting;  /* atomic native-return/callback progress request */
  unsigned submitted, outstanding, native, native_peak;
  unsigned created, next_home;
  int closed, stopping, ready;
};

/* Explicit private limits until final workload sizing. carriers must cover
** slots + native_limit. Records are initially zeroed. The VM, engine and task
** records outlive every entered call. Init/submit/destroy are host operations,
** outside managed executions and with no state gate held. Native calls keep
** their physical stack; queued tasks never own a carrier. */
LUAI_FUNC int luaSC_init (lua_State *, LuaScheduler *, const LuaSchedulerConfig *);
LUAI_FUNC int luaSC_submit (LuaScheduler *, LuaScheduledTask *, lua_State *, int);
/* Completion is acknowledged only after the final outgoing context is saved.
** Waiting doesn't join an opaque native activation or discard its outcome. */
LUAI_FUNC void luaSC_wait (LuaScheduledTask *);
LUAI_FUNC int luaSC_collect (lua_State *, LuaScheduledTask *, int *, int *);
LUAI_FUNC int luaSC_shutdown (LuaScheduler *);
/* Requires collected outcomes and no concurrent host calls. */
LUAI_FUNC int luaSC_destroy (lua_State *, LuaScheduler *);

#endif
