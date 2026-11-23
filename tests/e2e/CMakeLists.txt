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
luthr_test_program(upstream_lua "${reference_dir}/lua.c")
luthr_test_program(upstream_luac "${reference_dir}/luac.c")
foreach(program IN ITEMS upstream_lua upstream_luac)
  set_target_properties(${program} PROPERTIES C_STANDARD 99)
  target_compile_definitions(${program} PRIVATE LUA_USE_LINUX)
  target_link_libraries(${program} PRIVATE upstream_lua_reference)
endforeach()

target_link_options(upstream_lua PRIVATE -Wl,--export-dynamic)

foreach(runtime IN ITEMS upstream luthr)
  luthr_test_program(benchmark_${runtime} benchmark_host.c)
  set_target_properties(benchmark_${runtime} PROPERTIES C_STANDARD 99)
  target_compile_options(benchmark_${runtime} PRIVATE -Wall -Wextra -Werror)
  target_include_directories(benchmark_${runtime} BEFORE PRIVATE "${reference_dir}")
  target_link_options(benchmark_${runtime} PRIVATE -Wl,--export-dynamic)
  if(runtime STREQUAL "upstream")
    target_link_libraries(benchmark_${runtime} PRIVATE upstream_lua_reference)
  else()
    target_link_libraries(benchmark_${runtime} PRIVATE luthr_shared)
  endif()
endforeach()

foreach(runtime IN ITEMS upstream luthr)
  luthr_test_program(parallel_${runtime} parallel.c)
  target_sources(parallel_${runtime} PRIVATE "${PROJECT_SOURCE_DIR}/src/cpu_capacity.c")
  set_target_properties(parallel_${runtime} PROPERTIES C_STANDARD 99)
  target_compile_options(parallel_${runtime} PRIVATE -Wall -Wextra -Werror)
  target_include_directories(parallel_${runtime} BEFORE PRIVATE "${reference_dir}")
  target_link_libraries(parallel_${runtime} PRIVATE Threads::Threads)
  if(runtime STREQUAL "upstream")
    target_link_libraries(parallel_${runtime} PRIVATE upstream_lua_reference)
    set(workers 1)
  else()
    target_compile_definitions(parallel_${runtime} PRIVATE LUTHR_BENCH_SHARED_VM)
    target_link_libraries(parallel_${runtime} PRIVATE luthr_shared)
    set(workers 2)
  endif()
  foreach(workload IN ITEMS integers independent_tables shared_table shared_hash shared_growth shared_reads closures allocation_gc)
    foreach(collector IN ITEMS incremental generational)
      add_test(NAME lua_parallel_${runtime}_${workload}_${collector}
        COMMAND parallel_${runtime} "${CMAKE_CURRENT_SOURCE_DIR}/parallel.lua"
          "${workload}" "${collector}" "${workers}" 8 256 2 1)
      set_tests_properties(lua_parallel_${runtime}_${workload}_${collector}
        PROPERTIES LABELS "runtime;benchmark;memcheck" TIMEOUT 30)
      if(runtime STREQUAL "luthr")
        set_property(TEST lua_parallel_${runtime}_${workload}_${collector}
          APPEND PROPERTY LABELS parallel)
      endif()
    endforeach()
  endforeach()
endforeach()

set(callback_workloads lua_callback native_callbacks native_callbacks_local native_direct
  native_direct_local native_leaf native_stack native_identity
  native_scalar native_scalar_direct native_protected native_records native_streaming)
set(scheduled_workloads integers independent_tables shared_table shared_hash
  shared_growth shared_reads closures allocation_gc ${callback_workloads})
