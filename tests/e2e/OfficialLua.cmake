# Each runtime gets a fresh, owned working copy; the suite creates files.
file(MAKE_DIRECTORY "${work_dir}")
set(suite "${work_dir}/lua-5.5.1-tests")
file(REMOVE_RECURSE "${suite}")
file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${work_dir}")
if(target STREQUAL "x86_64-linux-musl")
  # The pinned musl accepts these locale names without Portuguese collation,
  # character classes or decimal separators. Pristine Lua fails these same
  # assumptions. Keep all other tests; locale.lua compares actual libc results.
  file(READ "${suite}/strings.lua" source)
  string(REPLACE "if not _port then\n\n  local locales ="
    "if false then -- Portuguese locale data unavailable in pinned musl\n\n  local locales ="
    adjusted "${source}")
  if(adjusted STREQUAL source)
    message(FATAL_ERROR "Official strings locale guard not found")
  endif()
  file(WRITE "${suite}/strings.lua" "${adjusted}")
  file(READ "${suite}/literals.lua" source)
  string(REPLACE "if os.setlocale(\"pt_BR\") or os.setlocale(\"ptb\") then"
    "if false then -- Portuguese locale data unavailable in pinned musl"
    adjusted "${source}")
  if(adjusted STREQUAL source)
    message(FATAL_ERROR "Official literals locale guard not found")
  endif()
  file(WRITE "${suite}/literals.lua" "${adjusted}")
  message(STATUS "Pinned musl: two Portuguese-locale assumption blocks use differential locale E2E coverage")
endif()
file(MAKE_DIRECTORY "${suite}/libs/P1")
foreach(module IN ITEMS lib1 lib11 lib2 lib21 lib2-v2)
  file(COPY "${module_dir}/${module}.so" DESTINATION "${suite}/libs")
endforeach()
# The I/O tests require non-seekable stdin, including when CTest uses /dev/null.
execute_process(COMMAND "${CMAKE_COMMAND}" -E true
  COMMAND "${runtime}" -E all.lua WORKING_DIRECTORY "${suite}"
  RESULT_VARIABLE result OUTPUT_FILE "${work_dir}/stdout.log"
  ERROR_FILE "${work_dir}/stderr.log")
if(NOT result EQUAL 0)
  file(READ "${work_dir}/stderr.log" error)
  message(FATAL_ERROR "Official Lua suite failed (${result}): ${runtime}\n${error}\nOutput: ${work_dir}/stdout.log")
endif()
file(STRINGS "${work_dir}/stdout.log" completion REGEX "^final OK !!!$")
if(NOT completion)
  message(FATAL_ERROR "Official Lua suite did not finish: ${work_dir}/stdout.log")
endif()
