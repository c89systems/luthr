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

find_path(VALGRIND_CLIENT_INCLUDE_DIR valgrind.h PATH_SUFFIXES valgrind
  NO_CMAKE_FIND_ROOT_PATH REQUIRED)
configure_file("${VALGRIND_CLIENT_INCLUDE_DIR}/valgrind.h"
  "${CMAKE_CURRENT_BINARY_DIR}/valgrind-client/valgrind.h" COPYONLY)
luthrum_test_program(reference_test reference_test.c)
set_target_properties(reference_test PROPERTIES C_STANDARD 99)
target_compile_options(reference_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(reference_test PRIVATE luthrum_static)
target_link_options(reference_test PRIVATE -Wl,--wrap=lua_rawgeti)
foreach(backend IN ITEMS raw facade)
  foreach(case IN ITEMS registry table oom hook)
    add_test(NAME reference_${backend}_${case} COMMAND reference_test ${backend}_${case})
    set_tests_properties(reference_${backend}_${case}
      PROPERTIES LABELS "runtime;parallel;memcheck" TIMEOUT 60)
  endforeach()
endforeach()
luthrum_test_program(execution_wait_test execution_wait_test.c)
set_target_properties(execution_wait_test PROPERTIES C_STANDARD 99)
target_compile_options(execution_wait_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(execution_wait_test PRIVATE luthrum_static)
target_link_options(execution_wait_test PRIVATE -Wl,--wrap=pthread_cond_wait
  -Wl,--wrap=pthread_cond_broadcast)
foreach(case IN ITEMS empty ordinary fallback managed mixed spurious early signal)
  add_test(NAME execution_wait_${case} COMMAND execution_wait_test ${case})
  set_tests_properties(execution_wait_${case}
    PROPERTIES LABELS "runtime;parallel;memcheck" TIMEOUT 30)
endforeach()
luthrum_test_program(carrier_pin_test carrier_pin_test.c)
set_target_properties(carrier_pin_test PROPERTIES C_STANDARD 99)
target_compile_options(carrier_pin_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(carrier_pin_test PRIVATE luthrum_static)
add_test(NAME carrier_pin_contract COMMAND carrier_pin_test)
set_tests_properties(carrier_pin_contract PROPERTIES
  LABELS "runtime;parallel;memcheck" TIMEOUT 30)
add_test(NAME carrier_pin_wrong_driver COMMAND carrier_pin_test wrong_driver)
set_tests_properties(carrier_pin_wrong_driver PROPERTIES LABELS runtime TIMEOUT 30)
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS prestart cpu catch xpcall native native_oom continuation callback unprotected foreign_vm oom parser parser_oom yield cleanup_error cleanup_wait request_race late
        admission_empty admission_before admission_after admission_race admission_gate admission_oom)
      add_test(NAME task_shutdown_${backend}_${case}_${gc}
        COMMAND task_stop_test shutdown_${backend}_${case}_${gc})
      set_tests_properties(task_shutdown_${backend}_${case}_${gc}
        PROPERTIES LABELS "parallel;runtime;memcheck" TIMEOUT 120)
    endforeach()
    foreach(case IN ITEMS recv send await driver pinned cleanup prestop prestop_send peer peer_head
        oom_recv oom_send oom_await oom_driver race race_repeat race_close race_finalize)
      add_test(NAME wait_shutdown_${backend}_${case}_${gc}
        COMMAND stop_wait_test shutdown_${backend}_${case}_${gc})
      set_tests_properties(wait_shutdown_${backend}_${case}_${gc}
        PROPERTIES LABELS "parallel;runtime;memcheck" TIMEOUT 120)
    endforeach()
    foreach(case IN ITEMS recv send await driver migrate_recv migrate_send migrate_await migrate_driver
        migrate_movealloc_recv migrate_movealloc_await)
      add_test(NAME native_scheduler_shutdown_${backend}_${case}_${gc}
        COMMAND native_scheduler_test stop_shutdown_${backend}_${case}_${gc})
      set_tests_properties(native_scheduler_shutdown_${backend}_${case}_${gc}
        PROPERTIES LABELS "parallel;memcheck" TIMEOUT 180)
    endforeach()
  endforeach()
endforeach()
luthrum_test_program(stop_wait_test stop_wait_test.c)
set_target_properties(stop_wait_test PROPERTIES C_STANDARD 99)
target_compile_options(stop_wait_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(stop_wait_test PRIVATE luthrum_static)
target_link_options(stop_wait_test PRIVATE -Wl,--wrap=pthread_cond_wait
  -Wl,--wrap=luaQ_waitstop -Wl,--wrap=luaQ_interrupt)
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS recv send await driver pinned cleanup prestop prestop_send peer peer_head
        oom_recv oom_send oom_await oom_driver race race_repeat race_close race_finalize)
      add_test(NAME stop_wait_${backend}_${case}_${gc}
        COMMAND stop_wait_test ${backend}_${case}_${gc})
      set_tests_properties(stop_wait_${backend}_${case}_${gc}
        PROPERTIES LABELS "parallel;runtime;memcheck" TIMEOUT 120)
    endforeach()
  endforeach()
endforeach()
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS recv send await driver migrate_recv migrate_send migrate_await migrate_driver
        early_recv early_await early_driver migrate_early_recv migrate_early_driver
        migrate_movealloc_recv migrate_movealloc_await)
      add_test(NAME native_scheduler_stop_${backend}_${case}_${gc}
        COMMAND native_scheduler_test stop_${backend}_${case}_${gc})
      set_tests_properties(native_scheduler_stop_${backend}_${case}_${gc}
        PROPERTIES LABELS "parallel;memcheck" TIMEOUT 180)
    endforeach()
  endforeach()
endforeach()
luthrum_test_program(task_stop_test task_stop_test.c)
set_target_properties(task_stop_test PROPERTIES C_STANDARD 99)
target_compile_options(task_stop_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(task_stop_test PRIVATE luthrum_static)
target_link_options(task_stop_test PRIVATE -Wl,--wrap=lua_resume)
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS prestart cpu catch xpcall native native_oom continuation callback unprotected foreign_vm oom parser parser_oom yield cleanup_error cleanup_wait request_race late)
      add_test(NAME task_stop_${backend}_${case}_${gc}
        COMMAND task_stop_test ${backend}_${case}_${gc})
      set_tests_properties(task_stop_${backend}_${case}_${gc}
        PROPERTIES LABELS "parallel;runtime;memcheck" TIMEOUT 120)
    endforeach()
  endforeach()
endforeach()
luthrum_test_program(file_library_test file_library_test.c)
set_target_properties(file_library_test PROPERTIES C_STANDARD 99)
target_compile_options(file_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(file_library_test PRIVATE luthrum_static)
target_link_options(file_library_test PRIVATE -Wl,--wrap=fopen -Wl,--wrap=fopen64 -Wl,--wrap=fclose)
if(LUTHRUM_TARGET STREQUAL "x86_64-linux-gnu" AND NOT TARGET debug_worker_stack)
  target_compile_definitions(file_library_test PRIVATE LUTHRUM_TEST_SMALL_NATIVE_STACK)
endif()
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS semantics errors binary yield bom stdin snapshot admission oom stack)
      add_test(NAME file_library_${backend}_${case}_${gc}
        COMMAND file_library_test ${backend}_${case}_${gc} "${CMAKE_CURRENT_BINARY_DIR}")
      set_tests_properties(file_library_${backend}_${case}_${gc}
        PROPERTIES LABELS "runtime;memcheck" TIMEOUT 120)
      if(case STREQUAL "stack")
        set_tests_properties(file_library_${backend}_${case}_${gc}
          PROPERTIES LABELS "stack;runtime;memcheck")
      endif()
    endforeach()
  endforeach()
endforeach()
luthrum_test_program(package_library_test package_library_test.c)
set_target_properties(package_library_test PROPERTIES C_STANDARD 99)
target_compile_options(package_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(package_library_test PRIVATE luthrum_static)
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS semantics errors metamethods bindings snapshot admission parallel oom)
      add_test(NAME package_library_${backend}_${case}_${gc}
        COMMAND package_library_test ${backend}_${case}_${gc})
      set_tests_properties(package_library_${backend}_${case}_${gc}
        PROPERTIES LABELS "runtime;memcheck" TIMEOUT 120)
      if(case STREQUAL "parallel")
        set_tests_properties(package_library_${backend}_${case}_${gc}
          PROPERTIES LABELS "parallel;runtime;memcheck")
      endif()
    endforeach()
  endforeach()
endforeach()
luthrum_test_program(load_library_test load_library_test.c)
set_target_properties(load_library_test PROPERTIES C_STANDARD 99)
target_compile_options(load_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(load_library_test PRIVATE luthrum_static)
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS semantics errors snapshot reader_snapshot binary_snapshot admission stream oom)
      add_test(NAME load_library_${backend}_${case}_${gc}
        COMMAND load_library_test ${backend}_${case}_${gc})
      set_tests_properties(load_library_${backend}_${case}_${gc}
        PROPERTIES LABELS "runtime;memcheck" TIMEOUT 120)
    endforeach()
  endforeach()
endforeach()
luthrum_test_program(dump_library_test dump_library_test.c)
set_target_properties(dump_library_test PROPERTIES C_STANDARD 99)
target_compile_options(dump_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dump_library_test PRIVATE luthrum_static)
target_link_options(dump_library_test PRIVATE -Wl,--wrap=lua_stringtonumber)
if(LUTHRUM_TARGET STREQUAL "x86_64-linux-gnu" AND NOT TARGET debug_worker_stack)
  # GNU Release only: Debug and native musl retain their default worker stacks.
  target_compile_definitions(dump_library_test PRIVATE LUTHRUM_TEST_SMALL_NATIVE_STACK)
endif()
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS semantics errors bindings snapshot public_snapshot private_snapshot admission oom number_snapshot stack)
      add_test(NAME dump_library_${backend}_${case}_${gc}
        COMMAND dump_library_test ${backend}_${case}_${gc})
      set_tests_properties(dump_library_${backend}_${case}_${gc}
        PROPERTIES LABELS "runtime;memcheck" TIMEOUT 60)
      if(case STREQUAL "stack")
        set_tests_properties(dump_library_${backend}_${case}_${gc}
          PROPERTIES LABELS "stack;runtime;memcheck")
      endif()
    endforeach()
  endforeach()
