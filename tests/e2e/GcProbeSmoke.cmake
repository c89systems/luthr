include("${CMAKE_CURRENT_LIST_DIR}/BenchmarkGc.cmake")
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
set(body_total 0)
set(records_total 0)
set(object_total 0)
foreach(path IN LISTS gc_profile_paths)
  file(STRINGS "${path}" rows)
  foreach(row IN LISTS rows)
    foreach(field IN ITEMS stop_requests stop_gc stop_barrier stop_other
        collector_body_ns collector_calls collector_running_calls barrier_records
        back_records repeated_back_records object_reclaim_calls object_reclaim_ns)
      string(JSON ${field} GET "${row}" "gc_${field}")
    endforeach()
    math(EXPR classified "${stop_gc} + ${stop_barrier} + ${stop_other}")
    if(NOT classified EQUAL stop_requests OR
        repeated_back_records GREATER back_records OR
        back_records GREATER barrier_records OR
        object_reclaim_ns GREATER collector_body_ns)
      message(FATAL_ERROR "Invalid GC probe accounting: ${path}: ${row}")
    endif()
    math(EXPR body_total "${body_total} + ${collector_body_ns}")
    math(EXPR records_total "${records_total} + ${barrier_records}")
    math(EXPR object_total "${object_total} + ${object_reclaim_ns}")
    if(path MATCHES "-stopped-")
      if(NOT collector_running_calls EQUAL 0)
        message(FATAL_ERROR "Stopped probe ran the collector")
      endif()
    endif()
  endforeach()
endforeach()
if(body_total EQUAL 0 OR records_total EQUAL 0 OR object_total EQUAL 0)
  message(FATAL_ERROR "Probe did not observe collection and barriers")
endif()
file(WRITE "${output_dir}/explicit.lua"
  "return function() local t={}; for i=1,512 do t[i]={i} end; collectgarbage('collect'); return 1 end,function() end,function() end,1\n")
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
foreach(runtime IN ITEMS upstream shared control profile)
  execute_process(COMMAND "${${runtime}}" "${script}" integers incremental
    1 0 8 256 1 0 0 invalid RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result STREQUAL "2" OR NOT output STREQUAL "" OR
      NOT error MATCHES "usage:")
    message(FATAL_ERROR "Invalid GC control was not rejected: ${runtime}")
  endif()
endforeach()
