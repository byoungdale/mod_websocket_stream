if(NOT DEFINED MODULE_FILE OR NOT EXISTS "${MODULE_FILE}")
    message(FATAL_ERROR "Module does not exist: ${MODULE_FILE}")
endif()

if(APPLE)
    set(NM_FLAGS -gU)
else()
    set(NM_FLAGS -gD)
endif()

execute_process(
    COMMAND nm ${NM_FLAGS} "${MODULE_FILE}"
    RESULT_VARIABLE NM_RESULT
    OUTPUT_VARIABLE NM_OUTPUT
    ERROR_VARIABLE NM_ERROR
)

if(NOT NM_RESULT EQUAL 0)
    message(FATAL_ERROR "nm failed: ${NM_ERROR}")
endif()

if(NOT NM_OUTPUT MATCHES "mod_websocket_stream_module_interface")
    message(FATAL_ERROR
        "The FreeSWITCH module interface symbol was not exported by ${MODULE_FILE}")
endif()

message(STATUS "Verified FreeSWITCH module export in ${MODULE_FILE}")
