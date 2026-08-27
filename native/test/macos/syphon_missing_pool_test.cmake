if(NOT DEFINED SYNC_SYPHON_PROBE)
  message(FATAL_ERROR "SYNC_SYPHON_PROBE is required")
endif()

if(NOT DEFINED SYNC_MISSING_FRAMEWORK_PATH)
  message(FATAL_ERROR "SYNC_MISSING_FRAMEWORK_PATH is required")
endif()

function(run_probe label)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "OBJC_DEBUG_MISSING_POOLS=YES"
      "${SYNC_SYPHON_PROBE}" ${ARGN}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "${label}: probe exited ${status}; stderr=${stderr}")
  endif()
  if(NOT stdout MATCHES "^{\"available\":(true|false),\"reason\":\"[^\"]+\"}\n$")
    message(FATAL_ERROR "${label}: unexpected stdout=${stdout}")
  endif()
  if(NOT stderr STREQUAL "")
    message(FATAL_ERROR "${label}: missing-pool diagnostic on stderr=${stderr}")
  endif()
endfunction()

run_probe("default discovery")
run_probe("explicit missing framework" "${SYNC_MISSING_FRAMEWORK_PATH}")