foreach(runtime IN ITEMS upstream luthr embedded)
  luthr_test_program(scheduled_${runtime} scheduled.c)
  target_sources(scheduled_${runtime} PRIVATE native.c "${PROJECT_SOURCE_DIR}/src/cpu_capacity.c")
  set_target_properties(scheduled_${runtime} PROPERTIES C_STANDARD 99)
  target_compile_options(scheduled_${runtime} PRIVATE -Wall -Wextra -Werror)
  target_include_directories(scheduled_${runtime} BEFORE PRIVATE "${reference_dir}")
  if(runtime STREQUAL "luthr")
    target_compile_definitions(scheduled_${runtime} PRIVATE LUTHR_BENCH_SCHEDULER)
    target_link_libraries(scheduled_${runtime} PRIVATE luthr_shared)
    set(scheduled_slots 2)
    set(scheduled_native 1)
  else()
    set(scheduled_slots 1)
    set(scheduled_native 0)
    if(runtime STREQUAL "upstream")
      target_link_libraries(scheduled_${runtime} PRIVATE upstream_lua_reference)
    else()
      target_link_libraries(scheduled_${runtime} PRIVATE luthr_shared)
    endif()
  endif()
  set(workloads "${scheduled_workloads}")
  if(runtime STREQUAL "embedded")
    set(workloads ${callback_workloads})
  endif()
  foreach(workload IN LISTS workloads)
    foreach(collector IN ITEMS incremental generational)
      add_test(NAME lua_scheduled_${runtime}_${workload}_${collector}
        COMMAND scheduled_${runtime} "${CMAKE_CURRENT_SOURCE_DIR}/parallel.lua"
          "${workload}" "${collector}" "${scheduled_slots}" "${scheduled_native}"
          8 256 2 1 1000)
      set_tests_properties(lua_scheduled_${runtime}_${workload}_${collector}
        PROPERTIES LABELS "runtime;benchmark;memcheck" TIMEOUT 30)
      if(runtime STREQUAL "luthr")
        set_property(TEST lua_scheduled_${runtime}_${workload}_${collector}
          APPEND PROPERTY LABELS parallel)
      endif()
    endforeach()
  endforeach()
endforeach()

foreach(collector IN ITEMS incremental generational)
  add_test(NAME lua_scheduled_retained_chain_${collector}
    COMMAND scheduled_luthr "${CMAKE_CURRENT_SOURCE_DIR}/parallel.lua"
      retained_chain "${collector}" 1 1 1 65000 1 0 0)
  set_tests_properties(lua_scheduled_retained_chain_${collector}
    PROPERTIES LABELS "runtime;parallel;memcheck" TIMEOUT 30)
  add_test(NAME lua_scheduled_nursery_cap_${collector}
    COMMAND scheduled_luthr "${CMAKE_CURRENT_SOURCE_DIR}/parallel.lua"
      independent_tables "${collector}" 8 0 32 18000 2 1 0)
  set_tests_properties(lua_scheduled_nursery_cap_${collector}
    PROPERTIES LABELS "runtime;parallel;memcheck" TIMEOUT 90)
  add_test(NAME lua_scheduled_array_records_${collector}
    COMMAND scheduled_luthr "${CMAKE_CURRENT_SOURCE_DIR}/parallel.lua"
      array_records "${collector}" 4 0 8 2000 2 1 0)
  set_tests_properties(lua_scheduled_array_records_${collector}
    PROPERTIES LABELS "runtime;parallel;memcheck" TIMEOUT 30)
endforeach()

set(scheduled_arguments
  "-Dupstream=$<TARGET_FILE:scheduled_upstream>" "-Dluthr=$<TARGET_FILE:scheduled_luthr>"
  "-Dembedded=$<TARGET_FILE:scheduled_embedded>"
  "-Dlibrary=$<TARGET_FILE:luthr_shared>" "-Dsource_dir=${PROJECT_SOURCE_DIR}"
  "-Dscript=${CMAKE_CURRENT_SOURCE_DIR}/parallel.lua"
  "-Dtarget=${LUTHR_TARGET}" "-Dconfig=${CMAKE_BUILD_TYPE}"
  "-Dsanitizer=${LUTHR_THREAD_SANITIZER}"
  "-Dcompiler=${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")

