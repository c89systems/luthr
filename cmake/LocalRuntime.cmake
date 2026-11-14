# Private execution paths are used only by non-shipped local test programs.
if(LUTHRUM_TARGET STREQUAL "x86_64-linux-musl")
  set(LUTHRUM_RUNTIME_LOADER "${CMAKE_SYSROOT}/lib/ld-musl-x86_64.so.1")
else()
  set(LUTHRUM_RUNTIME_LOADER "${CMAKE_SYSROOT}/lib/ld-linux-x86-64.so.2")
endif()
if(NOT EXISTS "${LUTHRUM_RUNTIME_LOADER}")
  message(FATAL_ERROR "Selected collection loader missing: ${LUTHRUM_RUNTIME_LOADER}")
endif()
set(LUTHRUM_LOCAL_LINK_OPTIONS
  "-Wl,--dynamic-linker,${LUTHRUM_RUNTIME_LOADER}"
  "-Wl,--disable-new-dtags"
  "-Wl,-rpath,${CMAKE_SYSROOT}/lib:${CMAKE_SYSROOT}/usr/lib")
function(luthrum_local_runtime target)
  target_link_options(${target} PRIVATE ${LUTHRUM_LOCAL_LINK_OPTIONS})
  set_property(TARGET ${target} PROPERTY CROSSCOMPILING_EMULATOR "")
endfunction()
