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
  unsigned quantum_polls;  /* zero selects an elapsed-time slice */
  unsigned callback_limit;
} LuaSchedulerConfig;

struct LuaExecutionSlot {
  void *owner;  /* atomic allocation-cache permission and task identity */
  uintptr_t stack_offer;  /* encoded slot identity for combined withdrawals */
  pthread_mutex_t mutex;
  LuaScheduledTask *head, *tail;
  unsigned count;
  LuaAllocationCache allocation;
  LuaSlotAllocPages stack_pages;
  lua_State *collector;  /* pinned until carriers have joined */
  LuaExecution collector_execution;  /* bounded, non-parking payload work */
  LuaScheduledTask *continuation;  /* at most one preferential resumption */
  unsigned dispatch_epoch;  /* queue mutex publishes each selected peer */
};

struct LuaScheduledTask {
  LuaFiber fiber;
  LuaTaskRoot root;
  LuaPolicy *policy;
  LuaScheduler *scheduler;
  LuaScheduledTask *next;  /* local runnable queue, never more than one */
  LuaScheduledTask *registered_next;
  LuaScheduledTask **registered_link;
  LuaExecutionSlot *slot;  /* active slot or the last native offer */
  uintptr_t stack_offer;  /* last offer published in the root's stack gate */
  LuaWaiter *waiter;
  LuaCarrier *carrier;  /* current dispatch; changes only after publication */
  LuaCarrier *affinity;  /* atomic reservation while pinned/inside opaque C */
  uint64_t slice_deadline;  /* set at the first contended poll */
  unsigned home, polls;
  unsigned dispatches, handoffs, migrations, parks, early_wakes;
  int native_used;  /* an outer native call returned since the last Lua poll */
  int nargs, reason, complete, notified, published;
  int native;  /* atomic permit grant, held through both active use and reuse */
  int native_pending, native_notified, external;
  lu_byte turn;  /* queue position at dispatch */
  uint64_t native_deadline;  /* expiry after the first contended Lua poll */
  uint64_t minor_deadline;  /* owner's initial slice for an explicit minor */
  size_t slice_live;  /* private survivors at the start of this slice */
  unsigned continuation_epoch;  /* peer dispatches before releasing the slot */
};

struct LuaCarrier {
  LuaScheduler *scheduler;
  pthread_t thread;
  LuaContext context;
  NativeStackContext collector_context;  /* this carrier's OS stack bounds */
  LuaScheduledTask *reserved;  /* atomic; unrelated work must not enter C here */
  pthread_cond_t changed;
  LuaCarrier *idle_next;  /* scheduler mutex; unselected sleepers only */
  int sleeping, standby;
  unsigned cursor;
  unsigned local_dispatches, steal_cursor;  /* carrier-local fairness state */
};

struct LuaScheduler {
  global_State *global;
  LuaSchedulerConfig config;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  pthread_cond_t reentry_changed;
  pthread_mutex_t native_mutex;
  LuaWaitQueue native_waiters;
  uint64_t native;  /* atomic permit count above the low queue bit */
  unsigned native_notices, native_peak;
  LuaExecutionSlot *slots;
  LuaCarrier *carriers;
  LuaCarrier *idle, *standby;  /* scheduler mutex; ordinary and reserve sleepers */
  LuaCarrier *slot_watcher;  /* scheduler mutex; at most one timed sleeper */
  LuaScheduledTask *tasks;
  unsigned runnable;  /* atomic; no-request polls only read this word */
  unsigned pending_ready;  /* atomic notifications awaiting context publication */
  unsigned reentry_waiting;  /* atomic native-return/callback progress request */
  unsigned external_yields;  /* atomic fairness request from provider threads */
  unsigned handoff_epoch;  /* scheduler mutex; saved-context publication */
  unsigned callbacks_active, callbacks_peak;
  unsigned submitted, outstanding;
  unsigned created, next_home;
  unsigned gc_active;  /* scheduler mutex; leave capacity for a phase driver */
  int closed, stopping, ready;
  void (*completed)(void *);
  void *completion_context;
  LuaExecution *minor_driver;  /* atomic; driver still using its first slice */
};

/* Explicit private limits until final workload sizing. carriers must cover
** slots + native_limit. Records are initially zeroed. The VM, engine and task
** records outlive every entered call. Init/submit/destroy are host operations,
** outside managed executions and with no state gate held. Native calls keep
** their physical stack; queued tasks never own a carrier. */
LUAI_FUNC int luaSC_init (lua_State *, LuaScheduler *, const LuaSchedulerConfig *);
LUAI_FUNC unsigned luaSC_nativeactive (const LuaScheduler *);
LUAI_FUNC void luaSC_entermanaged (LuaExecution *);
LUAI_FUNC void luaSC_leavemanaged (LuaExecution *);
LUAI_FUNC void luaSC_releaseslot (LuaScheduledTask *);
LUAI_FUNC void luaSC_enternative (LuaScheduledTask *, lua_State *);
LUAI_FUNC void luaSC_restorenative (LuaExecution *);
/* Reuse held capacity or reclaim the task's previous slot without waiting. */
LUAI_FUNC int luaSC_trymanaged (LuaExecution *);
/* Caller owns world.mutex and has volunteered to complete a new minor. */
LUAI_FUNC void luaSC_leadminor (lua_State *L);
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
/* An admitted mutator, world.mutex, or quiescence retains the engine. */
LUAI_FUNC void luaSC_notifygc (global_State *);
LUAI_FUNC void luaSC_worldresumed (global_State *);
/* Wake consecutive waiters belonging to one engine under the predicate lock.
** Return the first waiter belonging to a different owner. */
LUAI_FUNC LuaWaiter *luaSC_wakewaiters (LuaWaiter *);
LUAI_FUNC void luaSC_collectidle (LuaCarrier *, LuaExecutionSlot *);

#endif