endforeach()
luthrum_test_program(string_library_test string_library_test.c)
set_target_properties(string_library_test PROPERTIES C_STANDARD 99)
target_compile_options(string_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(string_library_test PRIVATE luthrum_static)
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS semantics errors bindings snapshot oom)
      add_test(NAME string_library_${backend}_${case}_${gc}
        COMMAND string_library_test ${backend}_${case}_${gc})
      set_tests_properties(string_library_${backend}_${case}_${gc}
        PROPERTIES LABELS "runtime;memcheck" TIMEOUT 120)
    endforeach()
  endforeach()
endforeach()
luthrum_test_program(pattern_library_test pattern_library_test.c)
set_target_properties(pattern_library_test PROPERTIES C_STANDARD 99)
target_compile_options(pattern_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(pattern_library_test PRIVATE luthrum_static)
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS semantics errors bindings oom stack snapshot_find
        snapshot_match snapshot_gsub snapshot_iterator snapshot_values callback_snapshot parallel stress)
      add_test(NAME pattern_library_${backend}_${case}_${gc}
        COMMAND pattern_library_test ${backend}_${case}_${gc})
      set_tests_properties(pattern_library_${backend}_${case}_${gc}
        PROPERTIES LABELS "runtime;memcheck" TIMEOUT 30)
      if(case MATCHES "^(parallel|stress)$")
        set_tests_properties(pattern_library_${backend}_${case}_${gc}
          PROPERTIES LABELS "parallel;runtime;memcheck" TIMEOUT 120)
      endif()
    endforeach()
  endforeach()
