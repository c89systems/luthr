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
    callback callback_budget callback_shutdown callback_oom pin pin_cancel oom collect_oom automatic independent_native_limits load_error raw_vm)
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
foreach(case IN ITEMS results callback_budget pin_cancel independent_native_limits)
  add_test(NAME public_tasks_shared_${case} COMMAND public_tasks_shared ${case})
  set_tests_properties(public_tasks_shared_${case}
    PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 20)
endforeach()

luthr_test_program(scheduler_test scheduler_test.c)
set_target_properties(scheduler_test PROPERTIES C_STANDARD 99)
target_compile_options(scheduler_test PRIVATE -Wall -Wextra -Werror -mtls-dialect=gnu2)
target_link_libraries(scheduler_test PRIVATE luthr_static)
target_link_options(scheduler_test PRIVATE -Wl,--wrap=pthread_mutex_lock
  -Wl,--wrap=luaQ_wait -Wl,--wrap=luaQ_wakewaiter
  -Wl,--wrap=luaX_poll -Wl,--wrap=luaCT_pause -Wl,--wrap=pthread_mutex_init -Wl,--wrap=luaW_attach
  -Wl,--wrap=pthread_cond_init -Wl,--wrap=pthread_cond_wait -Wl,--wrap=pthread_create -Wl,--wrap=mmap
  -Wl,--wrap=mprotect -Wl,--wrap=munmap -Wl,--wrap=clock_gettime -Wl,--wrap=luaC_endtask
  -Wl,--wrap=luaC_preparefull)
foreach(case IN ITEMS fairness parallel retained_fairness no_requests early_incremental early_generational wake_waiters stopped_dispatch trace_pause_retains_collector trace_pause_retains_mutators
    native native_reentry shutdown yield saturation cancel_admission early_native_return native_grant_stopped_world native_fifo native_competitive
    native_accounting_count native_accounting_arm
    native_burst_parking native_burst_poll native_burst_single native_lease_expiry native_lease_inactive native_lease_full
    late_native_waiter cancel_granted_native startup_failures callback_progress
    timed_callback_progress synchronous_callback simultaneous_reentry parallel_native_compensation native_overlapping_stops native_return_compensation native_return_handoff native_leaf_return native_recall_during_stack_borrow native_recall_without_stack_offer native_recall_with_retirement
    parked cancel_parked idle_notifications standby_carriers idle_affinity metadata_progress
    timed_metadata_progress timed_slice timed_continuation_slice timed_retained_slice timed_retained_start
    nursery_incremental nursery_generational nursery_pacing
    nursery_edges_incremental nursery_edges_generational
    nursery_survivors_incremental nursery_survivors_generational
    nursery_root_pressure_incremental nursery_root_pressure_generational
    nursery_retained_roots_incremental nursery_retained_roots_generational
    nursery_parked_roots_incremental nursery_parked_roots_generational
    nursery_layout_roots_incremental nursery_layout_roots_generational
    nursery_mark_incremental nursery_mark_generational
    nursery_mark_publish_incremental nursery_mark_publish_generational
    nursery_native_incremental nursery_native_generational
    nursery_full_incremental nursery_full_generational
    nursery_full_cutoff_incremental nursery_full_cutoff_generational)
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
    stream_cancel stream_shutdown coroutine_allocations)
  add_test(NAME native_lifecycle_${case} COMMAND native_lifecycle_test ${case})
  set_tests_properties(native_lifecycle_${case}
    PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 15)
endforeach()

