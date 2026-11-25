function(lua_package_archive variable name sha256)
  set(archive "${PROJECT_SOURCE_DIR}/.cache/vendor/${name}")
  if(NOT EXISTS "${archive}")
    message(FATAL_ERROR "Compatibility archive ${name} missing. Run make prepare.")
  endif()
  file(SHA256 "${archive}" digest)
  if(NOT digest STREQUAL sha256)
    message(FATAL_ERROR "Compatibility archive ${name} checksum mismatch")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${archive}")
  set(${variable} "${archive}" PARENT_SCOPE)
endfunction()

lua_package_archive(lpeg_archive lpeg-1.1.0.tar.gz
  4b155d67d2246c1ffa7ad7bc466c1ea899bbc40fef0257cc9c03cecbaed4352a)
lua_package_archive(luaunit_archive luaunit-3.5.tar.gz
  bd0085dc35c847afab0dbafc3669490ddc81b85623068ee0debe7667ffc03454)

set(lpeg_source "${CMAKE_CURRENT_BINARY_DIR}/lpeg-1.1.0")
if(NOT EXISTS "${lpeg_source}/test.lua")
  file(ARCHIVE_EXTRACT INPUT "${lpeg_archive}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
endif()
add_library(lua_package_lpeg MODULE
  "${lpeg_source}/lpcap.c" "${lpeg_source}/lpcode.c" "${lpeg_source}/lpcset.c"
  "${lpeg_source}/lpprint.c" "${lpeg_source}/lptree.c" "${lpeg_source}/lpvm.c")
set_target_properties(lua_package_lpeg PROPERTIES PREFIX "" OUTPUT_NAME "lpeg"
  C_STANDARD 99 LIBRARY_OUTPUT_DIRECTORY "${lpeg_source}")
target_include_directories(lua_package_lpeg PRIVATE "${reference_dir}")

foreach(runtime IN ITEMS upstream luthr)
  if(runtime STREQUAL "upstream")
    set(executable upstream_lua)
  else()
    set(executable luthr_test_host)
  endif()
  add_test(NAME lua_package_lpeg_${runtime}
    COMMAND "${executable}" -E -e
      [=[package.path = "./?.lua"; package.cpath = "./?.so"]=] test.lua)
  set_tests_properties(lua_package_lpeg_${runtime} PROPERTIES
    WORKING_DIRECTORY "${lpeg_source}" LABELS "runtime;compatibility" TIMEOUT 60)

  foreach(suite IN ITEMS unit functional)
    add_test(NAME lua_package_luaunit_${suite}_${runtime} COMMAND "${CMAKE_COMMAND}"
      "-Druntime=$<TARGET_FILE:${executable}>" "-Darchive=${luaunit_archive}"
      "-Dwork_dir=${CMAKE_CURRENT_BINARY_DIR}/luaunit-${suite}-${runtime}"
      "-Dsuite=${suite}" -P "${CMAKE_CURRENT_SOURCE_DIR}/RunLuaUnit.cmake")
    set_tests_properties(lua_package_luaunit_${suite}_${runtime} PROPERTIES
      LABELS "runtime;compatibility" TIMEOUT 60)
  endforeach()
endforeach()
