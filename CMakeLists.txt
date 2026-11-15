cmake_minimum_required(VERSION 3.25)
if(NOT CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Luthrum requires CMAKE_TOOLCHAIN_FILE=cmake/toolchains/bootlin.cmake; use make configure")
endif()
project(luthrum VERSION 0.1.0 LANGUAGES C)

if(NOT LUTHRUM_BOOTLIN_READY OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
    NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR
    NOT LUTHRUM_TARGET MATCHES "^x86_64-linux-(gnu|musl)$")
  message(FATAL_ERROR "Configure with cmake/toolchains/bootlin.cmake and an x86_64 Linux target")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INSTALL_LIBDIR lib CACHE STRING "Library directory")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set(MEMORYCHECK_COMMAND_OPTIONS "--error-exitcode=1 --leak-check=full --errors-for-leak-kinds=definite,indirect --show-leak-kinds=definite,indirect --fair-sched=yes")
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(CTest)
include(cmake/LocalRuntime.cmake)
add_compile_options(
  "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=."
  "-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=build"
  "-ffile-prefix-map=${LUTHRUM_TOOLCHAIN_ROOT}=toolchain"
  "-ffile-prefix-map=${CMAKE_SYSROOT}=")

function(luthrum_c89 target)
  target_compile_options(${target} PRIVATE
    -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror)
endfunction()

set(generated_include "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
set(facade_header "${generated_include}/luthrum/lua.h")
set(facade_source "${CMAKE_CURRENT_BINARY_DIR}/generated/lua_facade.c")
set(export_map "${CMAKE_CURRENT_BINARY_DIR}/generated/exports.map")
add_custom_command(OUTPUT "${facade_header}" "${facade_source}" "${export_map}"
  COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_lua_c89_facade.py"
    --include-dir "${CMAKE_CURRENT_SOURCE_DIR}/lua/src"
    --header "${facade_header}" --source "${facade_source}"
    --runtime-header "${CMAKE_CURRENT_SOURCE_DIR}/include/luthrum/lua_runtime.h"
    --exports "${export_map}"
  DEPENDS tools/generate_lua_c89_facade.py tools/generated_output_paths.py
    lua/src/lua.h lua/src/luaconf.h lua/src/lauxlib.h lua/src/lualib.h
    include/luthrum/lua_runtime.h
  VERBATIM)
add_custom_target(luthrum_facade_header DEPENDS "${facade_header}")

set(lua_sources
  laccess lapi lcode lctype ldebug ldo ldump lfunc lgate lgc llex lmem lobject lopcodes
  lparser lroots lstate lstring ltable ltm lundump lvm lworld lzio lauxlib lbaselib lcorolib
  ldblib liolib lmathlib loadlib loslib lstrlib ltablib lutf8lib linit)
list(TRANSFORM lua_sources PREPEND "lua/src/")
list(TRANSFORM lua_sources APPEND ".c")
add_library(luthrum_lua_objects OBJECT ${lua_sources})
set_target_properties(luthrum_lua_objects PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_compile_options(luthrum_lua_objects PRIVATE -Wall -Wextra -Werror -mtls-dialect=gnu2)
target_compile_definitions(luthrum_lua_objects PRIVATE LUA_USE_LINUX)
target_compile_definitions(luthrum_lua_objects PRIVATE
  "$<$<CONFIG:Debug>:LUAI_ASSERT;LUA_USE_APICHECK>")
target_link_libraries(luthrum_lua_objects PRIVATE Threads::Threads)

add_library(luthrum_facade_objects OBJECT "${facade_source}" src/lua_runtime.c)
add_dependencies(luthrum_facade_objects luthrum_facade_header)
set_target_properties(luthrum_facade_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
luthrum_c89(luthrum_facade_objects)
target_link_libraries(luthrum_facade_objects PRIVATE Threads::Threads)
# Only implementations including native Lua headers need these native types.
target_compile_options(luthrum_facade_objects PRIVATE -Wno-long-long)
target_compile_definitions(luthrum_facade_objects PRIVATE
  LLONG_MAX=9223372036854775807LL LLONG_MIN=-9223372036854775807LL-1LL
  ULLONG_MAX=18446744073709551615ULL)
target_include_directories(luthrum_facade_objects PRIVATE
  include "${generated_include}" lua/src)

foreach(linkage IN ITEMS static shared)
  string(TOUPPER "${linkage}" kind)
  add_library(luthrum_${linkage} ${kind}
    $<TARGET_OBJECTS:luthrum_lua_objects> $<TARGET_OBJECTS:luthrum_facade_objects>)
  add_library(luthrum::${linkage} ALIAS luthrum_${linkage})
  set_target_properties(luthrum_${linkage} PROPERTIES OUTPUT_NAME luthrum EXPORT_NAME ${linkage})
  target_include_directories(luthrum_${linkage} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${generated_include}>"
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lua/src>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  target_link_libraries(luthrum_${linkage} PUBLIC m ${CMAKE_DL_LIBS} Threads::Threads)
endforeach()
set_target_properties(luthrum_shared PROPERTIES
  VERSION ${PROJECT_VERSION} SOVERSION 0
  BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
target_link_options(luthrum_shared PRIVATE
  "LINKER:--version-script=${export_map}")
set_property(TARGET luthrum_shared APPEND PROPERTY LINK_DEPENDS
  "${export_map}")

add_executable(luthrum lua/src/lua.c)
target_compile_definitions(luthrum PRIVATE LUA_USE_LINUX)
set_target_properties(luthrum PROPERTIES C_STANDARD 99 INSTALL_RPATH "$ORIGIN/../lib")
target_link_libraries(luthrum PRIVATE luthrum_shared)
add_executable(luthrumc lua/src/luac.c)
set_target_properties(luthrumc PROPERTIES C_STANDARD 99)
target_link_libraries(luthrumc PRIVATE luthrum_static)

install(TARGETS luthrum_static luthrum_shared luthrum luthrumc
  EXPORT luthrumTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "${facade_header}" include/luthrum/lua_runtime.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/luthrum)
install(FILES lua/src/lua.h lua/src/luaconf.h lua/src/lauxlib.h
  lua/src/lualib.h lua/src/lua.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/luthrum/docs)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/luthrum)
install(DIRECTORY lua/doc/ DESTINATION ${CMAKE_INSTALL_DATADIR}/luthrum/lua/doc)
install(FILES lua/README DESTINATION ${CMAKE_INSTALL_DATADIR}/luthrum/lua)
install(FILES lua/src/lua.h DESTINATION ${CMAKE_INSTALL_DATADIR}/luthrum/lua/src)
configure_package_config_file(cmake/luthrumConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/luthrumConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/luthrum)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/luthrumConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(EXPORT luthrumTargets NAMESPACE luthrum:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/luthrum)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/luthrumConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/luthrumConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/luthrum)
configure_file(cmake/luthrum.pc.in "${CMAKE_CURRENT_BINARY_DIR}/luthrum.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/luthrum.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
