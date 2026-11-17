cmake_minimum_required(VERSION 3.25)
if(NOT CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Luthr requires CMAKE_TOOLCHAIN_FILE=cmake/toolchains/bootlin.cmake; use make configure")
endif()
project(luthr VERSION 0.1.0 LANGUAGES C ASM)

if(NOT LUTHR_BOOTLIN_READY OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
    NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR
    NOT LUTHR_TARGET MATCHES "^x86_64-linux-(gnu|musl)$")
  message(FATAL_ERROR "Configure with cmake/toolchains/bootlin.cmake and an x86_64 Linux target")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INSTALL_LIBDIR lib CACHE STRING "Library directory")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set(MEMORYCHECK_COMMAND_OPTIONS "--error-exitcode=1 --leak-check=full --errors-for-leak-kinds=definite,indirect --show-leak-kinds=definite,indirect --fair-sched=yes")
find_package(Threads REQUIRED)
find_path(VALGRIND_CLIENT_INCLUDE_DIR valgrind.h PATH_SUFFIXES valgrind
  NO_CMAKE_FIND_ROOT_PATH REQUIRED)
configure_file("${VALGRIND_CLIENT_INCLUDE_DIR}/valgrind.h"
  "${CMAKE_CURRENT_BINARY_DIR}/valgrind-client/valgrind.h" COPYONLY)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(CTest)
include(cmake/LocalRuntime.cmake)
option(LUTHR_THREAD_SANITIZER "Instrument GNU runtime and tests for race checking" OFF)
if(LUTHR_THREAD_SANITIZER)
  if(NOT LUTHR_TARGET STREQUAL "x86_64-linux-gnu" OR
      NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "ThreadSanitizer requires the pinned x86_64-linux-gnu compiler")
  endif()
  execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-file-name=libtsan.so
    RESULT_VARIABLE sanitizer_result OUTPUT_VARIABLE sanitizer_library
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT sanitizer_result EQUAL 0 OR NOT EXISTS "${sanitizer_library}")
    message(FATAL_ERROR "The pinned compiler's ThreadSanitizer runtime is unavailable")
  endif()
  get_filename_component(sanitizer_runtime_dir "${sanitizer_library}" DIRECTORY)
  add_compile_options("$<$<COMPILE_LANGUAGE:C>:-fsanitize=thread;-fno-omit-frame-pointer>")
  add_link_options(-fsanitize=thread "LINKER:-rpath,${sanitizer_runtime_dir}")
endif()
add_compile_options(
  "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=."
  "-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=build"
  "-ffile-prefix-map=${LUTHR_TOOLCHAIN_ROOT}=toolchain"
  "-ffile-prefix-map=${CMAKE_SYSROOT}=")

function(luthr_c89 target)
  target_compile_options(${target} PRIVATE
    -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror)
endfunction()

