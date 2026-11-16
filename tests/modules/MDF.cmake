# Released development inputs are prepared explicitly, never fetched by configure.
set(LUTHRUM_MDF_SOURCE "${PROJECT_SOURCE_DIR}/.cache/modules/libmdf-lua-0.23.1")
set(LUTHRUM_MDF_SDK "${PROJECT_SOURCE_DIR}/.cache/modules/libmdf-0.23.1-${LUTHRUM_TARGET}")
foreach(path IN ITEMS "${LUTHRUM_MDF_SOURCE}/lua/libmdf_core.c"
    "${LUTHRUM_MDF_SOURCE}/lua/libmdf.lua" "${LUTHRUM_MDF_SDK}/lib/libmdf.a")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "Required MDF acceptance input missing: ${path}. Run make prepare-modules MODULE=libmdf MODULE_ARCHIVE_DIR=/path/to/libmdf-0.23.1-artifacts")
  endif()
endforeach()
find_package(libmdf 0.23.1 EXACT CONFIG REQUIRED
  PATHS "${LUTHRUM_MDF_SDK}/lib/cmake/libmdf" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
add_library(mdf_binding OBJECT "${LUTHRUM_MDF_SOURCE}/lua/libmdf_core.c")
set_target_properties(mdf_binding PROPERTIES C_STANDARD 90 POSITION_INDEPENDENT_CODE ON)
target_compile_options(mdf_binding PRIVATE -Wall -Wextra -Werror)
target_include_directories(mdf_binding PRIVATE "${PROJECT_SOURCE_DIR}/lua/src")
target_link_libraries(mdf_binding PRIVATE libmdf::mdf_static)
