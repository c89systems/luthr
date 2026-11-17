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
  X(stopped_ns) X(trace_calls) X(trace_ns) X(trace_work_bytes) X(trace_cycles) \
  X(trace_assist_calls) X(trace_waits) X(trace_wait_ns) \
  X(root_snapshot_calls) X(root_snapshot_ns) \
  X(heap_entries) X(heap_gc_entries) X(heap_contended_entries) X(heap_gc_wait_ns) \
  X(quiesce_waits) X(quiesce_wait_ns) X(metadata_waits) X(metadata_wait_ns) \
  X(mutator_stop_waits) X(mutator_stop_wait_ns) \
  X(publish_calls) X(publish_ns) X(drain_calls) X(drain_ns) \
  X(barrier_records) X(back_records) X(repeated_back_records) \
  X(object_reclaim_calls) X(object_reclaim_ns) X(reclaim_assist_calls) \
  X(reclaim_waits) X(reclaim_wait_ns) \
  X(reclaim_calls) X(reclaim_ns) X(inspection_ns)
#define GC_PROBE_MAXIMA(X) \
  X(mark_peak_bytes) X(sweep_peak_bytes) X(reclaim_peak_bytes) X(idle_peak_bytes)
// clang-format on

typedef struct GcProbe {
#define GC_PROBE_FIELD(name) uint64_t name;
  GC_PROBE_COUNTERS(GC_PROBE_FIELD)
  GC_PROBE_MAXIMA(GC_PROBE_FIELD)
#undef GC_PROBE_FIELD
  uint64_t coordination_start;
  uint64_t stopped_start;
  unsigned coordination_depth, gc_depth, root_depth, reason;
  int enabled;
} GcProbe;

void gc_probe_bind(lua_State *, GcProbe *);
void gc_probe_add(GcProbe *, const GcProbe *);
void gc_probe_emit(const GcProbe *);
/* Called by the backing allocator under runtime metadata ownership. */
void gc_probe_allocation(uint64_t live);
#endif
