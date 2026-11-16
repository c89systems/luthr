execute_process(COMMAND "${readelf}" -d "${module}"
  RESULT_VARIABLE dynamic_result OUTPUT_VARIABLE dynamic)
execute_process(COMMAND "${nm}" -D --defined-only "${module}"
  RESULT_VARIABLE symbols_result OUTPUT_VARIABLE symbols)
if(NOT dynamic_result EQUAL 0 OR NOT symbols_result EQUAL 0)
  message(FATAL_ERROR "Could not inspect the installed native module")
endif()
if(dynamic MATCHES "lib(lua|luthrum|cpkt)" OR symbols MATCHES " [A-Za-z] lua(L)?_")
  message(FATAL_ERROR "The module must resolve Lua APIs from its embedding host")
endif()
if(NOT DEFINED opener OR NOT symbols MATCHES "${opener}")
  message(FATAL_ERROR "Native module opener missing")
endif()
