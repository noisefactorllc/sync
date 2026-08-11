if(NOT DEFINED SYNC_PAIRING_PROMPT_TESTS)
  message(FATAL_ERROR "SYNC_PAIRING_PROMPT_TESTS is required")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    "OBJC_DEBUG_MISSING_POOLS=YES"
    "LIBDISPATCH_DEBUG_MISSING_POOLS=NO"
    "${SYNC_PAIRING_PROMPT_TESTS}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
  TIMEOUT 15
)

if(NOT status EQUAL 0)
  message(FATAL_ERROR
    "pairing prompt diagnostic exited ${status}; stdout=${stdout}; stderr=${stderr}")
endif()
if(NOT stdout MATCHES "9/9 tests passed")
  message(FATAL_ERROR "pairing prompt diagnostic emitted unexpected stdout=${stdout}")
endif()
if(NOT stderr STREQUAL "")
  message(FATAL_ERROR "pairing prompt missing-pool diagnostic on stderr=${stderr}")
endif()