# Diagnostic executables share the exact production objects. Linker wrapping
# adds invocation-local counters only to the probe; the static control exposes
# measurement overhead without changing collector algorithms or public APIs.
foreach(mode IN ITEMS control profile)
  luthr_test_program(scheduled_gc_${mode} scheduled.c)
  target_sources(scheduled_gc_${mode} PRIVATE native.c "${PROJECT_SOURCE_DIR}/src/cpu_capacity.c")
  set_target_properties(scheduled_gc_${mode} PROPERTIES C_STANDARD 99)
  target_compile_options(scheduled_gc_${mode} PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(scheduled_gc_${mode} PRIVATE LUTHR_BENCH_SCHEDULER)
  target_link_libraries(scheduled_gc_${mode} PRIVATE luthr_static)
endforeach()
target_sources(scheduled_gc_profile PRIVATE gc_probe.c)
target_compile_definitions(scheduled_gc_profile PRIVATE LUTHR_BENCH_GC_PROBE)
luthr_test_program(gc_handoff_probe_test gc_handoff_probe.c)
target_sources(gc_handoff_probe_test PRIVATE gc_probe.c)
set_target_properties(gc_handoff_probe_test PROPERTIES C_STANDARD 99)
target_compile_options(gc_handoff_probe_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gc_handoff_probe_test PRIVATE luthr_static)
foreach(symbol IN ITEMS luaW_stop luaW_start luaW_heapenter luaW_collectenter luaW_trycollectenter luaW_heapleave
    luaAL_realloc
    luaC_step luaC_userstep luaC_fullgc luaC_syncbarrier luaC_publish luaC_drainbarriers luaC_reclaim
    luaM_reclaimpages luaQ_wait luaC_assistreclaim luaW_waitcollector luaCT_pause
    luaC_tracebatch luaC_scantrace luaC_assisttrace luaC_polltrace luaC_snapshot luaC_checksnapshot luaC_finishtrace
    luaC_sweepbatch luaC_sweeppayload luaSC_collectidle luaC_scansweep luaC_pollsweep luaC_assistsweep luaQ_wake)
  target_link_options(scheduled_gc_profile PRIVATE "-Wl,--wrap=${symbol}")
  target_link_options(gc_handoff_probe_test PRIVATE "-Wl,--wrap=${symbol}")
endforeach()
add_test(NAME gc_handoff_probe COMMAND gc_handoff_probe_test)
set_tests_properties(gc_handoff_probe PROPERTIES
  LABELS "runtime;benchmark;parallel;memcheck;gc_profile" TIMEOUT 15)
set(gc_arguments
  "-Dupstream=$<TARGET_FILE:scheduled_upstream>" "-Dshared=$<TARGET_FILE:scheduled_luthr>"
  "-Dcontrol=$<TARGET_FILE:scheduled_gc_control>" "-Dprofile=$<TARGET_FILE:scheduled_gc_profile>"
  "-Dlibrary=$<TARGET_FILE:luthr_static>" "-Dsource_dir=${PROJECT_SOURCE_DIR}"
  "-Dscript=${CMAKE_CURRENT_SOURCE_DIR}/parallel.lua"
  "-Dtarget=${LUTHR_TARGET}" "-Dconfig=${CMAKE_BUILD_TYPE}"
  "-Dcompiler=${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
add_custom_target(benchmark_gc
  COMMAND "${CMAKE_COMMAND}" -E env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0
    "${CMAKE_COMMAND}" ${gc_arguments}
    "-Doutput_dir=${PROJECT_BINARY_DIR}/benchmarks/gc"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/BenchmarkGc.cmake"
  DEPENDS scheduled_upstream scheduled_luthr scheduled_gc_control scheduled_gc_profile
  VERBATIM USES_TERMINAL)
add_test(NAME lua_gc_probe_smoke COMMAND "${CMAKE_COMMAND}" ${gc_arguments}
  -Dsamples=2 -Dwarmups=1 -Dtasks=8 -Dwork_divisor=16 "-Dlevels=1;2"
  "-Doutput_dir=${CMAKE_CURRENT_BINARY_DIR}/gc-probe-smoke"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/GcProbeSmoke.cmake")
set_tests_properties(lua_gc_probe_smoke PROPERTIES
  LABELS "runtime;benchmark;parallel;gc_profile" TIMEOUT 120)
foreach(collector IN ITEMS incremental generational)
  foreach(mode IN ITEMS running stopped)
    set(extra)
    if(mode STREQUAL "stopped")
      set(extra stopped)
    endif()
    add_test(NAME gc_probe_${collector}_${mode} COMMAND scheduled_gc_profile
      "${CMAKE_CURRENT_SOURCE_DIR}/parallel.lua" independent_tables
      "${collector}" 2 1 8 4096 1 0 0 ${extra})
    set_tests_properties(gc_probe_${collector}_${mode} PROPERTIES
      LABELS "runtime;benchmark;parallel;memcheck;gc_profile" TIMEOUT 30)
  endforeach()
endforeach()
add_test(NAME gc_probe_fanout COMMAND scheduled_gc_profile
  "${CMAKE_CURRENT_SOURCE_DIR}/parallel.lua" independent_tables
  incremental 8 1 32 5000 1 0 0)
set_tests_properties(gc_probe_fanout PROPERTIES
  LABELS "runtime;benchmark;parallel;memcheck;gc_profile" TIMEOUT 30)
add_test(NAME gc_allocation_bound COMMAND "${CMAKE_COMMAND}"
  "-Dprogram=$<TARGET_FILE:scheduled_gc_control>"
  "-Dscript=${CMAKE_CURRENT_SOURCE_DIR}/parallel.lua"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/GcAllocationBound.cmake")
set_tests_properties(gc_allocation_bound PROPERTIES
  LABELS "runtime;parallel;benchmark" TIMEOUT 120)
add_test(NAME gc_allocation_bound_extended COMMAND "${CMAKE_COMMAND}"
  "-Dprogram=$<TARGET_FILE:scheduled_gc_control>"
  "-Dscript=${CMAKE_CURRENT_SOURCE_DIR}/parallel.lua" -Diterations=80000
  -P "${CMAKE_CURRENT_SOURCE_DIR}/GcAllocationBound.cmake")
set_tests_properties(gc_allocation_bound_extended PROPERTIES
  LABELS "runtime;parallel;benchmark" TIMEOUT 120)
add_custom_target(benchmark_scheduler
  COMMAND "${CMAKE_COMMAND}" -E env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0
    "${CMAKE_COMMAND}" ${scheduled_arguments}
    "-Doutput_dir=${PROJECT_BINARY_DIR}/benchmarks/scheduler"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/BenchmarkScheduled.cmake"
  DEPENDS scheduled_upstream scheduled_luthr scheduled_embedded VERBATIM USES_TERMINAL)
add_test(NAME lua_scheduled_smoke COMMAND "${CMAKE_COMMAND}" ${scheduled_arguments}
  -Dsamples=2 -Dwarmups=1 -Dtasks=8 -Dwork_divisor=2048 -Ddelay_us=1000 "-Dlevels=1;2;0"
  "-Doutput_dir=${CMAKE_CURRENT_BINARY_DIR}/scheduled-smoke"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/BenchmarkScheduledSmoke.cmake")
add_test(NAME lua_scheduled_arguments COMMAND "${CMAKE_COMMAND}" ${scheduled_arguments}
  "-Dwork_dir=${CMAKE_CURRENT_BINARY_DIR}/scheduled-errors"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/BenchmarkScheduledArguments.cmake")
set_tests_properties(lua_scheduled_smoke lua_scheduled_arguments
  PROPERTIES LABELS "runtime;benchmark" TIMEOUT 60)

set(parallel_arguments
  "-Dupstream=$<TARGET_FILE:parallel_upstream>" "-Dluthr=$<TARGET_FILE:parallel_luthr>"
  "-Dlibrary=$<TARGET_FILE:luthr_shared>" "-Dsource_dir=${PROJECT_SOURCE_DIR}"
  "-Dscript=${CMAKE_CURRENT_SOURCE_DIR}/parallel.lua"
  "-Dtarget=${LUTHR_TARGET}" "-Dconfig=${CMAKE_BUILD_TYPE}"
  "-Dcompiler=${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
add_custom_target(benchmark_parallel
  COMMAND "${CMAKE_COMMAND}" -E env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0
    "${CMAKE_COMMAND}" ${parallel_arguments}
    "-Doutput_dir=${PROJECT_BINARY_DIR}/benchmarks/parallel"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/BenchmarkParallel.cmake"
  DEPENDS parallel_upstream parallel_luthr VERBATIM USES_TERMINAL)
add_test(NAME lua_parallel_benchmark_smoke COMMAND "${CMAKE_COMMAND}" ${parallel_arguments}
  -Dsamples=2 -Dwarmups=1 -Dtasks=8 -Dwork_divisor=2048 "-Dlevels=1;2"
  "-Doutput_dir=${CMAKE_CURRENT_BINARY_DIR}/parallel-smoke"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/BenchmarkParallelSmoke.cmake")
add_test(NAME lua_parallel_arguments COMMAND "${CMAKE_COMMAND}" ${parallel_arguments}
  "-Dwork_dir=${CMAKE_CURRENT_BINARY_DIR}/parallel-errors"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/BenchmarkArguments.cmake")
set_tests_properties(lua_parallel_benchmark_smoke lua_parallel_arguments
  PROPERTIES LABELS "runtime;benchmark" TIMEOUT 60)

add_library(compat_fixture MODULE native.c)
set_target_properties(compat_fixture PROPERTIES PREFIX "" C_STANDARD 99)
target_include_directories(compat_fixture PRIVATE "${reference_dir}")
target_compile_options(compat_fixture PRIVATE -Wall -Wextra -Werror)

set(suite_archive "${PROJECT_SOURCE_DIR}/.cache/vendor/lua-5.5.1-tests.tar.gz")
if(NOT EXISTS "${suite_archive}")
  message(FATAL_ERROR "Official Lua 5.5.1 test archive missing. Run make prepare (or bash tools/prepare_lua_reference.sh).")
endif()
file(SHA256 "${suite_archive}" suite_digest)
if(NOT suite_digest STREQUAL "da07b543872dc0bb2ff12aabd0c248578d78df3eb6b67efdc537a46d455c7f31")
  message(FATAL_ERROR "Official Lua 5.5.1 test archive checksum mismatch")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${suite_archive}")
set(suite_source "${CMAKE_CURRENT_BINARY_DIR}/official-source/lua-5.5.1-tests")
if(NOT EXISTS "${suite_source}/all.lua")
  file(ARCHIVE_EXTRACT INPUT "${suite_archive}"
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/official-source")
  file(GLOB suite_native_inputs "${suite_source}/libs/*.c")
  file(TOUCH ${suite_native_inputs})
endif()
set(suite_modules "${CMAKE_CURRENT_BINARY_DIR}/official-libs")
foreach(module IN ITEMS lib1 lib11 lib2 lib21 lib2-v2)
  set(module_source "${module}")
  if(module STREQUAL "lib2-v2")
    set(module_source lib22)
  endif()
  add_library(lua_suite_${module} MODULE "${suite_source}/libs/${module_source}.c")
  set_target_properties(lua_suite_${module} PROPERTIES PREFIX "" OUTPUT_NAME "${module}"
    C_STANDARD 99 LIBRARY_OUTPUT_DIRECTORY "${suite_modules}")
  target_include_directories(lua_suite_${module} PRIVATE "${reference_dir}")
endforeach()
foreach(collector IN ITEMS incremental generational)
  add_test(NAME lua_library_string_lifetime_${collector}
    COMMAND "${CMAKE_COMMAND}" -E env "LUA_CPATH=${suite_modules}/?.so"
      "$<TARGET_FILE:luthr_test_host>" -l lib2-v2 -e
      "collectgarbage('${collector}'); for i=1,100 do lib2.newstr(string.rep('x',100)) end; collectgarbage('collect'); keep=setmetatable({}, {__gc=function() keep_string=lib2.newstr(string.rep('y',100)) end}); assert(lib2.id())")
  set_tests_properties(lua_library_string_lifetime_${collector} PROPERTIES
    LABELS "runtime;compatibility;parallel" TIMEOUT 15)
endforeach()
foreach(runtime IN ITEMS upstream luthr)
  if(runtime STREQUAL "upstream")
    set(suite_runtime upstream_lua)
  else()
    set(suite_runtime luthr_test_host)
  endif()
  add_test(NAME lua_official_${runtime} COMMAND "${CMAKE_COMMAND}"
    "-Druntime=$<TARGET_FILE:${suite_runtime}>" "-Darchive=${suite_archive}"
    "-Dmodule_dir=${suite_modules}" "-Dtarget=${LUTHR_TARGET}"
    "-Dwork_dir=${CMAKE_CURRENT_BINARY_DIR}/official-${runtime}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/OfficialLua.cmake")
  set_tests_properties(lua_official_${runtime} PROPERTIES LABELS "runtime;compatibility" TIMEOUT 900)
endforeach()

set(benchmark_arguments
  "-Dupstream=$<TARGET_FILE:benchmark_upstream>" "-Dluthr=$<TARGET_FILE:benchmark_luthr>"
  "-Dscript=${CMAKE_CURRENT_SOURCE_DIR}/benchmark.lua"
  "-Dmodule_dir=$<TARGET_FILE_DIR:compat_fixture>"
  "-Dtarget=${LUTHR_TARGET}" "-Dconfig=${CMAKE_BUILD_TYPE}"
  "-Dcompiler=${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
add_custom_target(benchmark_lua
  COMMAND "${CMAKE_COMMAND}" -E env GLIBC_TUNABLES=glibc.pthread.stack_cache_size=0
    "${CMAKE_COMMAND}" ${benchmark_arguments}
    "-Doutput_dir=${PROJECT_BINARY_DIR}/benchmarks/lua"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/BenchmarkLua.cmake"
  DEPENDS benchmark_upstream benchmark_luthr compat_fixture
  VERBATIM USES_TERMINAL)
add_test(NAME lua_benchmark_smoke COMMAND "${CMAKE_COMMAND}" ${benchmark_arguments}
  -Dsamples=2 -Dwarmups=1 -Dwork_divisor=100
  "-Doutput_dir=${CMAKE_CURRENT_BINARY_DIR}/benchmark-smoke"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/BenchmarkSmoke.cmake")
set_tests_properties(lua_benchmark_smoke PROPERTIES LABELS "runtime;compatibility" TIMEOUT 30)

foreach(case IN ITEMS language native files_modules metatables diagnostics coroutine_close hooks locale coroutine_pipeline)
  foreach(collector IN ITEMS incremental generational)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${case}-${collector}/direct")
    add_test(NAME lua_e2e_${case}_${collector} COMMAND luthr_test_host
      "${CMAKE_CURRENT_SOURCE_DIR}/entry.lua" "$<TARGET_FILE_DIR:compat_fixture>"
      "${CMAKE_CURRENT_SOURCE_DIR}/${case}.lua" "${collector}"
      "${CMAKE_CURRENT_BINARY_DIR}/${case}-${collector}/direct")
    set_tests_properties(lua_e2e_${case}_${collector}
      PROPERTIES LABELS "runtime;compatibility;memcheck" TIMEOUT 30)
    add_test(NAME lua_compat_${case}_${collector} COMMAND "${CMAKE_COMMAND}"
      "-Dupstream=$<TARGET_FILE:upstream_lua>" "-Dluthr=$<TARGET_FILE:luthr_test_host>"
      "-Dupstream_compiler=$<TARGET_FILE:upstream_luac>"
      "-Dluthr_compiler=$<TARGET_FILE:luthrc>" "-Dloader=${LUTHR_RUNTIME_LOADER}"
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