endforeach()
luthrum_test_program(utf8_library_test utf8_library_test.c)
set_target_properties(utf8_library_test PROPERTIES C_STANDARD 99)
target_compile_options(utf8_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(utf8_library_test PRIVATE luthrum_static)
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS semantics errors bindings oom snapshot)
      add_test(NAME utf8_library_${backend}_${case}_${gc}
        COMMAND utf8_library_test ${backend}_${case}_${gc})
      set_tests_properties(utf8_library_${backend}_${case}_${gc}
        PROPERTIES LABELS "runtime;memcheck" TIMEOUT 30)
    endforeach()
  endforeach()
endforeach()
luthrum_test_program(math_library_test math_library_test.c)
set_target_properties(math_library_test PROPERTIES C_STANDARD 99)
target_compile_options(math_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(math_library_test PRIVATE luthrum_static)
target_link_options(math_library_test PRIVATE -Wl,--wrap=luaA_begin)
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS sequence errors bindings oom capture parallel_full parallel_bounded reseed)
      add_test(NAME math_library_${backend}_${case}_${gc}
        COMMAND math_library_test ${backend}_${case}_${gc})
      set_tests_properties(math_library_${backend}_${case}_${gc}
        PROPERTIES LABELS "parallel;runtime;memcheck" TIMEOUT 30)
    endforeach()
  endforeach()
