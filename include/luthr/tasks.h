#ifndef LUTHR_TASKS_H
#define LUTHR_TASKS_H

#include <luthr/lua.h>
#include <luthr/lua_runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct luthr_lua_scheduler luthr_lua_scheduler;
typedef struct luthr_lua_task luthr_lua_task;
typedef struct luthr_lua_callback luthr_lua_callback;
typedef struct luthr_lua_carrier_pin luthr_lua_carrier_pin;

/* Zero fields select automatic sizing. Explicit carrier limits must cover
 * parallelism plus native_limit. Provider callback threads are host-owned;
 * callback_limit separately bounds their admitted entries. */
typedef struct luthr_lua_scheduler_config {
  unsigned parallelism;
  unsigned carrier_limit;
  unsigned native_limit;
  unsigned callback_limit;
  unsigned quantum_instructions;
} luthr_lua_scheduler_config;

/* NULL options snapshot the owning facade's defaults, or unrestricted defaults
 * for a raw Lua VM. A supplied record is copied: zero disables instruction
 * limits, NULL disables tracing. Contexts remain caller-owned until completion.
 * The budget follows nested coroutines/callbacks and never resets on a yield.
 */
typedef struct luthr_lua_task_options {
  unsigned long instruction_limit;
  void *context;
  luthr_lua_runtime_trace_fn trace;
  void *trace_context;
} luthr_lua_task_options;

typedef struct luthr_lua_task_result {
  luthr_lua_runtime_status status;
  int lua_status;
  int result_count;
} luthr_lua_task_result;

typedef struct luthr_lua_scheduler_stats {
  unsigned parallelism, carrier_limit, native_limit, callback_limit;
  unsigned runnable, running, native_active, callbacks_active;
  unsigned tasks, finished;
  unsigned affinity_cpus;
  unsigned long quota_milli_cpus; /* zero means the quota is unknown */
} luthr_lua_scheduler_stats;

/* Host operations, outside Lua execution/state gates. The VM and scheduler
 * outlive every task handle and entered call. One scheduler may own a VM.
 * Creation/stack preparation/collection on the same receiver are serialized
 * by the host; poll, wait and cancel may run from other host threads. */
luthr_lua_runtime_status
luthr_lua_scheduler_new(luthr_lua_state *, const luthr_lua_scheduler_config *,
                        luthr_lua_scheduler **);
luthr_lua_runtime_status luthr_lua_scheduler_free(luthr_lua_scheduler *);
luthr_lua_runtime_status luthr_lua_scheduler_shutdown(luthr_lua_scheduler *);
luthr_lua_runtime_status
luthr_lua_scheduler_snapshot(luthr_lua_scheduler *,
                             luthr_lua_scheduler_stats *);

/* A borrowed nonblocking completion descriptor for poll/epoll host loops.
 * Drain acknowledges notifications; it does not collect or discard outcomes. */
int luthr_lua_scheduler_fd(const luthr_lua_scheduler *);
luthr_lua_runtime_status luthr_lua_scheduler_drain(luthr_lua_scheduler *);

/* Adopt a fresh/yielded child with its function and nargs already on its stack.
 * The host keeps its Lua root until admission succeeds, then stops accessing
 * that coroutine. Final outcomes are retained until collection; ordinary Lua
 * yields become scheduling handoffs and their yielded values are discarded. */
luthr_lua_runtime_status luthr_lua_task_submit(luthr_lua_scheduler *,
                                               luthr_lua_state *, int nargs,
                                               const luthr_lua_task_options *,
                                               luthr_lua_task **);
/* Compile before returning; source/name storage can be released afterward.
 * Preparation failure leaves a Lua error on the scheduler's host state.
 * Unlike synchronous facade runs, submission does not replace shared arg. */
luthr_lua_runtime_status
luthr_lua_task_submit_buffer(luthr_lua_scheduler *, const unsigned char *,
                             size_t, const char *,
                             const luthr_lua_task_options *, luthr_lua_task **);
luthr_lua_runtime_status luthr_lua_task_poll(luthr_lua_task *,
                                             luthr_lua_task_result *);
/* Zero timeout polls; negative waits without a deadline. A positive timeout
 * uses milliseconds. PENDING leaves outputs intact and does not stop native C.
 */
luthr_lua_runtime_status luthr_lua_task_wait(luthr_lua_task *, long timeout_ms,
                                             luthr_lua_task_result *);
luthr_lua_runtime_status luthr_lua_task_cancel(luthr_lua_task *);
/* Transfer once to a same-VM receiver, or discard results when receiver is
 * NULL. Stack-growth failure preserves the outcome for retry. Free requires a
 * collected outcome and no concurrent calls on this handle. */
luthr_lua_runtime_status luthr_lua_task_collect(luthr_lua_task *,
                                                luthr_lua_state *receiver,
                                                luthr_lua_task_result *);
luthr_lua_runtime_status luthr_lua_task_free(luthr_lua_task *);
/* Borrow a string error until collection/free. Non-string errors have a static
 * description; collect preserves the original Lua value. Serialize with
 * collect. */
const char *luthr_lua_task_error(luthr_lua_task *);
/* Invocation context for either a raw-VM task or an owning facade task. */
void *luthr_lua_task_context(luthr_lua_state *);

/* Push a genuine bounded chunk pipe. Lua methods: send(string), recv(),
 * close(), stats(). Each chunk is at most 1024 bytes; zero capacity selects one
 * slot. Call inside a protected Lua boundary, as for lua_newuserdata. */
void luthr_lua_pipe_new(luthr_lua_state *, unsigned capacity);

/* Register the function on top without consuming it. Foreign calls use their
 * own invocation policy; same-carrier nested calls share their task's policy.
 * Invoke receives the function on top and marshals/calls through the Lua API
 * inside a protected boundary. It cannot yield across the provider boundary.
 * Revoke prevents new admission. Free additionally requires provider
 * acknowledgment and prior revocation, and returns BUSY for live
 * calls. A zero entry_limit admits one outer entry; nested owner calls reuse
 * it. */
typedef void (*luthr_lua_callback_invoke_fn)(luthr_lua_state *, void *);
luthr_lua_runtime_status luthr_lua_callback_new(luthr_lua_scheduler *,
                                                luthr_lua_state *,
                                                unsigned entry_limit,
                                                const luthr_lua_task_options *,
                                                luthr_lua_callback **);
luthr_lua_runtime_status luthr_lua_callback_call(luthr_lua_callback *,
                                                 luthr_lua_callback_invoke_fn,
                                                 void *, int *lua_status,
                                                 char *error,
                                                 size_t error_size);
luthr_lua_runtime_status luthr_lua_callback_revoke(luthr_lua_callback *);
luthr_lua_runtime_status luthr_lua_callback_free(luthr_lua_callback *);

/* Binding-author resource scopes. Create the pin before the native resource,
 * then create/use/close it on that owner. Install protected cleanup before
 * entering the resource. Unpin only after closing, including Lua errors/stop;
 * success frees the pin record. No arbitrary finalizer is adapted by pinning.
 */
luthr_lua_runtime_status luthr_lua_carrier_pin_new(luthr_lua_state *,
                                                   luthr_lua_carrier_pin **);
int luthr_lua_carrier_pin_owner(const luthr_lua_carrier_pin *);
luthr_lua_runtime_status luthr_lua_carrier_unpin(luthr_lua_state *,
                                                 luthr_lua_carrier_pin *);

#ifdef __cplusplus
}
#endif
#endif
