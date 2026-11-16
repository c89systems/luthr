execute_process(COMMAND "${CMAKE_COMMAND}" -E env
  TSAN_OPTIONS=halt_on_error=1:exitcode=66 "${program}" safe
  RESULT_VARIABLE safe_result OUTPUT_VARIABLE safe_output ERROR_VARIABLE safe_error
  TIMEOUT 10)
if(NOT safe_result STREQUAL "0" OR NOT safe_error STREQUAL "")
  message(FATAL_ERROR "Synchronized task fixture failed: ${safe_result}\n${safe_output}${safe_error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
  TSAN_OPTIONS=halt_on_error=1:exitcode=66 "${program}" racy
  RESULT_VARIABLE race_result OUTPUT_VARIABLE race_output ERROR_VARIABLE race_error
  TIMEOUT 10)
if(NOT race_result STREQUAL "66" OR NOT race_error MATCHES "ThreadSanitizer: data race")
  message(FATAL_ERROR "Unsafe native overlap escaped race detection: ${race_result}\n${race_output}${race_error}")
endif()
