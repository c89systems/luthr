#ifndef LUTHR_BENCH_GC_PROBE_H
#define LUTHR_BENCH_GC_PROBE_H
#include <lua.h>
#include <stdint.h>

/* Per-invocation counters: a migrating task carries its measurements with it.
 * Timers are wall durations; waits summed across tasks can exceed elapsed time.
 * Coordination includes its nested publication/drain/wait measurements. */
// clang-format off
#define GC_PROBE_COUNTERS(X) \
  X(stop_requests) X(stop_gc) X(stop_barrier) X(stop_other) \
  X(collector_calls) X(collector_running_calls) \
  X(collector_body_ns) X(collector_disabled_ns) \
  X(coordination_ns) X(stop_request_ns) X(resume_ns) \
  X(stopped_ns) X(worker_calls) X(worker_empty_calls) X(worker_ns) X(worker_trace_calls) X(worker_sweep_calls) \
  X(trace_calls) X(trace_ns) X(trace_scan_ns) X(trace_work_bytes) X(trace_cycles) \
  X(trace_assist_calls) X(trace_poll_calls) X(trace_waits) X(trace_wait_ns) \
  X(trace_zero_progress_batches) X(trace_retry_tables) X(trace_retry_threads) \
  X(trace_empty_assist) X(trace_empty_poll) X(trace_empty_step) X(trace_empty_full) \
  X(trace_empty_init) X(trace_empty_mark) X(trace_empty_weak) X(trace_empty_quiescing) \
  X(trace_empty_soft) X(trace_empty_busy) X(trace_empty_ready) X(trace_empty_metadata) \
  X(trace_retry_protos) X(trace_retry_upvalues) X(trace_retry_closures) X(trace_retry_userdata) \
  X(sweep_calls) X(sweep_ns) X(sweep_scan_ns) X(sweep_records) \
  X(sweep_paused_records) X(sweep_poll_calls) X(sweep_assist_calls) \
  X(root_snapshot_calls) X(root_snapshot_ns) \
  X(heap_entries) X(heap_gc_entries) X(heap_contended_entries) X(heap_gc_wait_ns) X(heap_deferred_entries) \
  X(allocator_calls) X(allocator_waits) X(allocator_wait_ns) \
  X(quiesce_waits) X(quiesce_wait_ns) X(metadata_waits) X(metadata_wait_ns) \
  X(metadata_handoffs) X(metadata_handoff_ns) \
  X(mutator_stop_waits) X(mutator_stop_wait_ns) \
  X(publish_calls) X(publish_ns) X(drain_calls) X(drain_ns) \
  X(barrier_records) X(back_records) X(repeated_back_records) \
  X(object_reclaim_calls) X(object_reclaim_ns) X(reclaim_assist_calls) \
  X(reclaim_waits) X(reclaim_wait_ns) \
  X(reclaim_calls) X(reclaim_ns) X(inspection_ns)
#define GC_PROBE_MAXIMA(X) \
  X(mark_peak_bytes) X(sweep_peak_bytes) X(reclaim_peak_bytes) X(idle_peak_bytes) \
  X(logical_live_peak_bytes) \
  X(pacer_parallelism_peak) X(pacer_goal_peak_bytes) X(pacer_hard_peak_bytes) \
  X(metadata_handoff_max_ns) \
  X(trace_peak_workers) X(sweep_peak_workers)
// clang-format on

typedef struct GcProbe {
#define GC_PROBE_FIELD(name) uint64_t name;
  GC_PROBE_COUNTERS(GC_PROBE_FIELD)
  GC_PROBE_MAXIMA(GC_PROBE_FIELD)
#undef GC_PROBE_FIELD
  uint64_t coordination_start;
  uint64_t stopped_start;
  unsigned coordination_depth, gc_depth, root_depth, reason, operation;
  int enabled;
} GcProbe;

void gc_probe_bind(lua_State *, GcProbe *);
void gc_probe_add(GcProbe *, const GcProbe *);
void gc_probe_emit(const GcProbe *);
void gc_probe_begin_workers(void);
void gc_probe_end_workers(GcProbe *);
/* Called by the serialized backing allocator while its state is admitted. */
void gc_probe_allocation(uint64_t live);
#endif
