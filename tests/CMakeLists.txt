if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND LUTHRUM_TARGET STREQUAL "x86_64-linux-gnu")
  add_library(debug_worker_stack OBJECT debug_worker_stack_init.c)
  luthrum_c89(debug_worker_stack)
  target_link_libraries(debug_worker_stack PRIVATE Threads::Threads)
endif()

function(luthrum_test_program name source)
  add_executable(${name} ${source})
  luthrum_local_runtime(${name})
  if(TARGET debug_worker_stack)
    target_link_libraries(${name} PRIVATE debug_worker_stack)
  endif()
endfunction()

foreach(linkage IN ITEMS static shared)
  luthrum_test_program(lua_facade_${linkage} lua_facade_test.c)
  luthrum_c89(lua_facade_${linkage})
  target_link_libraries(lua_facade_${linkage} PRIVATE luthrum_${linkage})
  add_test(NAME lua_facade_${linkage} COMMAND lua_facade_${linkage})
  set_tests_properties(lua_facade_${linkage} PROPERTIES LABELS "facade;memcheck" TIMEOUT 10)

  luthrum_test_program(lua_contract_${linkage} lua_runtime_contract_test.c)
  luthrum_c89(lua_contract_${linkage})
  target_link_libraries(lua_contract_${linkage} PRIVATE luthrum_${linkage})
  foreach(case IN ITEMS large_arguments large_results large_yields
      trace_observer trace_limits closed_error close_error_replaced protected_limit
      xprotected_limit xhandler_limit xhandler_error_limit xhandler_normal gc_limit
      gc_coroutine_limit gc_finite gc_close_limit gc_close_after_limit native_gc_hooks
      coroutine_close_existing_limit coroutine_close_reopen_limit
      coroutine_close_cleared_limit coroutine_close_normal_results registry_limit
      protected_yields yielded_protected_limit upvalue_limit existing_wrap_limit
      native_upvalues shared_resume_budget shared_wrap_budget error_conversion_oom
      numeric_error preload_registration preload_arguments preload_lua_upvalues
      preload_c_upvalues preload_c_allocation reopen_resume_limit reopen_wrap_limit
      reopen_library_identity construction_failure)
    add_test(NAME lua_contract_${linkage}_${case} COMMAND lua_contract_${linkage} ${case})
    set_tests_properties(lua_contract_${linkage}_${case}
      PROPERTIES LABELS "facade;memcheck" TIMEOUT 10)
  endforeach()

  luthrum_test_program(lua_native_stack_${linkage} lua_native_stack_test.c)
  set_target_properties(lua_native_stack_${linkage} PROPERTIES C_STANDARD 99)
  target_compile_options(lua_native_stack_${linkage} PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(lua_native_stack_${linkage} PRIVATE luthrum_${linkage})
  if(LUTHRUM_TARGET STREQUAL "x86_64-linux-gnu")
    target_compile_definitions(lua_native_stack_${linkage} PRIVATE LUTHRUM_TEST_SMALL_NATIVE_STACK)
  endif()
  if(linkage STREQUAL "static")
    target_compile_definitions(lua_native_stack_${linkage} PRIVATE LUTHRUM_TEST_WRAP_BOUNDS)
    target_link_options(lua_native_stack_${linkage} PRIVATE -Wl,--wrap=pthread_getattr_np)
  endif()
  add_test(NAME lua_native_stack_${linkage} COMMAND lua_native_stack_${linkage})
  set_tests_properties(lua_native_stack_${linkage} PROPERTIES LABELS "stack;memcheck" TIMEOUT 30)
endforeach()

if(TARGET debug_worker_stack)
  luthrum_test_program(debug_worker_stack_test debug_worker_stack_test.c)
  luthrum_c89(debug_worker_stack_test)
  target_link_libraries(debug_worker_stack_test PRIVATE Threads::Threads)
  target_compile_definitions(debug_worker_stack_test PRIVATE LUTHRUM_TEST_NATIVE_SELF_EXEC)
  add_test(NAME debug_worker_defaults COMMAND debug_worker_stack_test defaults)
  add_test(NAME debug_worker_guard COMMAND debug_worker_stack_test guard)
  set_tests_properties(debug_worker_defaults PROPERTIES LABELS "stack;memcheck" TIMEOUT 10)
  set_tests_properties(debug_worker_guard PROPERTIES LABELS stack TIMEOUT 10)
endif()

luthrum_test_program(luthrum_test_host "${PROJECT_SOURCE_DIR}/lua/src/lua.c")
set_target_properties(luthrum_test_host PROPERTIES C_STANDARD 99)
target_compile_definitions(luthrum_test_host PRIVATE LUA_USE_LINUX)
target_link_libraries(luthrum_test_host PRIVATE luthrum_shared)
add_test(NAME interpreter COMMAND luthrum_test_host "${CMAKE_CURRENT_SOURCE_DIR}/interpreter.lua")
set_tests_properties(interpreter PROPERTIES LABELS "runtime;memcheck" TIMEOUT 10)

add_test(NAME bytecode_compile COMMAND "${LUTHRUM_RUNTIME_LOADER}"
  --library-path "${CMAKE_SYSROOT}/lib:${CMAKE_SYSROOT}/usr/lib"
  "$<TARGET_FILE:luthrumc>" -o "${CMAKE_CURRENT_BINARY_DIR}/interpreter.luac"
  "${CMAKE_CURRENT_SOURCE_DIR}/interpreter.lua")
set_tests_properties(bytecode_compile PROPERTIES FIXTURES_SETUP bytecode LABELS runtime TIMEOUT 10)
add_test(NAME bytecode_execute COMMAND luthrum_test_host "${CMAKE_CURRENT_BINARY_DIR}/interpreter.luac")
set_tests_properties(bytecode_execute PROPERTIES FIXTURES_REQUIRED bytecode LABELS "runtime;memcheck" TIMEOUT 10)

add_test(NAME stage_sdk COMMAND "${CMAKE_COMMAND}"
  "-Dbuild_dir=${PROJECT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/StageSdk.cmake")
set_tests_properties(stage_sdk PROPERTIES FIXTURES_SETUP sdk LABELS install TIMEOUT 20)
add_test(NAME installed_consumers COMMAND "${CMAKE_COMMAND}"
  "-Dbuild_dir=${PROJECT_BINARY_DIR}" "-Dsource_dir=${PROJECT_SOURCE_DIR}"
  "-Dtarget=${LUTHRUM_TARGET}" "-Dconfig=${CMAKE_BUILD_TYPE}"
  "-Dlifecycle_scripts=${CPKT_LIFECYCLE_SCRIPTS_DIR}"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/InstalledConsumers.cmake")
set_tests_properties(installed_consumers PROPERTIES FIXTURES_REQUIRED sdk LABELS install TIMEOUT 60)

add_test(NAME public_exports COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/exports.sh"
  "${CMAKE_NM}" "$<TARGET_FILE:luthrum_shared>" "${export_map}")
set_tests_properties(public_exports PROPERTIES LABELS abi TIMEOUT 10)
add_test(NAME clean_scope COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/clean.sh"
  "${PROJECT_SOURCE_DIR}/Makefile" "${CMAKE_CURRENT_BINARY_DIR}/clean-fixture")
set_tests_properties(clean_scope PROPERTIES LABELS lifecycle TIMEOUT 10)
if(LUTHRUM_TARGET STREQUAL "x86_64-linux-gnu" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_test(NAME toolchain_required COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/toolchain-required.sh"
    "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/no-toolchain")
  set_tests_properties(toolchain_required PROPERTIES LABELS lifecycle TIMEOUT 10)
endif()

if(TARGET debug_worker_stack)
  get_property(tests DIRECTORY PROPERTY TESTS)
  foreach(test IN LISTS tests)
    set_tests_properties(${test} PROPERTIES
      ENVIRONMENT "GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0")
  endforeach()
endif()
