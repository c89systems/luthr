# A transient workload must not accumulate garbage in proportion to its total
# allocation volume when automatic requests wait behind parallel workers.
foreach(collector IN ITEMS incremental generational)
  execute_process(COMMAND "${program}" "${script}" allocation_only
    "${collector}" 8 1 32 40000 1 0 0
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result STREQUAL "0" OR NOT error STREQUAL "")
    message(FATAL_ERROR "Bounded allocation failed (${collector}): ${result}: ${output}${error}")
  endif()
  string(STRIP "${output}" output)
  string(JSON done GET "${output}" completed)
  string(JSON peak GET "${output}" lua_peak_bytes)
  string(JSON retained GET "${output}" lua_retained_bytes)
  if(NOT done EQUAL 32 OR peak GREATER 33554432 OR retained GREATER 1048576)
    message(FATAL_ERROR "Transient allocation exceeded its bound (${collector}): completed=${done}, peak=${peak}, retained=${retained}")
  endif()
  message(STATUS "${collector}: transient peak=${peak}, retained=${retained}")
endforeach()
