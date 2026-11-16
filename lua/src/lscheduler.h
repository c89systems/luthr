/* Private production task engine. See lua.h for license. */
#ifndef lscheduler_h
#define lscheduler_h

#include "lcontext.h"
#include "ltask.h"
#include "lcallback.h"

typedef struct LuaScheduler LuaScheduler;
typedef struct LuaScheduledTask LuaScheduledTask;
typedef struct LuaExecutionSlot LuaExecutionSlot;
typedef struct LuaCarrier LuaCarrier;

typedef struct LuaSchedulerConfig {
  unsigned slots;
  unsigned carriers;
  unsigned native_limit;
  unsigned quantum_polls;
  unsigned callback_limit;
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
  LuaPolicy *policy;
  LuaScheduler *scheduler;
  LuaScheduledTask *next;  /* local runnable queue, never more than one */
  LuaScheduledTask *registered_next;
  LuaScheduledTask **registered_link;
  LuaExecutionSlot *slot;
  LuaWaiter *waiter;
  LuaCarrier *carrier;  /* current dispatch; changes only after publication */
  LuaCarrier *affinity;  /* atomic reservation while pinned/inside opaque C */
  unsigned home, polls;
  unsigned dispatches, handoffs, migrations, parks, early_wakes;
  int nargs, reason, native, complete, notified, published;
  int native_pending, external;
};

struct LuaCarrier {
  LuaScheduler *scheduler;
  pthread_t thread;
  LuaContext context;
  LuaScheduledTask *reserved;  /* atomic; unrelated work must not enter C here */
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
  unsigned routing_request;  /* deferred notification from nonblocking unpin */
  unsigned external_yields;  /* atomic fairness request from provider threads */
  unsigned handoff_epoch;  /* scheduler mutex; saved-context publication */
  unsigned callbacks_active, callbacks_peak;
  unsigned submitted, outstanding, native, native_peak;
  unsigned created, next_home;
  int closed, stopping, ready;
  void (*completed)(void *);
  void *completion_context;
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
typedef struct LuaCallbackOutcome {
  int lua_status, exhausted, stopped;
} LuaCallbackOutcome;
/* Synchronous registered callbacks share the engine's managed slots. Foreign
** entries have a separate bounded admission count; their provider owns the
** physical thread. Nested attached calls retain the enclosing task identity.
** Registration and provider acknowledgment still use luaCB's lifetime rules. */
LUAI_FUNC int luaSC_callback (LuaScheduler *, LuaCallback *,
  void (*)(lua_State *, void *), void *, char *, size_t, LuaCallbackOutcome *);
/* Requires collected outcomes and no concurrent host calls. */
LUAI_FUNC int luaSC_destroy (lua_State *, LuaScheduler *);

#endif