set(generated_include "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
set(facade_header "${generated_include}/luthr/lua.h")
set(facade_source "${CMAKE_CURRENT_BINARY_DIR}/generated/lua_facade.c")
set(export_map "${CMAKE_CURRENT_BINARY_DIR}/generated/exports.map")
add_custom_command(OUTPUT "${facade_header}" "${facade_source}" "${export_map}"
  COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_lua_c89_facade.py"
    --include-dir "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime"
    --header "${facade_header}" --source "${facade_source}"
    --runtime-header "${CMAKE_CURRENT_SOURCE_DIR}/include/luthr/lua_runtime.h"
    --task-header "${CMAKE_CURRENT_SOURCE_DIR}/include/luthr/tasks.h"
    --exports "${export_map}"
  DEPENDS tools/generate_lua_c89_facade.py tools/generated_output_paths.py
    src/runtime/lua.h src/runtime/luaconf.h src/runtime/lauxlib.h src/runtime/lualib.h
    include/luthr/lua_runtime.h include/luthr/tasks.h
  VERBATIM)
add_custom_target(luthr_facade_header DEPENDS "${facade_header}")

set(lua_sources
  laccess lapi lcallback lcode lcontext lcoroscope lctype ldebug ldo ldump lexec lfunc lgate lgc llex lmem lmempage lobject lopcodes
  lparser lpipe lpolicy lscheduler lstate lstring ltable ltask ltm lundump lvm lworld lzio lauxlib lbaselib lcorolib
  ldblib liolib lmathlib loadlib loslib lstrlib ltablib lutf8lib linit)
list(TRANSFORM lua_sources PREPEND "src/runtime/")
list(TRANSFORM lua_sources APPEND ".c")
add_library(luthr_lua_objects OBJECT ${lua_sources} src/runtime/lcontext_x86_64.S)
target_include_directories(luthr_lua_objects PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/valgrind-client")
set_target_properties(luthr_lua_objects PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_compile_options(luthr_lua_objects PRIVATE
  "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-mtls-dialect=gnu2>")
target_compile_definitions(luthr_lua_objects PRIVATE LUA_USE_LINUX)
target_compile_definitions(luthr_lua_objects PRIVATE
  "$<$<CONFIG:Debug>:LUAI_ASSERT;LUA_USE_APICHECK>")
target_link_libraries(luthr_lua_objects PRIVATE Threads::Threads)

add_library(luthr_facade_objects OBJECT "${facade_source}" src/lua_runtime.c)
add_dependencies(luthr_facade_objects luthr_facade_header)
set_target_properties(luthr_facade_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
luthr_c89(luthr_facade_objects)
target_link_libraries(luthr_facade_objects PRIVATE Threads::Threads)
# Only implementations including native Lua headers need these native types.
target_compile_options(luthr_facade_objects PRIVATE -Wno-long-long)
target_compile_definitions(luthr_facade_objects PRIVATE
  LLONG_MAX=9223372036854775807LL LLONG_MIN=-9223372036854775807LL-1LL
  ULLONG_MAX=18446744073709551615ULL)
target_include_directories(luthr_facade_objects PRIVATE
  include "${generated_include}" src/runtime)

add_library(luthr_task_objects OBJECT src/policy.c src/tasks.c src/cpu_capacity.c)
add_dependencies(luthr_task_objects luthr_facade_header)
set_target_properties(luthr_task_objects PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_compile_options(luthr_task_objects PRIVATE -Wall -Wextra -Werror -mtls-dialect=gnu2)
target_include_directories(luthr_task_objects PRIVATE include "${generated_include}" src/runtime)
target_link_libraries(luthr_task_objects PRIVATE Threads::Threads)

foreach(linkage IN ITEMS static shared)
  string(TOUPPER "${linkage}" kind)
  add_library(luthr_${linkage} ${kind}
    $<TARGET_OBJECTS:luthr_lua_objects> $<TARGET_OBJECTS:luthr_facade_objects>
    $<TARGET_OBJECTS:luthr_task_objects>)
  add_library(luthr::${linkage} ALIAS luthr_${linkage})
  set_target_properties(luthr_${linkage} PROPERTIES OUTPUT_NAME luthr EXPORT_NAME ${linkage})
  target_include_directories(luthr_${linkage} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${generated_include}>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/runtime>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  target_link_libraries(luthr_${linkage} PUBLIC m ${CMAKE_DL_LIBS} Threads::Threads)
endforeach()
set_target_properties(luthr_shared PROPERTIES
  VERSION ${PROJECT_VERSION} SOVERSION 0
  BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
target_link_options(luthr_shared PRIVATE
  "LINKER:--version-script=${export_map}")
set_property(TARGET luthr_shared APPEND PROPERTY LINK_DEPENDS
  "${export_map}")

add_executable(luthr src/runtime/lua.c)
target_compile_definitions(luthr PRIVATE LUA_USE_LINUX)
set_target_properties(luthr PROPERTIES C_STANDARD 99 INSTALL_RPATH "$ORIGIN/../lib")
target_link_libraries(luthr PRIVATE luthr_shared)
add_executable(luthrc src/runtime/luac.c)
set_target_properties(luthrc PROPERTIES C_STANDARD 99)
target_link_libraries(luthrc PRIVATE luthr_static)

install(TARGETS luthr_static luthr_shared luthr luthrc
  EXPORT luthrTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "${facade_header}" include/luthr/lua_runtime.h include/luthr/tasks.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/luthr)
install(FILES src/runtime/lua.h src/runtime/luaconf.h src/runtime/lauxlib.h
  src/runtime/lualib.h src/runtime/lua.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/luthr/docs)
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/luthr)
install(FILES src/runtime/lua.h DESTINATION ${CMAKE_INSTALL_DATADIR}/luthr/src/runtime)
configure_package_config_file(cmake/luthrConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/luthrConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/luthr)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/luthrConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(EXPORT luthrTargets NAMESPACE luthr:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/luthr)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/luthrConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/luthrConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/luthr)
configure_file(cmake/luthr.pc.in "${CMAKE_CURRENT_BINARY_DIR}/luthr.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/luthr.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
