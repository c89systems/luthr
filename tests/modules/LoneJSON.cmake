# Released development inputs are prepared explicitly, never fetched by configure.
set(LUTHRUM_LONEJSON_VERSION 0.50.0)
set(LUTHRUM_MODULE_CACHE "${PROJECT_SOURCE_DIR}/.cache/modules")
set(LUTHRUM_LONEJSON_SOURCE "${LUTHRUM_MODULE_CACHE}/lonejson-${LUTHRUM_LONEJSON_VERSION}")
set(LUTHRUM_LONEJSON_SDK "${LUTHRUM_MODULE_CACHE}/liblonejson-${LUTHRUM_LONEJSON_VERSION}-${LUTHRUM_TARGET}")
foreach(path IN ITEMS "${LUTHRUM_LONEJSON_SOURCE}/src/lua/lonejson_lua.c"
    "${LUTHRUM_LONEJSON_SOURCE}/lua/lonejson/init.lua"
    "${LUTHRUM_LONEJSON_SDK}/lib/liblonejson.a")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Required LoneJSON acceptance input missing: ${path}. Run make prepare-modules MODULE_ARCHIVE_DIR=/path/to/lonejson-0.50.0-artifacts")
  endif()
endforeach()
find_package(lonejson ${LUTHRUM_LONEJSON_VERSION} EXACT CONFIG REQUIRED
  PATHS "${LUTHRUM_LONEJSON_SDK}/lib/cmake/lonejson" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
add_library(lonejson_binding OBJECT "${LUTHRUM_LONEJSON_SOURCE}/src/lua/lonejson_lua.c")
set_target_properties(lonejson_binding PROPERTIES C_STANDARD 90 POSITION_INDEPENDENT_CODE ON)
target_compile_options(lonejson_binding PRIVATE -Wall -Wextra -Werror)
target_include_directories(lonejson_binding PRIVATE "${PROJECT_SOURCE_DIR}/lua/src")
target_link_libraries(lonejson_binding PRIVATE lonejson::lonejson_static)
