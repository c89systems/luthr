file(MAKE_DIRECTORY "${build_dir}/module-input-checks/bad")
file(READ "${source_dir}/.cache/modules/lonejson-0.50.0/VERSION" before)
execute_process(COMMAND bash "${source_dir}/tests/modules/prepare.sh"
  "${build_dir}/module-input-checks/missing"
  RESULT_VARIABLE missing OUTPUT_QUIET ERROR_VARIABLE missing_error)
if(NOT missing EQUAL 2 OR NOT missing_error MATCHES "Module archives missing")
  message(FATAL_ERROR "Missing module archives did not fail with preparation guidance")
endif()
file(STRINGS "${source_dir}/tests/modules/lonejson-0.50.0.sha256" inputs)
foreach(input IN LISTS inputs)
  string(REGEX REPLACE "^[a-f0-9]+  " "" name "${input}")
  file(WRITE "${build_dir}/module-input-checks/bad/${name}" "incorrect archive\n")
endforeach()
execute_process(COMMAND bash "${source_dir}/tests/modules/prepare.sh"
  "${build_dir}/module-input-checks/bad" RESULT_VARIABLE mismatch
  OUTPUT_VARIABLE checks ERROR_QUIET)
if(mismatch EQUAL 0 OR NOT checks MATCHES "FAILED")
  message(FATAL_ERROR "Incorrect module archive did not fail before extraction")
endif()
file(READ "${source_dir}/.cache/modules/lonejson-0.50.0/VERSION" after)
if(NOT before STREQUAL after)
  message(FATAL_ERROR "Failed module preparation changed the prepared inputs")
endif()
