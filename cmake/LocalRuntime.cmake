# Private execution paths are used only by non-shipped local test programs.
if(LUTHR_TARGET STREQUAL "x86_64-linux-musl")
  set(LUTHR_RUNTIME_LOADER "${CMAKE_SYSROOT}/lib/ld-musl-x86_64.so.1")
else()
  set(LUTHR_RUNTIME_LOADER "${CMAKE_SYSROOT}/lib/ld-linux-x86-64.so.2")
endif()
if(NOT EXISTS "${LUTHR_RUNTIME_LOADER}")
  message(FATAL_ERROR "Selected collection loader missing: ${LUTHR_RUNTIME_LOADER}")
endif()
set(LUTHR_LOCAL_LINK_OPTIONS
  "-Wl,--dynamic-linker,${LUTHR_RUNTIME_LOADER}"
  "-Wl,--disable-new-dtags"
  "-Wl,-rpath,${CMAKE_SYSROOT}/lib:${CMAKE_SYSROOT}/usr/lib")
function(luthr_local_runtime target)
  target_link_options(${target} PRIVATE ${LUTHR_LOCAL_LINK_OPTIONS})
  set_property(TARGET ${target} PROPERTY CROSSCOMPILING_EMULATOR "")
endfunction()