luthr_test_program(capture_cells_test capture_cells_test.c)
set_target_properties(capture_cells_test PROPERTIES C_STANDARD 99)
target_compile_options(capture_cells_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(capture_cells_test PRIVATE luthr_static)
target_link_options(capture_cells_test PRIVATE -Wl,--wrap=pthread_mutex_lock
  -Wl,--wrap=luaH_trygetsharedshortstr -Wl,--wrap=luaH_get
  -Wl,--wrap=luaA_begin -Wl,--wrap=luaA_end
  -Wl,--wrap=luaF_loadshared -Wl,--wrap=luaF_writecapture -Wl,--wrap=luaF_storelocals
  -Wl,--wrap=luaF_loadlocal -Wl,--wrap=luaF_loadrange -Wl,--wrap=luaX_pollrequests)
foreach(case IN ITEMS independent readers access uncaptured operands semantics pair_boundaries incremental generational
    private_incremental private_generational mixed_incremental mixed_generational
    deep_snapshot_incremental deep_snapshot_generational
    shadow_incremental shadow_generational lowered_source_incremental lowered_source_generational
    lowered_binary_incremental lowered_binary_generational lowered_fixed_incremental lowered_fixed_generational)
  add_test(NAME capture_cells_${case} COMMAND capture_cells_test ${case})
  set_tests_properties(capture_cells_${case}
    PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(execution_context_test execution_context_test.c)
set_target_properties(execution_context_test PROPERTIES C_STANDARD 99)
target_compile_options(execution_context_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(execution_context_test PRIVATE luthr_static)
target_link_options(execution_context_test PRIVATE -Wl,--wrap=luaE_getnativestackcontext)
foreach(case IN ITEMS host managed borrow pair foreign publication native_work native_bounds)
  add_test(NAME execution_context_${case} COMMAND execution_context_test ${case})
  set_tests_properties(execution_context_${case}
    PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(api_region_test api_region_test.c)
set_target_properties(api_region_test PROPERTIES C_STANDARD 99)
target_compile_options(api_region_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(api_region_test PRIVATE luthr_static Threads::Threads)
target_link_options(api_region_test PRIVATE -Wl,--wrap=luaW_enter)

luthr_test_program(stack_admission_test stack_admission_test.c)
set_target_properties(stack_admission_test PROPERTIES C_STANDARD 99)
target_compile_options(stack_admission_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(stack_admission_test PRIVATE luthr_static Threads::Threads)
target_link_options(stack_admission_test PRIVATE -Wl,--wrap=luaW_enter -Wl,--wrap=luaW_leave -Wl,--wrap=luaQ_wait -Wl,--wrap=luaX_waitvalues)
foreach(case IN ITEMS fifo_incremental fifo_generational fifo_stack_incremental fifo_stack_generational retire_incremental retire_generational
    reuse_incremental reuse_generational world_stop_incremental world_stop_generational
    rendezvous_incremental rendezvous_generational grow_incremental grow_generational
    dormant_incremental dormant_generational bounded_incremental bounded_generational)
  add_test(NAME stack_admission_${case} COMMAND stack_admission_test ${case})
  set_tests_properties(stack_admission_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 15)
endforeach()

luthr_test_program(gc_admission_test gc_admission_test.c)
set_target_properties(gc_admission_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_admission_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_admission_test PRIVATE luthr_static Threads::Threads)
target_link_options(gc_admission_test PRIVATE -Wl,--wrap=luaW_stop -Wl,--wrap=luaQ_wait)
foreach(collector IN ITEMS incremental generational)
  add_test(NAME gc_admission_${collector} COMMAND gc_admission_test ${collector})
  set_tests_properties(gc_admission_${collector}
    PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 15)
endforeach()

foreach(case IN ITEMS managed_incremental managed_generational foreign_incremental
    foreign_generational nested_incremental nested_generational yield_incremental yield_generational
    handoff_incremental handoff_generational collection_incremental collection_generational
    metadata_incremental metadata_generational native_return_incremental native_return_generational)
  add_test(NAME api_region_${case} COMMAND api_region_test ${case})
  set_tests_properties(api_region_${case}
    PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(value_api_test value_api_test.c)
set_target_properties(value_api_test PROPERTIES C_STANDARD 99)
target_compile_options(value_api_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(value_api_test PRIVATE luthr_static Threads::Threads)
target_link_options(value_api_test PRIVATE -Wl,--wrap=luaA_end)
foreach(collector IN ITEMS incremental generational)
  foreach(case IN ITEMS values denied relocate parallel close close_error number_string)
    add_test(NAME value_api_${case}_${collector}
      COMMAND value_api_test ${collector} ${case})
    set_tests_properties(value_api_${case}_${collector} PROPERTIES
      LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
  endforeach()
endforeach()

luthr_test_program(table_read_test table_read_test.c)
set_target_properties(table_read_test PROPERTIES C_STANDARD 99)
target_compile_options(table_read_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(table_read_test PRIVATE luthr_static)
target_link_options(table_read_test PRIVATE -Wl,--wrap=luaA_begin
  -Wl,--wrap=pthread_mutex_lock -Wl,--wrap=luaS_eqstr -Wl,--wrap=luaM_free_
  -Wl,--wrap=luaA_metafield -Wl,--wrap=luaH_readshortstr -Wl,--wrap=luaA_index
  -Wl,--wrap=luaH_trygetsharedshortstr -Wl,--wrap=luaH_get
  -Wl,--wrap=lua_setmetatable)
foreach(case IN ITEMS fast primitive batching stack_grace private_resize overlap retirement pressure failure hash_failure resume
    inline_overlap inline_array_failure inline_hash_failure
    primitive_reattach primitive_index primitive_incremental primitive_generational string_publication
    long_string_hash long_string_partition
    index_hit index_miss index_metatable_change object_bindings_incremental object_bindings_generational
    record_iteration_incremental record_iteration_generational
    ordinary_iteration_incremental ordinary_iteration_generational
    mixed_incremental mixed_generational writes_incremental writes_generational
    write_error_incremental write_error_generational retained_incremental retained_generational
    captured_incremental captured_generational)
  add_test(NAME table_read_${case} COMMAND table_read_test ${case})
  set_tests_properties(table_read_${case}
    PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 15)
endforeach()

luthr_test_program(gc_controller_test gc_controller_test.c)
set_target_properties(gc_controller_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_controller_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_controller_test PRIVATE luthr_static)
foreach(case IN ITEMS controller gate pair withdrawal)
  add_test(NAME gc_controller_${case} COMMAND gc_controller_test ${case})
  set_tests_properties(gc_controller_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(gc_pacing_test gc_pacing_test.c)
set_target_properties(gc_pacing_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_pacing_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_pacing_test PRIVATE luthr_static)
target_link_options(gc_pacing_test PRIVATE -Wl,--wrap=luaC_tracebatch
  -Wl,--wrap=luaC_sweepbatch -Wl,--wrap=luaQ_poll -Wl,--wrap=luaW_stop
  -Wl,--wrap=luaM_free_ -Wl,--wrap=luaM_releasepage)
foreach(case IN ITEMS overdue explicit overflow fast persistent_incremental
    persistent_generational refresh_incremental refresh_generational
    managed_function_incremental managed_function_generational
    sweep_pressure sweep_poll_pressure sweep_credit cohort_promotion marking_edges born_black_incremental born_black_generational
    borrowed_incremental borrowed_generational bounded_incremental
    bounded_generational idle_incremental idle_generational
    pressure_incremental pressure_generational unavailable_incremental
    unavailable_generational sweep_growth sweep_inline sweep_pooled stopped_incremental
    stopped_generational unwind_incremental unwind_generational
    table_growth_incremental table_growth_generational
    table_growth_stopped_incremental table_growth_stopped_generational
    closing_incremental closing_generational closing_pending_incremental
    closing_pending_generational nursery_incremental nursery_generational nursery_progress
    nursery_backing)
  add_test(NAME gc_pacing_${case} COMMAND gc_pacing_test ${case})
  set_tests_properties(gc_pacing_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(allocation_pool_test allocation_pool_test.c)
set_target_properties(allocation_pool_test PROPERTIES C_STANDARD 99)
target_compile_options(allocation_pool_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(allocation_pool_test PRIVATE luthr_static)
target_link_options(allocation_pool_test PRIVATE -Wl,--wrap=luaW_heapenter
  -Wl,--wrap=luaMP_initpage -Wl,--wrap=madvise)
foreach(case IN ITEMS serial parallel batch cap init_gc control_lifetime remote_returns
    spare_decommit spare_pressure spare_advice_failure)
  add_test(NAME allocation_pool_${case} COMMAND allocation_pool_test ${case})
  set_tests_properties(allocation_pool_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(allocator_admission_test allocator_admission_test.c)
set_target_properties(allocator_admission_test PROPERTIES C_STANDARD 99)
target_compile_options(allocator_admission_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(allocator_admission_test PRIVATE luthr_static Threads::Threads)
target_link_options(allocator_admission_test PRIVATE -Wl,--wrap=luaW_heapenter
  -Wl,--wrap=luaQ_wake -Wl,--wrap=realloc)
foreach(case IN ITEMS metadata_incremental metadata_generational callback_incremental callback_generational
    external_incremental external_generational
    notification_incremental notification_generational builtin_direct builtin_refill
    builtin_facade builtin_external)
  add_test(NAME allocator_admission_${case} COMMAND allocator_admission_test ${case})
  set_tests_properties(allocator_admission_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(allocator_interposition_test allocator_interposition_test.c)
set_target_properties(allocator_interposition_test PROPERTIES C_STANDARD 99)
target_compile_options(allocator_interposition_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(allocator_interposition_test PRIVATE luthr_shared)
target_link_options(allocator_interposition_test PRIVATE -Wl,--export-dynamic-symbol=luaL_alloc)
add_test(NAME allocator_interposition COMMAND allocator_interposition_test)
set_tests_properties(allocator_interposition PROPERTIES
  LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)

luthr_test_program(page_ownership_test page_ownership_test.c)
set_target_properties(page_ownership_test PROPERTIES C_STANDARD 99)
target_compile_options(page_ownership_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(page_ownership_test PRIVATE luthr_static)
target_link_options(page_ownership_test PRIVATE -Wl,--wrap=luaW_heapenter
  -Wl,--wrap=luaAL_enter
  -Wl,--wrap=luaQ_poll -Wl,--wrap=luaW_stop)
foreach(case IN ITEMS retained pressure coroutine_cache coroutine_cache_unwind busy_cache
    remote_returns growth_handoff growth_epoch assist_rebind boundary credit overlap helpers unwind finite)
  add_test(NAME page_ownership_${case} COMMAND page_ownership_test ${case})
  set_tests_properties(page_ownership_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 15)
endforeach()

luthr_test_program(gc_availability_test gc_availability_test.c)
set_target_properties(gc_availability_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_availability_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_availability_test PRIVATE luthr_static)
target_link_options(gc_availability_test PRIVATE -Wl,--wrap=luaC_tracebatch
  -Wl,--wrap=luaC_scantrace
  -Wl,--wrap=luaW_waitcollector
  -Wl,--wrap=luaW_trycollectenter)
foreach(case IN ITEMS defer_incremental defer_generational publish_incremental publish_generational
    queue_incremental queue_generational
    remote_incremental remote_generational
    busy_incremental busy_generational parked_incremental parked_generational
    pressure_incremental pressure_generational)
  add_test(NAME gc_availability_${case} COMMAND gc_availability_test ${case})
  set_tests_properties(gc_availability_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(gc_scheduler_test gc_scheduler_test.c)
set_target_properties(gc_scheduler_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_scheduler_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_scheduler_test PRIVATE luthr_static)
target_link_options(gc_scheduler_test PRIVATE -Wl,--wrap=luaC_scantrace
  -Wl,--wrap=luaC_scansweep -Wl,--wrap=luaW_armtrace -Wl,--wrap=luaQ_poll)
foreach(case IN ITEMS mark_incremental mark_generational sweep_incremental sweep_generational admission roots blocked_native
    mark_pause_incremental mark_pause_generational sweep_pause_incremental sweep_pause_generational
    single_incremental single_generational stopped_incremental
    minor_inline_generational minor_assist_generational)
  add_test(NAME gc_scheduler_${case} COMMAND gc_scheduler_test ${case})
  set_tests_properties(gc_scheduler_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
endforeach()

luthr_test_program(gc_directory_test gc_directory_test.c)
set_target_properties(gc_directory_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_directory_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_directory_test PRIVATE luthr_static)
target_link_options(gc_directory_test PRIVATE -Wl,--wrap=luaM_flushdebt -Wl,--wrap=luaD_shrinkstack
  -Wl,--wrap=luaA_beginlock)
foreach(case IN ITEMS forward remembered publish_incremental publish_generational
    string_cache_incremental string_cache_generational byte_strings_incremental
    byte_strings_generational string_startup trim_incremental trim_generational)
  add_test(NAME gc_directory_${case} COMMAND gc_directory_test ${case})
  set_tests_properties(gc_directory_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
endforeach()

luthr_test_program(gc_requests_test gc_requests_test.c)
set_target_properties(gc_requests_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_requests_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_requests_test PRIVATE luthr_static)
target_link_options(gc_requests_test PRIVATE -Wl,--wrap=luaW_heapenter
  -Wl,--wrap=luaW_heapleave
  -Wl,--wrap=luaW_collectenter -Wl,--wrap=luaW_trycollectenter
  -Wl,--wrap=luaW_stop -Wl,--wrap=luaW_waitcollector -Wl,--wrap=luaC_tracebatch
  -Wl,--wrap=luaD_tryscalarcall)
foreach(case IN ITEMS incremental generational explicit explicit_trace_slice stopped phase_incremental phase_generational
    overdue_incremental overdue_generational overdue_stopped shrink_strings reachable_finalizers late_finalizer
    handoff_trace_incremental handoff_trace_generational handoff_sweep_incremental handoff_sweep_generational
    full_incremental full_generational
    empty_minor minor_sweep_birth minor_unwind minor_finalizer minor_arm minor_arm_registry minor_arm_scalar
    arm_ack arm_withdraw arm_incremental major_after_minor late_task_root)
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
  foreach(case IN ITEMS overlap collect_again handshake assist assist_trace assist_trace_unavailable assist_unwind)
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
  -Wl,--wrap=luaM_malloc_ -Wl,--wrap=luaM_allocstack -Wl,--wrap=luaC_scanstrong
  -Wl,--wrap=luaW_waitcollector)
foreach(case IN ITEMS array hash userdata metatable cclosure capture stack stack_wait_snapshot stack_wait_release stack_wait_offer stack_wait_reopen stack_wait_collect stack_wait_park busy parser closure_builder thread_builder thread_builder_error weakvalues weakkeys weakarray resurrected_weak weak_retry weak_stamp_wrap intern cache
    concurrent_incremental concurrent_generational concurrent_minor emergency_incremental emergency_minor help_incremental help_minor help_to_generational help_to_incremental ephemerons parallel_mark parallel_ephemerons parallel_weakvalues
    capture_read weaken_array_after_read weaken_hash_after_read page_frontier)
  add_test(NAME gc_tracing_${case} COMMAND gc_tracing_test ${case})
  set_tests_properties(gc_tracing_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 15)
endforeach()

luthr_test_program(gc_table_scan_test gc_table_scan_test.c)
set_target_properties(gc_table_scan_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_table_scan_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_table_scan_test PRIVATE luthr_static)
target_link_options(gc_table_scan_test PRIVATE -Wl,--wrap=luaA_tryscan
  -Wl,--wrap=luaC_scanstrong -Wl,--wrap=luaH_tracechunk -Wl,--wrap=luaC_traceedges
  -Wl,--wrap=luaC_finishstrong -Wl,--wrap=luaM_free_ -Wl,--wrap=luaHS_scanend
  -Wl,--undefined=luaC_scanstrong)
foreach(collector IN ITEMS incremental generational)
  foreach(case IN ITEMS array list metatable relocate pause first_write first_write_small scalar_cards record)
    add_test(NAME gc_table_scan_${case}_${collector}
      COMMAND gc_table_scan_test ${case}_${collector})
    set_tests_properties(gc_table_scan_${case}_${collector} PROPERTIES
      LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
  endforeach()
  foreach(case IN ITEMS array list metatable fields retirement)
    add_test(NAME gc_table_scan_private_${case}_${collector}
      COMMAND gc_table_scan_test ${case}_${collector} private)
    set_tests_properties(gc_table_scan_private_${case}_${collector} PROPERTIES
      LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
  endforeach()
endforeach()

foreach(case IN ITEMS remember cards promotion)
  add_test(NAME gc_table_scan_${case}_generational
    COMMAND gc_table_scan_test ${case}_generational)
  set_tests_properties(gc_table_scan_${case}_generational PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
endforeach()

add_test(NAME gc_table_scan_private_promotion_generational
  COMMAND gc_table_scan_test promotion_generational private)
set_tests_properties(gc_table_scan_private_promotion_generational PROPERTIES
  LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)

luthr_test_program(gc_object_scan_test gc_object_scan_test.c)
set_target_properties(gc_object_scan_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_object_scan_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_object_scan_test PRIVATE luthr_static)
target_link_options(gc_object_scan_test PRIVATE -Wl,--wrap=luaC_objectchunk
  -Wl,--wrap=luaA_tryscan -Wl,--wrap=pthread_mutex_trylock
  -Wl,--undefined=luaC_objectchunk)
foreach(collector IN ITEMS incremental generational)
  foreach(kind IN ITEMS userdata userdata0 cclosure lclosure capture)
    foreach(sharing IN ITEMS private shared)
      add_test(NAME gc_object_scan_${kind}_${sharing}_${collector}
        COMMAND gc_object_scan_test ${kind}_${sharing}_${collector})
      set_tests_properties(gc_object_scan_${kind}_${sharing}_${collector}
        PROPERTIES LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
    endforeach()
  endforeach()
endforeach()

luthr_test_program(object_sharing_test object_sharing_test.c)
set_target_properties(object_sharing_test PROPERTIES C_STANDARD 99)
target_compile_options(object_sharing_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(object_sharing_test PRIVATE luthr_static)
foreach(collector IN ITEMS incremental generational)
  foreach(route IN ITEMS xmove global cclosure uservalue metatable callback finalizer)
    add_test(NAME object_sharing_${route}_${collector}
      COMMAND object_sharing_test ${route}_${collector})
    set_tests_properties(object_sharing_${route}_${collector} PROPERTIES
      LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
  endforeach()
endforeach()

luthr_test_program(table_writers_test table_writers_test.c)
set_target_properties(table_writers_test PROPERTIES C_STANDARD 99)
target_compile_options(table_writers_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(table_writers_test PRIVATE luthr_static)
target_link_options(table_writers_test PRIVATE -Wl,--wrap=luaA_begin
  -Wl,--wrap=luaH_elementstore -Wl,--undefined=luaH_elementstore
  -Wl,--wrap=luaH_recordstore -Wl,--wrap=luaQ_wait)
foreach(collector IN ITEMS incremental generational)
  foreach(kind IN ITEMS array hash array_graph hash_graph record record_graph record_packed collision length)
    add_test(NAME table_writers_${kind}_${collector}
      COMMAND table_writers_test ${collector} ${kind})
    set_tests_properties(table_writers_${kind}_${collector} PROPERTIES
      LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
  endforeach()
endforeach()

luthr_test_program(table_contention_test table_contention_test.c)
set_target_properties(table_contention_test PROPERTIES C_STANDARD 99)
target_compile_options(table_contention_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(table_contention_test PRIVATE luthr_static)
target_link_options(table_contention_test PRIVATE -Wl,--wrap=luaH_elementstore
  -Wl,--wrap=luaQ_wait -Wl,--wrap=luaH_waitwriters)
foreach(collector IN ITEMS incremental generational)
  foreach(kind IN ITEMS array hash array_graph hash_graph)
    foreach(route IN ITEMS raw vm read vm_read vm_missing)
      add_test(NAME table_contention_${kind}_${route}_${collector}
        COMMAND table_contention_test ${collector} ${kind} ${route})
      set_tests_properties(table_contention_${kind}_${route}_${collector} PROPERTIES
        LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
    endforeach()
  endforeach()
endforeach()

luthr_test_program(table_layout_wait_test table_layout_wait_test.c)
set_target_properties(table_layout_wait_test PROPERTIES C_STANDARD 99)
target_compile_options(table_layout_wait_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(table_layout_wait_test PRIVATE luthr_static)
target_link_options(table_layout_wait_test PRIVATE -Wl,--wrap=luaA_begin
  -Wl,--wrap=luaH_elementstore -Wl,--wrap=luaQ_wait -Wl,--wrap=pthread_mutex_lock)
foreach(collector IN ITEMS incremental generational)
  add_test(NAME table_layout_wait_${collector}
    COMMAND table_layout_wait_test ${collector})
  set_tests_properties(table_layout_wait_${collector} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
endforeach()

luthr_test_program(table_partitions_test table_partitions_test.c)
set_target_properties(table_partitions_test PROPERTIES C_STANDARD 99)
target_compile_options(table_partitions_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(table_partitions_test PRIVATE luthr_static)
target_link_options(table_partitions_test PRIVATE -Wl,--wrap=luaH_releasepart
  -Wl,--wrap=luaQ_wait -Wl,--wrap=luaQ_wake -Wl,--wrap=luaM_realloc_)
foreach(collector IN ITEMS incremental generational)
  add_test(NAME table_partitions_${collector}
    COMMAND table_partitions_test ${collector})
  set_tests_properties(table_partitions_${collector} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
endforeach()

luthr_test_program(closure_cache_test closure_cache_test.c)
set_target_properties(closure_cache_test PROPERTIES C_STANDARD 99)
target_compile_options(closure_cache_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(closure_cache_test PRIVATE luthr_static)
target_link_options(closure_cache_test PRIVATE -Wl,--wrap=luaA_end)
foreach(collector IN ITEMS incremental generational)
  foreach(case IN ITEMS reuse weak construction cycles captures parallel)
    add_test(NAME closure_cache_${case}_${collector}
      COMMAND closure_cache_test ${case} ${collector})
    set_tests_properties(closure_cache_${case}_${collector} PROPERTIES
      LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
  endforeach()
endforeach()

luthr_test_program(object_layout_test object_layout_test.c)
set_target_properties(object_layout_test PROPERTIES C_STANDARD 99)
target_compile_options(object_layout_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(object_layout_test PRIVATE luthr_static)
foreach(collector IN ITEMS incremental generational)
  foreach(kind IN ITEMS closure capture cell managed userdata userdata_values table_lua table_api table_record)
    add_test(NAME object_layout_${kind}_${collector}
      COMMAND object_layout_test ${kind} ${collector})
    set_tests_properties(object_layout_${kind}_${collector} PROPERTIES
      LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
  endforeach()
endforeach()

luthr_test_program(sharing_roots_test sharing_roots_test.c)
set_target_properties(sharing_roots_test PROPERTIES C_STANDARD 99)
target_compile_options(sharing_roots_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(sharing_roots_test PRIVATE luthr_static)
target_link_options(sharing_roots_test PRIVATE -Wl,--wrap=luaA_end)
foreach(collector IN ITEMS incremental generational minor)
  add_test(NAME sharing_roots_${collector} COMMAND sharing_roots_test ${collector})
  set_tests_properties(sharing_roots_${collector} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
endforeach()

luthr_test_program(pipe_publication_test pipe_publication_test.c)
set_target_properties(pipe_publication_test PROPERTIES C_STANDARD 99)
target_compile_options(pipe_publication_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(pipe_publication_test PRIVATE luthr_static)
target_link_options(pipe_publication_test PRIVATE -Wl,--wrap=luaS_newudata)
foreach(collector IN ITEMS incremental generational)
  add_test(NAME pipe_publication_${collector} COMMAND pipe_publication_test ${collector})
  set_tests_properties(pipe_publication_${collector} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
endforeach()

luthr_test_program(gc_sweeping_test gc_sweeping_test.c)
set_target_properties(gc_sweeping_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_sweeping_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_sweeping_test PRIVATE luthr_static)
target_link_options(gc_sweeping_test PRIVATE -Wl,--wrap=luaC_scansweep
  -Wl,--wrap=luaC_sweepbatch -Wl,--wrap=luaC_reclaim -Wl,--wrap=luaQ_poll
  -Wl,--wrap=luaW_restore)
foreach(case IN ITEMS incremental minor conversion white_minor resurrect_minor boundary empty_bands promoted_builder threads unwind
    emergency_incremental emergency_minor emergency_conversion)
  add_test(NAME gc_sweeping_${case} COMMAND gc_sweeping_test ${case})
  set_tests_properties(gc_sweeping_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 30)
endforeach()

luthr_test_program(gc_epochs_test gc_epochs_test.c)
set_target_properties(gc_epochs_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_epochs_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_epochs_test PRIVATE luthr_static)
target_link_options(gc_epochs_test PRIVATE -Wl,--wrap=luaC_scansweep)
foreach(case IN ITEMS live live_generational modes rotations weak_age mature_page)
  add_test(NAME gc_epochs_${case} COMMAND gc_epochs_test ${case})
  set_tests_properties(gc_epochs_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 15)
endforeach()

luthr_test_program(gc_membership_test gc_membership_test.c)
set_target_properties(gc_membership_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_membership_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_membership_test PRIVATE luthr_static)
target_link_options(gc_membership_test PRIVATE -Wl,--wrap=luaW_stop)
foreach(case IN ITEMS incremental generational old_generational
    sweeping_incremental pending_incremental pending_generational)
  add_test(NAME gc_membership_${case} COMMAND gc_membership_test ${case})
  set_tests_properties(gc_membership_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 15)
endforeach()

luthr_test_program(finalizer_work_test finalizer_work_test.c)
set_target_properties(finalizer_work_test PROPERTIES C_STANDARD 99)
target_compile_options(finalizer_work_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(finalizer_work_test PRIVATE luthr_static)
target_link_options(finalizer_work_test PRIVATE -Wl,--wrap=luaC_claimtrace
  -Wl,--wrap=luaC_tracebatch)
foreach(case IN ITEMS discovery_incremental discovery_generational
    pending_incremental pending_generational promote_incremental promote_generational
    overlap_incremental overlap_pending_incremental order_incremental order_generational)
  add_test(NAME finalizer_work_${case} COMMAND finalizer_work_test ${case})
  set_tests_properties(finalizer_work_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 20)
endforeach()

luthr_test_program(gc_barriers_test gc_barriers_test.c)
set_target_properties(gc_barriers_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_barriers_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_barriers_test PRIVATE luthr_static)
target_link_options(gc_barriers_test PRIVATE -Wl,--wrap=luaW_stop)
foreach(case IN ITEMS trace_handoff sweep_handoff)
  add_test(NAME gc_barriers_${case} COMMAND gc_barriers_test ${case} generational)
  set_tests_properties(gc_barriers_${case} PROPERTIES
    LABELS "unit;runtime;parallel;memcheck" TIMEOUT 10)
endforeach()
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
  -Wl,--wrap=luaM_realloc_)
target_link_options(memory_publication_test PRIVATE -Wl,--wrap=luaAL_enter)
foreach(case IN ITEMS no_requests stack_admission allocation_admission stack_contention intern_hits ordinary_operations allocation_cache local_returns stack_retry marking_cache coalesced_gc handoff paired_handoff late waiting
    totals construction weak_resize metatable_resize incremental generational page_growth page_cap page_isolation
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
target_link_options(task_stop_test PRIVATE -Wl,--wrap=lua_resume
  -Wl,--wrap=luaD_tryleaf -Wl,--wrap=luaH_trynext)
foreach(backend IN ITEMS raw facade)
  foreach(gc IN ITEMS incremental generational)
    foreach(case IN ITEMS prestart cpu leaf leaf_return iterator_leaf_return catch xpcall native native_oom continuation callback unprotected foreign_vm oom parser parser_oom yield cleanup_error cleanup_wait request_race late)
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
    foreach(case IN ITEMS semantics errors bindings leaf snapshot oom slice)
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
        snapshot_match snapshot_gsub snapshot_gsub_class snapshot_iterator
        snapshot_values callback_snapshot parallel stress)
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
  -Wl,--wrap=pthread_mutex_init -Wl,--wrap=pthread_cond_init
  -Wl,--wrap=luaS_newlstr)
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
foreach(case IN ITEMS replace blocked builtin)
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
foreach(case IN ITEMS allocator_mutex allocator_condition world_mutex world_condition
    world_resume_condition world_rendezvous_condition world_phase_condition world_trace_condition
    publisher_mutex pool_mutex pool_last_mutex
    thread_no_resources closed_capture open_capture facade_policy_mutex facade_backing_mutex facade_allocator_mutex
    facade_world_mutex facade_pool_mutex)
  add_test(NAME synchronization_failure_${case} COMMAND synchronization_failure_test ${case})
  set_tests_properties(synchronization_failure_${case} PROPERTIES LABELS "runtime;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(world_unwind_test world_unwind_test.c)
set_target_properties(world_unwind_test PROPERTIES C_STANDARD 99)
target_compile_options(world_unwind_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(world_unwind_test PRIVATE luthr_static)
target_link_options(world_unwind_test PRIVATE -Wl,--wrap=luaX_apienter)
foreach(case IN ITEMS warning_error warning_oom forwarded_gc)
  add_test(NAME world_unwind_${case} COMMAND world_unwind_test ${case})
  set_tests_properties(world_unwind_${case} PROPERTIES LABELS "runtime;memcheck" TIMEOUT 10)
endforeach()

luthr_test_program(api_ownership_test api_ownership_test.c)
set_target_properties(api_ownership_test PROPERTIES C_STANDARD 99)
target_compile_options(api_ownership_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(api_ownership_test PRIVATE luthr_static)
target_link_options(api_ownership_test PRIVATE
  -Wl,--wrap=luaX_apienter -Wl,--wrap=luaX_apileave -Wl,--wrap=luaA_end
  -Wl,--wrap=luaW_leave
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
  capture_cells_test closure_cache_test execution_context_test api_region_test stack_admission_test gc_admission_test value_api_test table_read_test memory_publication_test
  allocation_pool_test allocator_admission_test allocator_interposition_test page_ownership_test gc_pacing_test gc_controller_test
  gc_barriers_test gc_availability_test gc_scheduler_test gc_directory_test gc_requests_test gc_reclamation_test gc_tracing_test finalizer_work_test
  gc_membership_test gc_table_scan_test gc_object_scan_test table_writers_test table_contention_test table_layout_wait_test table_partitions_test object_sharing_test object_layout_test sharing_roots_test pipe_publication_test
  gc_sweeping_test
  gc_epochs_test
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