endforeach()
luthrum_test_program(task_ownership_test task_ownership_test.c)
set_target_properties(task_ownership_test PROPERTIES C_STANDARD 99)
target_compile_options(task_ownership_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(task_ownership_test PRIVATE luthrum_static)
target_link_options(task_ownership_test PRIVATE -Wl,--wrap=lua_checkstack
  -Wl,--wrap=pthread_cond_wait)
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS normal active yield error finish_error cleanup_error selfclose oom retry race
        await_ready await_retry await_race await_notify await_notify_rollback)
      add_test(NAME task_ownership_${backend}_${case}_${gc}
        COMMAND task_ownership_test ${backend}_${case}_${gc})
      set_tests_properties(task_ownership_${backend}_${case}_${gc}
        PROPERTIES LABELS "parallel;runtime;memcheck" TIMEOUT 30)
    endforeach()
  endforeach()
endforeach()
luthrum_test_program(coroutine_operation_test coroutine_operation_test.c)
set_target_properties(coroutine_operation_test PROPERTIES C_STANDARD 99)
target_compile_options(coroutine_operation_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(coroutine_operation_test PRIVATE luthrum_static)
target_link_options(coroutine_operation_test PRIVATE -Wl,--wrap=lua_checkstack
  -Wl,--wrap=lua_resume -Wl,--wrap=lua_closethread -Wl,--wrap=luaW_restore)
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS resume_prepare resume_prepare_oom resume_result wrap_prepare wrap_result
        close_prepare close_result close_cleanup close_reentrant active_yield root_normal root_recover
        root_error root_handler_error root_oom root_selfclose root_construction bridge_close)
      add_test(NAME coroutine_operation_${backend}_${case}_${gc}
        COMMAND coroutine_operation_test ${backend}_${case}_${gc})
      set_tests_properties(coroutine_operation_${backend}_${case}_${gc}
        PROPERTIES LABELS "parallel;runtime;memcheck" TIMEOUT 30)
    endforeach()
  endforeach()
