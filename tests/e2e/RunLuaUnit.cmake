set(source "${work_dir}/luaunit-LUAUNIT_V3_5")
file(REMOVE_RECURSE "${source}")
file(MAKE_DIRECTORY "${work_dir}")
file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${work_dir}")

# The functional suite invokes the interpreter again using arg[-1].
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
  --unset=_DEBUG --unset=LUAUNIT_OUTPUT --unset=LUA_INIT --unset=LUA_INIT_5_5
  --unset=LUA_PATH_5_5 --unset=LUA_CPATH_5_5 "LUA_PATH=./?.lua;./?/init.lua"
  "LUA_CPATH=" LC_ALL=C "${runtime}" "run_${suite}_tests.lua"
  WORKING_DIRECTORY "${source}" RESULT_VARIABLE status
  OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(suite STREQUAL "unit")
  set(expected 214)
else()
  set(expected 19)
endif()
if(NOT status STREQUAL "0" OR NOT output MATCHES
    "Ran ${expected} tests in [0-9.]+ seconds, ${expected} successes, 0 failures")
  message(FATAL_ERROR "LuaUnit ${suite} suite failed (${status})\n${output}\n${error}")
endif()
message(STATUS "${output}")
