find_program(make_program NAMES gmake make REQUIRED)

function(check target expected)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=CONFIG
    --unset=MAKEFLAGS --unset=MFLAGS --unset=MAKEOVERRIDES
    TARGET=x86_64-linux-gnu "${make_program}" --no-print-directory --dry-run
    -f "${source_dir}/Makefile" ${ARGV2} "${target}"
    WORKING_DIRECTORY "${source_dir}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  string(FIND "${output}" "/x86_64-linux-gnu/${expected}" directory)
  if(NOT result EQUAL 0 OR directory LESS 0 OR
      NOT output MATCHES "-DCMAKE_BUILD_TYPE=\"?${expected}\"?([ \r\n]|$)")
    message(FATAL_ERROR "Wrong ${target} configuration (${ARGV2}): ${result}: ${output}${error}")
  endif()
endfunction()

foreach(target IN ITEMS build unit test)
  check("${target}" Debug)
endforeach()
foreach(target IN ITEMS benchmark benchmark-parallel benchmark-scheduler)
  check("${target}" Release)
  check("${target}" Debug CONFIG=Debug)
endforeach()
check(build Release CONFIG=Release)
