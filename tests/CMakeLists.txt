if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND LUTHR_TARGET STREQUAL "x86_64-linux-gnu")
  add_library(debug_worker_stack OBJECT debug_worker_stack_init.c)
  luthr_c89(debug_worker_stack)
  target_link_libraries(debug_worker_stack PRIVATE Threads::Threads)
endif()

function(luthr_test_program name source)
  add_executable(${name} ${source})
  luthr_local_runtime(${name})
  if(TARGET debug_worker_stack)
    target_link_libraries(${name} PRIVATE debug_worker_stack)
  endif()
endfunction()

if(LUTHR_THREAD_SANITIZER)
  luthr_test_program(race_detector_test race_detector_test.c)
  set_target_properties(race_detector_test PROPERTIES C_STANDARD 99)
  target_compile_options(race_detector_test PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(race_detector_test PRIVATE luthr_shared)
  add_test(NAME race_detector COMMAND "${CMAKE_COMMAND}"
    "-Dprogram=$<TARGET_FILE:race_detector_test>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectRace.cmake")
  set_tests_properties(race_detector PROPERTIES LABELS "parallel;unit" TIMEOUT 25)
endif()

luthr_test_program(cpu_capacity_test cpu_capacity_test.c)
target_sources(cpu_capacity_test PRIVATE "${PROJECT_SOURCE_DIR}/src/cpu_capacity.c")
set_target_properties(cpu_capacity_test PROPERTIES C_STANDARD 99)
target_compile_options(cpu_capacity_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(cpu_capacity_test PRIVATE Threads::Threads)
include("${CMAKE_CURRENT_SOURCE_DIR}/CapacityFixtures.cmake")

luthr_test_program(public_tasks_test public_tasks_test.c)
luthr_c89(public_tasks_test)
target_link_libraries(public_tasks_test PRIVATE luthr_static)
foreach(case IN ITEMS results prepared parallel_load budget raw_policy traces blocked_trace defaults nested shutdown
    callback callback_budget callback_shutdown callback_oom pin pin_cancel oom collect_oom automatic load_error raw_vm)
  add_test(NAME public_tasks_${case} COMMAND public_tasks_test ${case})
  set_tests_properties(public_tasks_${case}
    PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 20)
endforeach()

luthr_test_program(sdk_consumer_smoke consumer/main.c)
luthr_c89(sdk_consumer_smoke)
target_link_libraries(sdk_consumer_smoke PRIVATE luthr_shared)
add_test(NAME sdk_consumer_smoke COMMAND sdk_consumer_smoke)
set_tests_properties(sdk_consumer_smoke PROPERTIES LABELS "unit;facade;memcheck" TIMEOUT 10)

luthr_test_program(native_sdk_test native_sdk_test.c)
set_target_properties(native_sdk_test PROPERTIES C_STANDARD 99)
target_compile_options(native_sdk_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(native_sdk_test PRIVATE luthr_shared)
add_test(NAME native_sdk COMMAND native_sdk_test)
set_tests_properties(native_sdk PROPERTIES LABELS "unit;runtime;memcheck" TIMEOUT 10)

luthr_test_program(public_tasks_shared public_tasks_test.c)
luthr_c89(public_tasks_shared)
target_link_libraries(public_tasks_shared PRIVATE luthr_shared)
foreach(case IN ITEMS results callback_budget pin_cancel)
  add_test(NAME public_tasks_shared_${case} COMMAND public_tasks_shared ${case})
  set_tests_properties(public_tasks_shared_${case}
    PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 20)
endforeach()

luthr_test_program(scheduler_test scheduler_test.c)
set_target_properties(scheduler_test PROPERTIES C_STANDARD 99)
target_compile_options(scheduler_test PRIVATE -Wall -Wextra -Werror -mtls-dialect=gnu2)
target_link_libraries(scheduler_test PRIVATE luthr_static)
target_link_options(scheduler_test PRIVATE -Wl,--wrap=pthread_mutex_lock
  -Wl,--wrap=luaQ_wait
  -Wl,--wrap=luaX_poll -Wl,--wrap=luaCT_pause -Wl,--wrap=pthread_mutex_init
  -Wl,--wrap=pthread_cond_init -Wl,--wrap=pthread_cond_wait -Wl,--wrap=pthread_create -Wl,--wrap=mmap
  -Wl,--wrap=mprotect -Wl,--wrap=munmap)
foreach(case IN ITEMS fairness parallel no_requests early_incremental early_generational
    native native_reentry shutdown yield saturation cancel_admission startup_failures callback_progress
    parked cancel_parked idle_notifications idle_affinity metadata_progress)
  add_test(NAME scheduler_${case} COMMAND scheduler_test ${case})
  set_tests_properties(scheduler_${case}
    PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(native_lifecycle_test native_lifecycle_test.c)
set_target_properties(native_lifecycle_test PROPERTIES C_STANDARD 99)
target_compile_options(native_lifecycle_test PRIVATE -Wall -Wextra -Werror -mtls-dialect=gnu2)
target_link_libraries(native_lifecycle_test PRIVATE luthr_static)
foreach(case IN ITEMS affinity affinity_cancel opaque_wait opaque_cancel nested wait
    shutdown revoke marshal_error oom late retained_slot foreign_fairness opaque_fairness stream
    stream_cancel stream_shutdown)
  add_test(NAME native_lifecycle_${case} COMMAND native_lifecycle_test ${case})
  set_tests_properties(native_lifecycle_${case}
    PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 15)
endforeach()

luthr_test_program(capture_cells_test capture_cells_test.c)
set_target_properties(capture_cells_test PROPERTIES C_STANDARD 99)
target_compile_options(capture_cells_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(capture_cells_test PRIVATE luthr_static)
target_link_options(capture_cells_test PRIVATE -Wl,--wrap=pthread_mutex_lock
  -Wl,--wrap=luaA_begin -Wl,--wrap=luaA_end
  -Wl,--wrap=luaF_readcapture -Wl,--wrap=luaF_writecapture -Wl,--wrap=luaF_storelocals
  -Wl,--wrap=luaF_loadlocal -Wl,--wrap=luaF_loadrange -Wl,--wrap=luaX_pollrequests)
foreach(case IN ITEMS independent readers access uncaptured operands semantics incremental generational
    shadow_incremental shadow_generational)
  add_test(NAME capture_cells_${case} COMMAND capture_cells_test ${case})
  set_tests_properties(capture_cells_${case}
    PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(execution_context_test execution_context_test.c)
set_target_properties(execution_context_test PROPERTIES C_STANDARD 99)
target_compile_options(execution_context_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(execution_context_test PRIVATE luthr_static)
target_link_options(execution_context_test PRIVATE -Wl,--wrap=luaE_getnativestackcontext)
foreach(case IN ITEMS host managed borrow pair foreign publication)
  add_test(NAME execution_context_${case} COMMAND execution_context_test ${case})
  set_tests_properties(execution_context_${case}
    PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(gc_pacing_test gc_pacing_test.c)
set_target_properties(gc_pacing_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_pacing_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_pacing_test PRIVATE luthr_static)
target_link_options(gc_pacing_test PRIVATE -Wl,--wrap=luaC_tracebatch
  -Wl,--wrap=luaQ_poll)
foreach(case IN ITEMS overdue explicit overflow fast persistent_incremental
    persistent_generational refresh_incremental refresh_generational
    borrowed_incremental borrowed_generational bounded_incremental
    bounded_generational idle_incremental idle_generational
    pressure_incremental pressure_generational unavailable_incremental
    unavailable_generational sweep_growth stopped_incremental
    stopped_generational unwind_incremental unwind_generational
    closing_incremental closing_generational closing_pending_incremental
    closing_pending_generational)
  add_test(NAME gc_pacing_${case} COMMAND gc_pacing_test ${case})
  set_tests_properties(gc_pacing_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(allocation_pool_test allocation_pool_test.c)
set_target_properties(allocation_pool_test PROPERTIES C_STANDARD 99)
target_compile_options(allocation_pool_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(allocation_pool_test PRIVATE luthr_static)
target_link_options(allocation_pool_test PRIVATE -Wl,--wrap=luaW_heapenter
  -Wl,--wrap=luaMP_initpage)
foreach(case IN ITEMS serial parallel batch cap init_gc control_lifetime remote_returns)
  add_test(NAME allocation_pool_${case} COMMAND allocation_pool_test ${case})
  set_tests_properties(allocation_pool_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(page_ownership_test page_ownership_test.c)
set_target_properties(page_ownership_test PROPERTIES C_STANDARD 99)
target_compile_options(page_ownership_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(page_ownership_test PRIVATE luthr_static)
target_link_options(page_ownership_test PRIVATE -Wl,--wrap=luaW_heapenter
  -Wl,--wrap=luaQ_poll -Wl,--wrap=luaW_stop)
foreach(case IN ITEMS retained pressure task lease_unwind busy_cache remote_returns growth_handoff growth_epoch assist_rebind boundary credit overlap helpers unwind finite)
  add_test(NAME page_ownership_${case} COMMAND page_ownership_test ${case})
  set_tests_properties(page_ownership_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 15)
endforeach()

luthr_test_program(gc_requests_test gc_requests_test.c)
set_target_properties(gc_requests_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_requests_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_requests_test PRIVATE luthr_static)
target_link_options(gc_requests_test PRIVATE -Wl,--wrap=luaW_heapenter
  -Wl,--wrap=luaW_collectenter -Wl,--wrap=luaW_stop)
foreach(case IN ITEMS incremental generational explicit stopped phase_incremental phase_generational)
  add_test(NAME gc_requests_${case} COMMAND gc_requests_test ${case})
  set_tests_properties(gc_requests_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(gc_reclamation_test gc_reclamation_test.c)
set_target_properties(gc_reclamation_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_reclamation_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_reclamation_test PRIVATE luthr_static)
target_link_options(gc_reclamation_test PRIVATE -Wl,--wrap=luaM_free_
  -Wl,--wrap=luaQ_wait -Wl,--wrap=luaC_publish -Wl,--wrap=luaQ_poll)
foreach(collector IN ITEMS incremental generational)
  foreach(case IN ITEMS overlap collect_again handshake assist assist_trace assist_unwind)
    add_test(NAME gc_reclamation_${case}_${collector}
      COMMAND gc_reclamation_test ${case} ${collector})
    set_tests_properties(gc_reclamation_${case}_${collector} PROPERTIES
      LABELS "unit;runtime;parallel;memcheck" TIMEOUT 15)
  endforeach()
endforeach()

luthr_test_program(gc_tracing_test gc_tracing_test.c)
set_target_properties(gc_tracing_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_tracing_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_tracing_test PRIVATE luthr_static)
target_link_options(gc_tracing_test PRIVATE -Wl,--wrap=luaQ_poll
  -Wl,--wrap=luaA_tryscan -Wl,--wrap=luaC_tracebatch -Wl,--wrap=luaW_stop
  -Wl,--wrap=luaM_malloc_)
foreach(case IN ITEMS array hash userdata metatable cclosure capture stack busy parser closure_builder thread_builder thread_builder_error weakvalues weakkeys weakarray resurrected_weak intern cache
    concurrent_incremental concurrent_generational concurrent_minor emergency_incremental emergency_minor help_incremental help_minor help_to_generational help_to_incremental ephemerons parallel_mark parallel_ephemerons parallel_weakvalues)
  add_test(NAME gc_tracing_${case} COMMAND gc_tracing_test ${case})
  set_tests_properties(gc_tracing_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 15)
endforeach()

luthr_test_program(gc_barriers_test gc_barriers_test.c)
set_target_properties(gc_barriers_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_barriers_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_barriers_test PRIVATE luthr_static)
target_link_options(gc_barriers_test PRIVATE -Wl,--wrap=luaW_stop)
foreach(collector IN ITEMS incremental generational)
  foreach(case IN ITEMS repeated forward mixed collisions reset parallel parallel_gc targets)
    add_test(NAME gc_barriers_${case}_${collector}
      COMMAND gc_barriers_test ${case} ${collector})
    set_tests_properties(gc_barriers_${case}_${collector} PROPERTIES
      LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
  endforeach()
endforeach()

luthr_test_program(memory_publication_test memory_publication_test.c)
set_target_properties(memory_publication_test PROPERTIES C_STANDARD 99)
target_compile_options(memory_publication_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(memory_publication_test PRIVATE luthr_static)
target_link_options(memory_publication_test PRIVATE -Wl,--wrap=pthread_mutex_lock
  -Wl,--wrap=luaW_enter -Wl,--wrap=luaW_leave -Wl,--wrap=luaX_poll -Wl,--wrap=luaQ_wait
  -Wl,--wrap=luaM_free_)
target_link_options(memory_publication_test PRIVATE -Wl,--wrap=luaW_heapenter)
foreach(case IN ITEMS no_requests stack_admission allocation_admission stack_contention intern_hits ordinary_operations allocation_cache marking_cache coalesced_gc handoff paired_handoff late waiting
    totals construction weak_resize incremental generational page_growth page_cap page_isolation
    metadata_handoff metadata_spurious metadata_collect metadata_priority metadata_contention stopped_gc refill_recheck)
  add_test(NAME memory_publication_${case} COMMAND memory_publication_test ${case})
  set_tests_properties(memory_publication_${case}
    PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(reference_test reference_test.c)
set_target_properties(reference_test PROPERTIES C_STANDARD 99)
target_compile_options(reference_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(reference_test PRIVATE luthr_static)
target_link_options(reference_test PRIVATE -Wl,--wrap=lua_rawgeti)
foreach(backend IN ITEMS raw facade)
  foreach(case IN ITEMS registry table oom hook)
    add_test(NAME reference_${backend}_${case} COMMAND reference_test ${backend}_${case})
    set_tests_properties(reference_${backend}_${case}
      PROPERTIES LABELS "runtime;parallel;memcheck" TIMEOUT 60)
  endforeach()
endforeach()
luthr_test_program(execution_wait_test execution_wait_test.c)
set_target_properties(execution_wait_test PROPERTIES C_STANDARD 99)
target_compile_options(execution_wait_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(execution_wait_test PRIVATE luthr_static)
target_link_options(execution_wait_test PRIVATE -Wl,--wrap=pthread_cond_wait
  -Wl,--wrap=pthread_cond_broadcast)
foreach(case IN ITEMS empty ordinary fallback managed mixed spurious early signal)
  add_test(NAME execution_wait_${case} COMMAND execution_wait_test ${case})
  set_tests_properties(execution_wait_${case}
    PROPERTIES LABELS "runtime;parallel;memcheck" TIMEOUT 30)
endforeach()
luthr_test_program(main_stack_test main_stack_test.c)
set_target_properties(main_stack_test PROPERTIES C_STANDARD 99)
target_compile_options(main_stack_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(main_stack_test PRIVATE luthr_static)
foreach(case IN ITEMS main limit worker fork_worker)
  add_test(NAME main_stack_${case} COMMAND main_stack_test ${case})
  set_tests_properties(main_stack_${case} PROPERTIES LABELS "stack;memcheck" TIMEOUT 30)
endforeach()

luthr_test_program(carrier_pin_test carrier_pin_test.c)
set_target_properties(carrier_pin_test PROPERTIES C_STANDARD 99)
target_compile_options(carrier_pin_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(carrier_pin_test PRIVATE luthr_static)
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
luthr_test_program(stop_wait_test stop_wait_test.c)
set_target_properties(stop_wait_test PROPERTIES C_STANDARD 99)
target_compile_options(stop_wait_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(stop_wait_test PRIVATE luthr_static)
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
luthr_test_program(task_stop_test task_stop_test.c)
set_target_properties(task_stop_test PROPERTIES C_STANDARD 99)
target_compile_options(task_stop_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(task_stop_test PRIVATE luthr_static)
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
luthr_test_program(file_library_test file_library_test.c)
set_target_properties(file_library_test PROPERTIES C_STANDARD 99)
target_compile_options(file_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(file_library_test PRIVATE luthr_static)
target_link_options(file_library_test PRIVATE -Wl,--wrap=fopen -Wl,--wrap=fopen64 -Wl,--wrap=fclose)
if(LUTHR_TARGET STREQUAL "x86_64-linux-gnu" AND NOT TARGET debug_worker_stack)
  target_compile_definitions(file_library_test PRIVATE LUTHR_TEST_SMALL_NATIVE_STACK)
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
luthr_test_program(package_library_test package_library_test.c)
set_target_properties(package_library_test PROPERTIES C_STANDARD 99)
target_compile_options(package_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(package_library_test PRIVATE luthr_static)
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
luthr_test_program(load_library_test load_library_test.c)
set_target_properties(load_library_test PROPERTIES C_STANDARD 99)
target_compile_options(load_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(load_library_test PRIVATE luthr_static)
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
luthr_test_program(dump_library_test dump_library_test.c)
set_target_properties(dump_library_test PROPERTIES C_STANDARD 99)
target_compile_options(dump_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dump_library_test PRIVATE luthr_static)
target_link_options(dump_library_test PRIVATE -Wl,--wrap=lua_stringtonumber)
if(LUTHR_TARGET STREQUAL "x86_64-linux-gnu" AND NOT TARGET debug_worker_stack)
  # GNU Release only: Debug and native musl retain their default worker stacks.
  target_compile_definitions(dump_library_test PRIVATE LUTHR_TEST_SMALL_NATIVE_STACK)
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
luthr_test_program(string_library_test string_library_test.c)
set_target_properties(string_library_test PROPERTIES C_STANDARD 99)
target_compile_options(string_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(string_library_test PRIVATE luthr_static)
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
luthr_test_program(pattern_library_test pattern_library_test.c)
set_target_properties(pattern_library_test PROPERTIES C_STANDARD 99)
target_compile_options(pattern_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(pattern_library_test PRIVATE luthr_static)
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
luthr_test_program(utf8_library_test utf8_library_test.c)
set_target_properties(utf8_library_test PROPERTIES C_STANDARD 99)
target_compile_options(utf8_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(utf8_library_test PRIVATE luthr_static)
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
luthr_test_program(math_library_test math_library_test.c)
set_target_properties(math_library_test PROPERTIES C_STANDARD 99)
target_compile_options(math_library_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(math_library_test PRIVATE luthr_static)
target_link_options(math_library_test PRIVATE -Wl,--wrap=luaA_beginpayload)
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
luthr_test_program(task_ownership_test task_ownership_test.c)
set_target_properties(task_ownership_test PROPERTIES C_STANDARD 99)
target_compile_options(task_ownership_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(task_ownership_test PRIVATE luthr_static)
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
luthr_test_program(coroutine_operation_test coroutine_operation_test.c)
set_target_properties(coroutine_operation_test PROPERTIES C_STANDARD 99)
target_compile_options(coroutine_operation_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(coroutine_operation_test PRIVATE luthr_static)
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
luthr_test_program(control_snapshot_test control_snapshot_test.c)
set_target_properties(control_snapshot_test PROPERTIES C_STANDARD 99)
target_compile_options(control_snapshot_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(control_snapshot_test PRIVATE luthr_static)
target_link_options(control_snapshot_test PRIVATE -Wl,--wrap=lua_tothread)
foreach(case IN ITEMS raw_incremental raw_generational facade_incremental facade_generational)
  add_test(NAME control_snapshot_${case} COMMAND control_snapshot_test ${case})
  set_tests_properties(control_snapshot_${case} PROPERTIES LABELS "parallel;runtime;memcheck" TIMEOUT 30)
endforeach()
luthr_test_program(pipe_contract_test pipe_contract_test.c)
set_target_properties(pipe_contract_test PROPERTIES C_STANDARD 99)
target_compile_options(pipe_contract_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(pipe_contract_test PRIVATE luthr_static)
target_link_options(pipe_contract_test PRIVATE
  -Wl,--wrap=pthread_mutex_init -Wl,--wrap=pthread_cond_init)
foreach(case IN ITEMS managed contract transfer_oom corrupt create_oom mutex condition
    roots_incremental roots_generational identity_incremental identity_generational)
  add_test(NAME pipe_contract_${case} COMMAND pipe_contract_test ${case})
  set_tests_properties(pipe_contract_${case} PROPERTIES LABELS "runtime;memcheck" TIMEOUT 30)
endforeach()
set_tests_properties(pipe_contract_identity_incremental pipe_contract_identity_generational
  PROPERTIES LABELS "parallel;runtime;memcheck")
luthr_test_program(callback_entry_test callback_entry_test.c)
set_target_properties(callback_entry_test PROPERTIES C_STANDARD 99)
target_compile_options(callback_entry_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(callback_entry_test PRIVATE luthr_static)
target_link_options(callback_entry_test PRIVATE
  -Wl,--wrap=pthread_mutex_init -Wl,--wrap=pthread_cond_init)
foreach(case IN ITEMS mutex condition create_oom create_oom_stack create_oom_message invoke_oom
    roots_incremental roots_generational errors reserve)
  add_test(NAME callback_entry_${case} COMMAND callback_entry_test ${case})
  set_tests_properties(callback_entry_${case} PROPERTIES LABELS "runtime;memcheck" TIMEOUT 30)
endforeach()
luthr_test_program(fiber_vm_test fiber_vm_test.c)
target_sources(fiber_vm_test PRIVATE private/context_x86_64.S)
set_target_properties(fiber_vm_test PROPERTIES C_STANDARD 99)
target_include_directories(fiber_vm_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/valgrind-client")
target_compile_options(fiber_vm_test PRIVATE
  "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-mtls-dialect=gnu2>")
target_link_libraries(fiber_vm_test PRIVATE luthr_static)
foreach(case IN ITEMS incremental generational)
  add_test(NAME fiber_vm_${case} COMMAND fiber_vm_test ${case})
  set_tests_properties(fiber_vm_${case} PROPERTIES LABELS "parallel;memcheck" TIMEOUT 60)
endforeach()
add_test(NAME fiber_stack_guards COMMAND fiber_vm_test guards)
set_tests_properties(fiber_stack_guards PROPERTIES LABELS stack TIMEOUT 10)
add_test(NAME fiber_execution_boundary COMMAND fiber_vm_test execution_boundary)
set_tests_properties(fiber_execution_boundary PROPERTIES LABELS runtime TIMEOUT 10)

luthr_test_program(native_scheduler_test native_scheduler_test.c)
target_sources(native_scheduler_test PRIVATE private/context_x86_64.S)
set_target_properties(native_scheduler_test PROPERTIES C_STANDARD 99)
target_include_directories(native_scheduler_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/valgrind-client")
target_compile_options(native_scheduler_test PRIVATE
  "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-mtls-dialect=gnu2>")
target_link_libraries(native_scheduler_test PRIVATE luthr_static)
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

luthr_test_program(parallel_vm_test parallel_vm_test.c)
set_target_properties(parallel_vm_test PROPERTIES C_STANDARD 99)
target_compile_options(parallel_vm_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(parallel_vm_test PRIVATE luthr_static)
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

luthr_test_program(cross_api_test cross_api_test.c)
set_target_properties(cross_api_test PROPERTIES C_STANDARD 99)
target_compile_options(cross_api_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(cross_api_test PRIVATE luthr_static)
target_link_options(cross_api_test PRIVATE -Wl,--wrap=pthread_cond_wait)
foreach(case IN ITEMS moves_incremental moves_generational moves_bulk move_wait_gc
    resume_parent_gc close_parent_gc foreign_error forward_close_gc)
  add_test(NAME cross_api_${case} COMMAND cross_api_test ${case})
  set_tests_properties(cross_api_${case} PROPERTIES LABELS "parallel;memcheck" TIMEOUT 30)
endforeach()

luthr_test_program(parallel_hook_test parallel_hook_test.c)
set_target_properties(parallel_hook_test PROPERTIES C_STANDARD 99)
target_compile_options(parallel_hook_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(parallel_hook_test PRIVATE luthr_static)
foreach(case IN ITEMS replace_incremental replace_generational blocked signal yield counts)
  add_test(NAME parallel_hook_${case} COMMAND parallel_hook_test ${case})
  set_tests_properties(parallel_hook_${case} PROPERTIES LABELS "parallel;memcheck" TIMEOUT 30)
endforeach()

luthr_test_program(parallel_policy_test parallel_policy_test.c)
set_target_properties(parallel_policy_test PROPERTIES C_STANDARD 99)
target_compile_options(parallel_policy_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(parallel_policy_test PRIVATE luthr_static)
target_link_options(parallel_policy_test PRIVATE -Wl,--wrap=lua_sethook)
foreach(case IN ITEMS budget_incremental budget_generational budget_trace trace_replace
    trace_blocked wrappers allocation_cap warning_pairs external_failure)
  add_test(NAME parallel_policy_${case} COMMAND parallel_policy_test ${case})
  set_tests_properties(parallel_policy_${case} PROPERTIES LABELS "parallel;memcheck" TIMEOUT 30)
endforeach()

luthr_test_program(callback_config_test callback_config_test.c)
set_target_properties(callback_config_test PROPERTIES C_STANDARD 99)
target_compile_options(callback_config_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(callback_config_test PRIVATE luthr_static)
foreach(case IN ITEMS replace blocked)
  add_test(NAME callback_config_${case} COMMAND callback_config_test ${case})
  set_tests_properties(callback_config_${case} PROPERTIES LABELS "parallel;memcheck" TIMEOUT 30)
endforeach()

luthr_test_program(gc_rendezvous_test gc_rendezvous_test.c)
set_target_properties(gc_rendezvous_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_rendezvous_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_rendezvous_test PRIVATE luthr_static)
foreach(case IN ITEMS managed_incremental managed_generational native_incremental native_generational
    contended_incremental contended_generational)
  add_test(NAME gc_rendezvous_${case} COMMAND gc_rendezvous_test ${case})
  set_tests_properties(gc_rendezvous_${case} PROPERTIES LABELS "parallel;memcheck" TIMEOUT 30)
endforeach()

luthr_test_program(synchronization_failure_test synchronization_failure_test.c)
set_target_properties(synchronization_failure_test PROPERTIES C_STANDARD 99)
target_compile_options(synchronization_failure_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(synchronization_failure_test PRIVATE luthr_static)
target_link_options(synchronization_failure_test PRIVATE
  -Wl,--wrap=pthread_mutex_init -Wl,--wrap=pthread_cond_init
  -Wl,--wrap=pthread_mutex_destroy -Wl,--wrap=pthread_cond_destroy)
foreach(case IN ITEMS main_mutex main_condition world_mutex world_condition thread_mutex thread_condition
    closed_capture open_capture facade_policy_mutex facade_trace_mutex facade_state_mutex
    facade_world_mutex)
  add_test(NAME synchronization_failure_${case} COMMAND synchronization_failure_test ${case})
  set_tests_properties(synchronization_failure_${case} PROPERTIES LABELS "runtime;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(world_unwind_test world_unwind_test.c)
set_target_properties(world_unwind_test PROPERTIES C_STANDARD 99)
target_compile_options(world_unwind_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(world_unwind_test PRIVATE luthr_static)
target_link_options(world_unwind_test PRIVATE -Wl,--wrap=luaX_lockstate)
foreach(case IN ITEMS warning_error warning_oom forwarded_gc)
  add_test(NAME world_unwind_${case} COMMAND world_unwind_test ${case})
  set_tests_properties(world_unwind_${case} PROPERTIES LABELS "runtime;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(api_ownership_test api_ownership_test.c)
set_target_properties(api_ownership_test PROPERTIES C_STANDARD 99)
target_compile_options(api_ownership_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(api_ownership_test PRIVATE luthr_static)
target_link_options(api_ownership_test PRIVATE
  -Wl,--wrap=luaX_lockstate -Wl,--wrap=luaX_unlockstate -Wl,--wrap=luaA_end
  -Wl,--wrap=luaX_pollrequests)
foreach(case IN ITEMS string_pointer string_length dump_entry rawset_entry capture_selfcopy
    vm_newtable_incremental vm_newtable_generational vm_concat_incremental
    vm_concat_generational vm_closure_incremental vm_closure_generational)
  add_test(NAME api_ownership_${case} COMMAND api_ownership_test ${case})
  set_tests_properties(api_ownership_${case} PROPERTIES LABELS "runtime;memcheck" TIMEOUT 10)
endforeach()
set_tests_properties(api_ownership_capture_selfcopy PROPERTIES LABELS "parallel;runtime;memcheck")

luthr_test_program(native_roots_test native_roots_test.c)
set_target_properties(native_roots_test PROPERTIES C_STANDARD 99)
target_compile_options(native_roots_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(native_roots_test PRIVATE luthr_static)
foreach(case IN ITEMS retain_incremental retain_generational nested_error yield
    self_close allocation_failure many_roots many_roots_generational
    forwarded_error panic replace graph publish)
  add_test(NAME native_roots_${case} COMMAND native_roots_test ${case})
  set_tests_properties(native_roots_${case} PROPERTIES LABELS "runtime;memcheck" TIMEOUT 10)
endforeach()

foreach(linkage IN ITEMS static shared)
  luthr_test_program(lua_facade_${linkage} lua_facade_test.c)
  luthr_c89(lua_facade_${linkage})
  target_link_libraries(lua_facade_${linkage} PRIVATE luthr_${linkage})
  add_test(NAME lua_facade_${linkage} COMMAND lua_facade_${linkage})
  set_tests_properties(lua_facade_${linkage} PROPERTIES LABELS "facade;memcheck" TIMEOUT 10)

  luthr_test_program(lua_contract_${linkage} lua_runtime_contract_test.c)
  luthr_c89(lua_contract_${linkage})
  target_link_libraries(lua_contract_${linkage} PRIVATE luthr_${linkage})
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

  luthr_test_program(lua_native_stack_${linkage} lua_native_stack_test.c)
  set_target_properties(lua_native_stack_${linkage} PROPERTIES C_STANDARD 99)
  target_compile_options(lua_native_stack_${linkage} PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(lua_native_stack_${linkage} PRIVATE luthr_${linkage})
  if(LUTHR_TARGET STREQUAL "x86_64-linux-gnu")
    target_compile_definitions(lua_native_stack_${linkage} PRIVATE LUTHR_TEST_SMALL_NATIVE_STACK)
  endif()
  if(linkage STREQUAL "static")
    target_compile_definitions(lua_native_stack_${linkage} PRIVATE LUTHR_TEST_WRAP_BOUNDS)
    target_link_options(lua_native_stack_${linkage} PRIVATE -Wl,--wrap=pthread_getattr_np)
  endif()
  add_test(NAME lua_native_stack_${linkage} COMMAND lua_native_stack_${linkage})
  set_tests_properties(lua_native_stack_${linkage} PROPERTIES LABELS "stack;memcheck" TIMEOUT 30)
endforeach()

if(TARGET debug_worker_stack)
  luthr_test_program(debug_worker_stack_test debug_worker_stack_test.c)
  luthr_c89(debug_worker_stack_test)
  target_link_libraries(debug_worker_stack_test PRIVATE Threads::Threads)
  target_compile_definitions(debug_worker_stack_test PRIVATE LUTHR_TEST_NATIVE_SELF_EXEC)
  add_test(NAME debug_worker_defaults COMMAND debug_worker_stack_test defaults)
  add_test(NAME debug_worker_guard COMMAND debug_worker_stack_test guard)
  set_tests_properties(debug_worker_defaults PROPERTIES LABELS "stack;memcheck" TIMEOUT 10)
  set_tests_properties(debug_worker_guard PROPERTIES LABELS stack TIMEOUT 10)
endif()

luthr_test_program(luthr_test_host "${PROJECT_SOURCE_DIR}/src/runtime/lua.c")
set_target_properties(luthr_test_host PROPERTIES C_STANDARD 99)
target_compile_definitions(luthr_test_host PRIVATE LUA_USE_LINUX)
target_link_libraries(luthr_test_host PRIVATE luthr_shared)
add_test(NAME interpreter COMMAND luthr_test_host "${CMAKE_CURRENT_SOURCE_DIR}/interpreter.lua")
set_tests_properties(interpreter PROPERTIES LABELS "runtime;memcheck" TIMEOUT 10)

add_test(NAME bytecode_compile COMMAND "${LUTHR_RUNTIME_LOADER}"
  --library-path "${CMAKE_SYSROOT}/lib:${CMAKE_SYSROOT}/usr/lib"
  "$<TARGET_FILE:luthrc>" -o "${CMAKE_CURRENT_BINARY_DIR}/interpreter.luac"
  "${CMAKE_CURRENT_SOURCE_DIR}/interpreter.lua")
set_tests_properties(bytecode_compile PROPERTIES FIXTURES_SETUP bytecode LABELS runtime TIMEOUT 10)
add_test(NAME bytecode_execute COMMAND luthr_test_host "${CMAKE_CURRENT_BINARY_DIR}/interpreter.luac")
set_tests_properties(bytecode_execute PROPERTIES FIXTURES_REQUIRED bytecode LABELS "runtime;memcheck" TIMEOUT 10)

add_subdirectory(e2e)

add_test(NAME stage_sdk COMMAND "${CMAKE_COMMAND}"
  "-Dbuild_dir=${PROJECT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/StageSdk.cmake")
set_tests_properties(stage_sdk PROPERTIES FIXTURES_SETUP sdk LABELS install TIMEOUT 20)
add_test(NAME installed_consumers COMMAND "${CMAKE_COMMAND}"
  "-Dbuild_dir=${PROJECT_BINARY_DIR}" "-Dsource_dir=${PROJECT_SOURCE_DIR}"
  "-Dtarget=${LUTHR_TARGET}" "-Dconfig=${CMAKE_BUILD_TYPE}"
  "-Dlifecycle_scripts=${CPKT_LIFECYCLE_SCRIPTS_DIR}"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/InstalledConsumers.cmake")
set_tests_properties(installed_consumers PROPERTIES FIXTURES_REQUIRED sdk LABELS install TIMEOUT 60)

add_test(NAME public_exports COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/exports.sh"
  "${CMAKE_NM}" "$<TARGET_FILE:luthr_shared>" "${export_map}")
set_tests_properties(public_exports PROPERTIES LABELS abi TIMEOUT 10)

# Deliberately small implementation sample. Full suites remain final gates.
add_custom_target(unit_build DEPENDS luthr luthrc luthr_static luthr_shared
  lua_facade_static lua_facade_shared native_roots_test execution_wait_test
  capture_cells_test execution_context_test memory_publication_test
  allocation_pool_test page_ownership_test gc_pacing_test
  gc_barriers_test gc_requests_test gc_reclamation_test gc_tracing_test
  scheduler_test native_lifecycle_test
  public_tasks_test public_tasks_shared sdk_consumer_smoke native_sdk_test)
if(TARGET race_detector_test)
  add_dependencies(unit_build race_detector_test)
endif()
set_property(TEST lua_facade_static lua_facade_shared public_exports
  native_roots_retain_incremental native_roots_nested_error
  native_roots_allocation_failure native_roots_publish execution_wait_empty
  execution_wait_managed execution_wait_mixed execution_wait_early
  APPEND PROPERTY LABELS unit)

add_test(NAME clean_scope COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/clean.sh"
  "${PROJECT_SOURCE_DIR}/Makefile" "${CMAKE_CURRENT_BINARY_DIR}/clean-fixture")
set_tests_properties(clean_scope PROPERTIES LABELS lifecycle TIMEOUT 10)

add_test(NAME make_defaults COMMAND "${CMAKE_COMMAND}"
  "-Dsource_dir=${PROJECT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/MakeDefaults.cmake")
set_tests_properties(make_defaults PROPERTIES LABELS "lifecycle;unit" TIMEOUT 10)
if(LUTHR_TARGET STREQUAL "x86_64-linux-gnu" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
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
