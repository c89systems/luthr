include("${CMAKE_CURRENT_LIST_DIR}/BenchmarkGc.cmake")
file(STRINGS "${output_dir}/summary.csv" header LIMIT_COUNT 1)
foreach(field IN ITEMS metadata_handoffs metadata_handoff_ns metadata_handoff_max_ns)
  if(NOT header MATCHES "(^|,)gc_${field}(,|$)")
    message(FATAL_ERROR "Metadata handoff metric is missing from summary: ${field}")
  endif()
endforeach()
# The quick matrix can fit below the collector's debt threshold. These bounded
# cases must actually cross it so missing probe hooks cannot pass unnoticed.
foreach(collector IN ITEMS incremental generational)
  foreach(mode IN ITEMS running stopped)
    set(extra)
    if(mode STREQUAL "stopped")
      set(extra stopped)
    endif()
    execute_process(COMMAND "${profile}" "${script}" independent_tables
      "${collector}" 2 1 8 4096 1 0 0 ${extra}
      RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result STREQUAL "0" OR NOT error STREQUAL "")
      message(FATAL_ERROR "GC phase probe failed: ${result}: ${output}${error}")
    endif()
    set(path "${output_dir}/profile-observed-${collector}-${mode}-2.jsonl")
    file(WRITE "${path}" "${output}")
    list(APPEND gc_profile_paths "${path}")
  endforeach()
endforeach()
# A stopped automatic collector leaves this finalizer for cleanup. Charge its
# deliberate native delay to total runtime, including in the upstream control.
file(WRITE "${output_dir}/late-gc.lua"
  "return function() local t=setmetatable({}, {__gc=function() benchmark_delay() end}); return 1 end,function() end,function() end,1\n")
foreach(runtime IN ITEMS upstream shared control profile)
  if(runtime STREQUAL "upstream")
    set(native 0)
  else()
    set(native 1)
  endif()
  execute_process(COMMAND "${${runtime}}" "${output_dir}/late-gc.lua" late_gc
    incremental 1 "${native}" 1 1 1 0 50000 stopped RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result STREQUAL "0" OR NOT error STREQUAL "")
    message(FATAL_ERROR "Final collection timing failed: ${runtime}: ${result}: ${output}${error}")
  endif()
  string(STRIP "${output}" output)
  string(JSON cleanup GET "${output}" cleanup_wall_ns)
  string(JSON total GET "${output}" wall_ns)
  if(cleanup LESS 40000000 OR total LESS cleanup)
    message(FATAL_ERROR "Finalizer delay was excluded from total runtime: ${runtime}")
  endif()
endforeach()
set(body_total 0)
set(records_total 0)
set(trace_total 0)
set(marked_total 0)
set(roots_total 0)
set(stopped_total 0)
set(swept_total 0)
set(workers_peak 0)
foreach(path IN LISTS gc_profile_paths)
  file(STRINGS "${path}" rows)
  foreach(row IN LISTS rows)
    foreach(field IN ITEMS stop_requests stop_gc stop_barrier stop_other publish_calls
        collector_body_ns collector_calls collector_running_calls barrier_records
        back_records repeated_back_records object_reclaim_calls object_reclaim_ns
        trace_ns trace_scan_ns trace_peak_workers trace_work_bytes root_snapshot_ns stopped_ns
        sweep_ns sweep_scan_ns sweep_records sweep_paused_records
        startup_waits startup_wait_ns arm_waits arm_wait_ns minor_waits minor_wait_ns
        phase_waits phase_wait_ns trace_waits trace_wait_ns reclaim_waits reclaim_wait_ns
        metadata_waits metadata_wait_ns metadata_handoffs metadata_handoff_ns metadata_handoff_max_ns)
      string(JSON ${field} GET "${row}" "gc_${field}")
    endforeach()
    math(EXPR classified "${stop_gc} + ${stop_barrier} + ${stop_other}")
    foreach(clock IN ITEMS cpu wall)
      string(JSON total GET "${row}" "${clock}_ns")
      string(JSON execution GET "${row}" "execution_${clock}_ns")
      string(JSON cleanup GET "${row}" "cleanup_${clock}_ns")
      math(EXPR combined "${execution} + ${cleanup}")
      if(NOT combined EQUAL total OR cleanup EQUAL 0)
        message(FATAL_ERROR "Collector cleanup is missing from total timing: ${path}")
      endif()
    endforeach()
    if(trace_peak_workers GREATER workers_peak)
      set(workers_peak "${trace_peak_workers}")
    endif()
    string(JSON peak GET "${row}" lua_peak_bytes)
    foreach(phase IN ITEMS mark sweep reclaim idle)
      string(JSON observed GET "${row}" "gc_${phase}_peak_bytes")
      if(observed GREATER peak)
        message(FATAL_ERROR "GC phase peak exceeds total peak: ${path}")
      endif()
    endforeach()
    if(NOT classified EQUAL stop_requests OR
        NOT publish_calls EQUAL stop_requests OR
        repeated_back_records GREATER back_records OR
        back_records GREATER barrier_records OR
        object_reclaim_ns GREATER collector_body_ns OR trace_scan_ns GREATER trace_ns OR
        sweep_scan_ns GREATER sweep_ns OR sweep_paused_records GREATER 0 OR
        metadata_handoffs GREATER metadata_waits OR metadata_handoff_ns GREATER metadata_wait_ns OR
        metadata_handoff_max_ns GREATER metadata_handoff_ns)
      message(FATAL_ERROR "Invalid GC probe accounting: ${path}: ${row}")
    endif()
    foreach(reason IN ITEMS startup arm minor phase trace reclaim)
      if(${reason}_waits EQUAL 0 AND NOT ${reason}_wait_ns EQUAL 0)
        message(FATAL_ERROR "Invalid ${reason} wait accounting: ${path}: ${row}")
      endif()
    endforeach()
    math(EXPR body_total "${body_total} + ${collector_body_ns}")
    math(EXPR records_total "${records_total} + ${barrier_records}")
    math(EXPR trace_total "${trace_total} + ${trace_ns}")
    math(EXPR marked_total "${marked_total} + ${trace_work_bytes}")
    math(EXPR roots_total "${roots_total} + ${root_snapshot_ns}")
    math(EXPR stopped_total "${stopped_total} + ${stopped_ns}")
    math(EXPR swept_total "${swept_total} + ${sweep_records}")
    if(path MATCHES "-stopped-")
      if(NOT collector_running_calls EQUAL 0)
        message(FATAL_ERROR "Stopped probe ran the collector")
      endif()
    endif()
  endforeach()
