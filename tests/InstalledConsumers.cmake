function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed consumer command failed (${result}): ${ARGV}")
  endif()
endfunction()
file(REMOVE_RECURSE "${build_dir}/relocated-sdk" "${build_dir}/consumer")
file(RENAME "${build_dir}/sdk" "${build_dir}/relocated-sdk")
run("${CMAKE_COMMAND}" -E env PKG_CONFIG_PATH= PKG_CONFIG_SYSROOT_DIR=
  "PKG_CONFIG_LIBDIR=${build_dir}/relocated-sdk/lib/pkgconfig"
  "${CMAKE_COMMAND}" -S "${source_dir}/tests/consumer" -B "${build_dir}/consumer"
  -G Ninja "-DCMAKE_TOOLCHAIN_FILE=${source_dir}/cmake/toolchains/bootlin.cmake"
  "-DCPKT_LIFECYCLE_SCRIPTS_DIR=${lifecycle_scripts}" "-DLUTHRUM_TARGET=${target}"
  "-DCMAKE_BUILD_TYPE=${config}" "-DCMAKE_PREFIX_PATH=${build_dir}/relocated-sdk"
  "-DCMAKE_FIND_ROOT_PATH=${build_dir}/relocated-sdk"
  "-DLUTHRUM_SOURCE_DIR=${source_dir}")
run("${CMAKE_COMMAND}" --build "${build_dir}/consumer" --parallel 1)
run("${CMAKE_CTEST_COMMAND}" --test-dir "${build_dir}/consumer"
  --parallel 1 --stop-on-failure --output-on-failure --no-tests=error)
