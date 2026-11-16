# The pristine reference is Lua itself, never a downstream stack dependency.
set(reference_archive "${PROJECT_SOURCE_DIR}/.cache/vendor/lua-5.5.1.tar.gz")
if(NOT EXISTS "${reference_archive}")
  message(FATAL_ERROR "Pristine Lua 5.5.1 reference archive missing. Run make prepare (or bash tools/prepare_lua_reference.sh). Product-only builds may use BUILD_TESTING=OFF.")
endif()
file(SHA256 "${reference_archive}" reference_digest)
if(NOT reference_digest STREQUAL "1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce")
  message(FATAL_ERROR "Pristine Lua 5.5.1 reference archive checksum mismatch")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${reference_archive}")
set(reference_dir "${CMAKE_CURRENT_BINARY_DIR}/upstream/lua-5.5.1/src")
if(NOT EXISTS "${reference_dir}/lua.h")
  file(ARCHIVE_EXTRACT INPUT "${reference_archive}"
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/upstream")
  file(GLOB reference_inputs "${reference_dir}/*.c" "${reference_dir}/*.h")
  file(TOUCH ${reference_inputs})
endif()
file(GLOB reference_sources CONFIGURE_DEPENDS "${reference_dir}/*.c")
list(REMOVE_ITEM reference_sources "${reference_dir}/lua.c" "${reference_dir}/luac.c")
add_library(upstream_lua_reference STATIC ${reference_sources})
set_target_properties(upstream_lua_reference PROPERTIES C_STANDARD 99)
target_compile_definitions(upstream_lua_reference PRIVATE LUA_USE_LINUX)
target_include_directories(upstream_lua_reference PUBLIC "${reference_dir}")
target_link_libraries(upstream_lua_reference PUBLIC m ${CMAKE_DL_LIBS})
luthrum_test_program(upstream_lua "${reference_dir}/lua.c")
luthrum_test_program(upstream_luac "${reference_dir}/luac.c")
foreach(program IN ITEMS upstream_lua upstream_luac)
  set_target_properties(${program} PROPERTIES C_STANDARD 99)
  target_compile_definitions(${program} PRIVATE LUA_USE_LINUX)
  target_link_libraries(${program} PRIVATE upstream_lua_reference)
endforeach()

target_link_options(upstream_lua PRIVATE -Wl,--export-dynamic)

add_library(compat_fixture MODULE native.c)
set_target_properties(compat_fixture PROPERTIES PREFIX "" C_STANDARD 99)
target_include_directories(compat_fixture PRIVATE "${reference_dir}")
target_compile_options(compat_fixture PRIVATE -Wall -Wextra -Werror)

set(benchmark_arguments
  "-Dupstream=$<TARGET_FILE:upstream_lua>" "-Dluthrum=$<TARGET_FILE:luthrum_test_host>"
  "-Dscript=${CMAKE_CURRENT_SOURCE_DIR}/benchmark.lua"
  "-Dmodule_dir=$<TARGET_FILE_DIR:compat_fixture>"
  "-Dtarget=${LUTHRUM_TARGET}" "-Dconfig=${CMAKE_BUILD_TYPE}"
  "-Dcompiler=${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
add_custom_target(benchmark_lua
  COMMAND "${CMAKE_COMMAND}" -E env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0
    "${CMAKE_COMMAND}" ${benchmark_arguments}
    "-Doutput_dir=${PROJECT_BINARY_DIR}/benchmarks/lua"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/BenchmarkLua.cmake"
  DEPENDS upstream_lua luthrum_test_host compat_fixture
  VERBATIM USES_TERMINAL)
add_test(NAME lua_benchmark_smoke COMMAND "${CMAKE_COMMAND}" ${benchmark_arguments}
  -Dsamples=2 -Dwarmups=1 -Dwork_divisor=100
  "-Doutput_dir=${CMAKE_CURRENT_BINARY_DIR}/benchmark-smoke"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/BenchmarkSmoke.cmake")
set_tests_properties(lua_benchmark_smoke PROPERTIES LABELS "runtime;compatibility" TIMEOUT 30)

foreach(case IN ITEMS language native files_modules metatables diagnostics coroutine_close hooks)
  foreach(collector IN ITEMS incremental generational)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${case}-${collector}/direct")
    add_test(NAME lua_e2e_${case}_${collector} COMMAND luthrum_test_host
      "${CMAKE_CURRENT_SOURCE_DIR}/entry.lua" "$<TARGET_FILE_DIR:compat_fixture>"
      "${CMAKE_CURRENT_SOURCE_DIR}/${case}.lua" "${collector}"
      "${CMAKE_CURRENT_BINARY_DIR}/${case}-${collector}/direct")
    set_tests_properties(lua_e2e_${case}_${collector}
      PROPERTIES LABELS "runtime;compatibility;memcheck" TIMEOUT 30)
    add_test(NAME lua_compat_${case}_${collector} COMMAND "${CMAKE_COMMAND}"
      "-Dupstream=$<TARGET_FILE:upstream_lua>" "-Dluthrum=$<TARGET_FILE:luthrum_test_host>"
      "-Dupstream_compiler=$<TARGET_FILE:upstream_luac>"
      "-Dluthrum_compiler=$<TARGET_FILE:luthrumc>" "-Dloader=${LUTHRUM_RUNTIME_LOADER}"
      "-Druntime_libraries=${CMAKE_SYSROOT}/lib:${CMAKE_SYSROOT}/usr/lib"
      "-Dentry=${CMAKE_CURRENT_SOURCE_DIR}/entry.lua"
      "-Dsource=${CMAKE_CURRENT_SOURCE_DIR}/${case}.lua"
      "-Dmodule_dir=$<TARGET_FILE_DIR:compat_fixture>" "-Dcollector=${collector}"
      "-Dwork_dir=${CMAKE_CURRENT_BINARY_DIR}/${case}-${collector}/comparison"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/CompareLua.cmake")
    set_tests_properties(lua_compat_${case}_${collector}
      PROPERTIES LABELS "runtime;compatibility" TIMEOUT 30)
  endforeach()
endforeach()

if(TARGET debug_worker_stack)
  get_property(e2e_tests DIRECTORY PROPERTY TESTS)
  foreach(test IN LISTS e2e_tests)
    set_tests_properties(${test} PROPERTIES
      ENVIRONMENT "GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0")
  endforeach()
endif()