endforeach()
if(body_total EQUAL 0 OR records_total EQUAL 0 OR
    trace_total EQUAL 0 OR marked_total EQUAL 0 OR roots_total EQUAL 0 OR
    stopped_total EQUAL 0 OR workers_peak EQUAL 0 OR swept_total EQUAL 0)
  message(FATAL_ERROR "Probe did not observe tracing, roots, stops and sweeping")
endif()
# Drop a table with separate backing so this control must also exercise the
# deferred destruction probe. Inline tables can return their slabs in sweep.
file(WRITE "${output_dir}/explicit.lua"
  "return function() local t={}; for i=1,512 do t[i]=i end; t=nil; collectgarbage('collect'); return 1 end,function() end,function() end,1\n")
execute_process(COMMAND "${profile}" "${output_dir}/explicit.lua" explicit
  incremental 2 1 8 256 1 0 0 stopped RESULT_VARIABLE result
  OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result STREQUAL "0" OR NOT error STREQUAL "")
  message(FATAL_ERROR "Explicit collection control failed: ${result}: ${output}${error}")
endif()
string(STRIP "${output}" output)
string(JSON body GET "${output}" gc_collector_body_ns)
if(body EQUAL 0)
  message(FATAL_ERROR "Explicit full collection was mislabeled as disabled dispatch")
endif()
string(JSON reclaimed GET "${output}" gc_object_reclaim_calls)
if(reclaimed EQUAL 0)
  message(FATAL_ERROR "Explicit full collection did not observe deferred destruction")
endif()
file(WRITE "${output_dir}/poll.lua"
  "return function() collectgarbage('stop'); local head; for i=1,20000 do head={head} end; collectgarbage('restart'); collectgarbage('step',0); local sum=0; for i=1,200000 do sum=sum+i end; assert(head and sum==20000100000); return 1 end,function() end,function() end,1\n")
execute_process(COMMAND "${profile}" "${output_dir}/poll.lua" marking_poll
  incremental 1 1 1 1 1 0 0 RESULT_VARIABLE result
  OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result STREQUAL "0" OR NOT error STREQUAL "")
  message(FATAL_ERROR "VM marking probe failed: ${result}: ${output}${error}")
endif()
string(STRIP "${output}" output)
string(JSON polls GET "${output}" gc_trace_poll_calls)
string(JSON running GET "${output}" gc_collector_running_calls)
if(polls EQUAL 0 OR polls GREATER running)
  message(FATAL_ERROR "VM marking work is missing from collector accounting")
endif()
foreach(runtime IN ITEMS upstream shared control profile)
  execute_process(COMMAND "${${runtime}}" "${script}" integers incremental
    1 0 8 256 1 0 0 invalid RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result STREQUAL "2" OR NOT output STREQUAL "" OR
      NOT error MATCHES "usage:")
    message(FATAL_ERROR "Invalid GC control was not rejected: ${runtime}")
  endif()
endforeach()
