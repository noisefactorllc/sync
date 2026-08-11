if(NOT DEFINED SYNC_METAL_PROBE)
  message(FATAL_ERROR "SYNC_METAL_PROBE is required")
endif()

execute_process(
  # Keep libdispatch's worker-thread fallback pools enabled. Without this setting,
  # OBJC_DEBUG_MISSING_POOLS disables those system pools and reports Metal's own
  # asynchronous telemetry block rather than a missing pool at our daemon boundary.
  COMMAND "${CMAKE_COMMAND}" -E env
    "OBJC_DEBUG_MISSING_POOLS=YES"
    "LIBDISPATCH_DEBUG_MISSING_POOLS=NO"
    "${SYNC_METAL_PROBE}"
  RESULT_VARIABLE status
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
  TIMEOUT 10
)

if(NOT status EQUAL 0)
  message(FATAL_ERROR "Metal publisher probe exited ${status}; stderr=${stderr}")
endif()
if(NOT stdout STREQUAL "{\"published\":true}\n")
  message(FATAL_ERROR "Metal publisher probe emitted unexpected stdout=${stdout}")
endif()
if(NOT stderr STREQUAL "")
  message(FATAL_ERROR "Metal publisher missing-pool diagnostic on stderr=${stderr}")
endif()