endforeach()
luthrum_test_program(control_snapshot_test control_snapshot_test.c)
set_target_properties(control_snapshot_test PROPERTIES C_STANDARD 99)
target_compile_options(control_snapshot_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(control_snapshot_test PRIVATE luthrum_static)
target_link_options(control_snapshot_test PRIVATE -Wl,--wrap=lua_tothread)
foreach(case IN ITEMS raw_incremental raw_generational facade_incremental facade_generational)
  add_test(NAME control_snapshot_${case} COMMAND control_snapshot_test ${case})
  set_tests_properties(control_snapshot_${case} PROPERTIES LABELS "parallel;runtime;memcheck" TIMEOUT 30)
endforeach()
luthrum_test_program(pipe_contract_test pipe_contract_test.c)
set_target_properties(pipe_contract_test PROPERTIES C_STANDARD 99)
target_compile_options(pipe_contract_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(pipe_contract_test PRIVATE luthrum_static)
target_link_options(pipe_contract_test PRIVATE
  -Wl,--wrap=pthread_mutex_init -Wl,--wrap=pthread_cond_init)
foreach(case IN ITEMS managed contract transfer_oom corrupt create_oom mutex condition
    roots_incremental roots_generational identity_incremental identity_generational)
  add_test(NAME pipe_contract_${case} COMMAND pipe_contract_test ${case})
  set_tests_properties(pipe_contract_${case} PROPERTIES LABELS "runtime;memcheck" TIMEOUT 30)
endforeach()
set_tests_properties(pipe_contract_identity_incremental pipe_contract_identity_generational
  PROPERTIES LABELS "parallel;runtime;memcheck")
luthrum_test_program(callback_entry_test callback_entry_test.c)
set_target_properties(callback_entry_test PROPERTIES C_STANDARD 99)
target_compile_options(callback_entry_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(callback_entry_test PRIVATE luthrum_static)
target_link_options(callback_entry_test PRIVATE
  -Wl,--wrap=pthread_mutex_init -Wl,--wrap=pthread_cond_init)
foreach(case IN ITEMS mutex condition create_oom create_oom_stack create_oom_message invoke_oom
    roots_incremental roots_generational errors reserve)
  add_test(NAME callback_entry_${case} COMMAND callback_entry_test ${case})
  set_tests_properties(callback_entry_${case} PROPERTIES LABELS "runtime;memcheck" TIMEOUT 30)
endforeach()
luthrum_test_program(fiber_vm_test fiber_vm_test.c)
target_sources(fiber_vm_test PRIVATE private/context.c private/context_x86_64.S)
set_target_properties(fiber_vm_test PROPERTIES C_STANDARD 99)
target_include_directories(fiber_vm_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/valgrind-client")
target_compile_options(fiber_vm_test PRIVATE
  "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-mtls-dialect=gnu2>")
target_link_libraries(fiber_vm_test PRIVATE luthrum_static)
foreach(case IN ITEMS incremental generational)
  add_test(NAME fiber_vm_${case} COMMAND fiber_vm_test ${case})
  set_tests_properties(fiber_vm_${case} PROPERTIES LABELS "parallel;memcheck" TIMEOUT 60)
endforeach()
add_test(NAME fiber_stack_guards COMMAND fiber_vm_test guards)
set_tests_properties(fiber_stack_guards PROPERTIES LABELS stack TIMEOUT 10)
add_test(NAME fiber_execution_boundary COMMAND fiber_vm_test execution_boundary)
set_tests_properties(fiber_execution_boundary PROPERTIES LABELS runtime TIMEOUT 10)

luthrum_test_program(native_scheduler_test native_scheduler_test.c)
target_sources(native_scheduler_test PRIVATE private/context.c private/context_x86_64.S)
set_target_properties(native_scheduler_test PROPERTIES C_STANDARD 99)
target_include_directories(native_scheduler_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/valgrind-client")
target_compile_options(native_scheduler_test PRIVATE
  "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-mtls-dialect=gnu2>")
target_link_libraries(native_scheduler_test PRIVATE luthrum_static)
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS normal nested error stop shutdown cpu yield nested_stop nested_shutdown
        stop_close_error normal_close_error movealloc_normal movealloc_nested_stop oom nested_oom)
      add_test(NAME native_scheduler_affinity_${backend}_${case}_${gc}
        COMMAND native_scheduler_test affinity_${backend}_${case}_${gc})
      set_tests_properties(native_scheduler_affinity_${backend}_${case}_${gc}
        PROPERTIES LABELS "runtime;parallel;memcheck" TIMEOUT 180)
    endforeach()
  endforeach()
endforeach()
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS normal migrate early error callback callback_migrate callback_error
        driver driver_migrate library library_migrate library_callback library_error
        utf8 utf8_migrate utf8_callback utf8_movealloc_callback utf8_error
        pattern pattern_migrate pattern_callback pattern_movealloc_callback pattern_error
        strings strings_migrate strings_callback strings_movealloc_callback strings_error
        dump dump_migrate dump_callback dump_movealloc_callback dump_error
        load load_migrate load_callback load_movealloc_callback load_error
        package package_migrate package_callback package_movealloc_callback package_error)
      add_test(NAME native_scheduler_task_${backend}_${case}_${gc}
        COMMAND native_scheduler_test task_${backend}_${case}_${gc})
      set_tests_properties(native_scheduler_task_${backend}_${case}_${gc}
        PROPERTIES LABELS "parallel;memcheck" TIMEOUT 60)
      if(case MATCHES "^(utf8|pattern|strings|dump|load|package)")
        # Forced carrier switches and large result sets under Valgrind.
        set_tests_properties(native_scheduler_task_${backend}_${case}_${gc}
          PROPERTIES TIMEOUT 180)
      endif()
    endforeach()
    foreach(case IN ITEMS file file_migrate file_callback file_movealloc_callback file_error)
      add_test(NAME native_scheduler_task_${backend}_${case}_${gc}
        COMMAND native_scheduler_test task_${backend}_${case}_${gc}
          "${CMAKE_CURRENT_SOURCE_DIR}/dofile_body.lua")
      set_tests_properties(native_scheduler_task_${backend}_${case}_${gc}
        PROPERTIES LABELS "parallel;memcheck" TIMEOUT 180)
    endforeach()
  endforeach()
endforeach()
foreach(case IN ITEMS incremental generational budget_incremental budget_generational
    trace_incremental trace_generational wait_object_incremental wait_object_generational
    wait_gate_incremental wait_gate_generational wait_gc_incremental wait_gc_generational
    wait_early_incremental wait_early_generational wait_migrate_object_incremental
    wait_migrate_object_generational wait_migrate_gc_incremental wait_migrate_gc_generational
    resource_incremental resource_generational foreign_incremental foreign_generational
    foreign_revoke_incremental foreign_revoke_generational
    foreign_budget_incremental foreign_budget_generational
    foreign_open_incremental foreign_open_generational
    stream_single_incremental stream_single_generational
    stream_migrate_incremental stream_migrate_generational
    stream_native_incremental stream_native_generational
    stream_foreign_incremental stream_foreign_generational
    stream_abort_send_migrate_incremental stream_abort_send_migrate_generational
    stream_abort_recv_incremental stream_abort_recv_generational
    stream_close_send_incremental stream_close_send_generational
    stream_close_recv_incremental stream_close_recv_generational
    stream_control_incremental stream_control_generational
    stream_control_migrate_incremental stream_control_migrate_generational
    stream_control_native_incremental stream_control_native_generational
    stream_control_facade_incremental stream_control_facade_generational
    stream_control_facade_migrate_incremental stream_control_facade_migrate_generational
    stream_control_facade_native_incremental stream_control_facade_native_generational
    stream_control_closing_migrate_incremental stream_control_closing_migrate_generational
    stream_control_facade_closing_native_incremental stream_control_facade_closing_native_generational)
  add_test(NAME native_scheduler_${case} COMMAND native_scheduler_test ${case})
  set_tests_properties(native_scheduler_${case} PROPERTIES LABELS "parallel;memcheck" TIMEOUT 60)
endforeach()

luthrum_test_program(parallel_vm_test parallel_vm_test.c)
set_target_properties(parallel_vm_test PROPERTIES C_STANDARD 99)
target_compile_options(parallel_vm_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(parallel_vm_test PRIVATE luthrum_static)
foreach(case IN ITEMS closed open api cycle blocked allocate_stopped allocate_incremental
    allocate_generational allocate_failure allocate_open allocate_open_generational allocate_blocked
    table_stopped table_incremental table_generational table_meta_incremental table_meta_generational
    table_blocked table_error table_failure table_handler_blocked table_yield
    join_stopped join_incremental join_generational join_open_incremental join_open_generational
    capi_stopped capi_incremental capi_generational capi_failure capi_blocked
    capi_registry_incremental capi_registry_generational capi_conversion_incremental
    capi_conversion_generational capi_borrowed)
  add_test(NAME parallel_vm_${case} COMMAND parallel_vm_test ${case})
  set_tests_properties(parallel_vm_${case} PROPERTIES LABELS "parallel;memcheck" TIMEOUT 30)
endforeach()

luthrum_test_program(cross_api_test cross_api_test.c)
set_target_properties(cross_api_test PROPERTIES C_STANDARD 99)
target_compile_options(cross_api_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(cross_api_test PRIVATE luthrum_static)
target_link_options(cross_api_test PRIVATE -Wl,--wrap=pthread_cond_wait)
foreach(case IN ITEMS moves_incremental moves_generational moves_bulk move_wait_gc
    resume_parent_gc close_parent_gc foreign_error forward_close_gc)
  add_test(NAME cross_api_${case} COMMAND cross_api_test ${case})
  set_tests_properties(cross_api_${case} PROPERTIES LABELS "parallel;memcheck" TIMEOUT 30)
endforeach()

luthrum_test_program(parallel_hook_test parallel_hook_test.c)
set_target_properties(parallel_hook_test PROPERTIES C_STANDARD 99)
target_compile_options(parallel_hook_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(parallel_hook_test PRIVATE luthrum_static)
foreach(case IN ITEMS replace_incremental replace_generational blocked signal yield counts)
  add_test(NAME parallel_hook_${case} COMMAND parallel_hook_test ${case})
  set_tests_properties(parallel_hook_${case} PROPERTIES LABELS "parallel;memcheck" TIMEOUT 30)
endforeach()

luthrum_test_program(parallel_policy_test parallel_policy_test.c)
set_target_properties(parallel_policy_test PROPERTIES C_STANDARD 99)
target_compile_options(parallel_policy_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(parallel_policy_test PRIVATE luthrum_static)
target_link_options(parallel_policy_test PRIVATE -Wl,--wrap=lua_sethook)
foreach(case IN ITEMS budget_incremental budget_generational budget_trace trace_replace
    trace_blocked wrappers allocation_cap warning_pairs external_failure)
  add_test(NAME parallel_policy_${case} COMMAND parallel_policy_test ${case})
  set_tests_properties(parallel_policy_${case} PROPERTIES LABELS "parallel;memcheck" TIMEOUT 30)
endforeach()

luthrum_test_program(callback_config_test callback_config_test.c)
set_target_properties(callback_config_test PROPERTIES C_STANDARD 99)
target_compile_options(callback_config_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(callback_config_test PRIVATE luthrum_static)
foreach(case IN ITEMS replace blocked)
  add_test(NAME callback_config_${case} COMMAND callback_config_test ${case})
  set_tests_properties(callback_config_${case} PROPERTIES LABELS "parallel;memcheck" TIMEOUT 30)
endforeach()

luthrum_test_program(gc_rendezvous_test gc_rendezvous_test.c)
set_target_properties(gc_rendezvous_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_rendezvous_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_rendezvous_test PRIVATE luthrum_static)
foreach(case IN ITEMS managed_incremental managed_generational native_incremental native_generational
    contended_incremental contended_generational)
  add_test(NAME gc_rendezvous_${case} COMMAND gc_rendezvous_test ${case})
  set_tests_properties(gc_rendezvous_${case} PROPERTIES LABELS "parallel;memcheck" TIMEOUT 30)
endforeach()

luthrum_test_program(synchronization_failure_test synchronization_failure_test.c)
set_target_properties(synchronization_failure_test PROPERTIES C_STANDARD 99)
target_compile_options(synchronization_failure_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(synchronization_failure_test PRIVATE luthrum_static)
target_link_options(synchronization_failure_test PRIVATE
  -Wl,--wrap=pthread_mutex_init -Wl,--wrap=pthread_cond_init
  -Wl,--wrap=pthread_mutex_destroy -Wl,--wrap=pthread_cond_destroy)
foreach(case IN ITEMS main_mutex main_condition world_mutex world_condition thread_mutex thread_condition
    closed_capture open_capture facade_policy_mutex facade_trace_mutex facade_state_mutex
    facade_world_mutex)
  add_test(NAME synchronization_failure_${case} COMMAND synchronization_failure_test ${case})
  set_tests_properties(synchronization_failure_${case} PROPERTIES LABELS "runtime;memcheck" TIMEOUT 10)
endforeach()

luthrum_test_program(world_unwind_test world_unwind_test.c)
set_target_properties(world_unwind_test PROPERTIES C_STANDARD 99)
target_compile_options(world_unwind_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(world_unwind_test PRIVATE luthrum_static)
target_link_options(world_unwind_test PRIVATE -Wl,--wrap=luaX_lockstate)
foreach(case IN ITEMS warning_error warning_oom forwarded_gc)
  add_test(NAME world_unwind_${case} COMMAND world_unwind_test ${case})
  set_tests_properties(world_unwind_${case} PROPERTIES LABELS "runtime;memcheck" TIMEOUT 10)
endforeach()

luthrum_test_program(api_ownership_test api_ownership_test.c)
set_target_properties(api_ownership_test PROPERTIES C_STANDARD 99)
target_compile_options(api_ownership_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(api_ownership_test PRIVATE luthrum_static)
target_link_options(api_ownership_test PRIVATE
  -Wl,--wrap=luaX_lockstate -Wl,--wrap=luaX_unlockstate -Wl,--wrap=luaA_end)
foreach(case IN ITEMS string_pointer string_length dump_entry rawset_entry capture_selfcopy
    vm_newtable_incremental vm_newtable_generational vm_concat_incremental
    vm_concat_generational vm_closure_incremental vm_closure_generational)
  add_test(NAME api_ownership_${case} COMMAND api_ownership_test ${case})
  set_tests_properties(api_ownership_${case} PROPERTIES LABELS "runtime;memcheck" TIMEOUT 10)
endforeach()
set_tests_properties(api_ownership_capture_selfcopy PROPERTIES LABELS "parallel;runtime;memcheck")

luthrum_test_program(native_roots_test native_roots_test.c)
set_target_properties(native_roots_test PROPERTIES C_STANDARD 99)
target_compile_options(native_roots_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(native_roots_test PRIVATE luthrum_static)
foreach(case IN ITEMS retain_incremental retain_generational nested_error yield
    self_close allocation_failure many_roots many_roots_generational
    forwarded_error panic replace graph publish)
  add_test(NAME native_roots_${case} COMMAND native_roots_test ${case})
  set_tests_properties(native_roots_${case} PROPERTIES LABELS "runtime;memcheck" TIMEOUT 10)
endforeach()

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

add_subdirectory(e2e)

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
