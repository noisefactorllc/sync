# Every way Syphon discovery can end without a usable server class must name
# itself. `available:false` on its own sent an operator hunting for an
# afternoon in August 2026: the packaged app shipped a framework that loads
# cleanly but defines no SyphonMetalServer, and nothing said so.
if(NOT DEFINED SYNC_SYPHON_PROBE)
  message(FATAL_ERROR "SYNC_SYPHON_PROBE is required")
endif()

if(NOT DEFINED SYNC_MISSING_FRAMEWORK_PATH)
  message(FATAL_ERROR "SYNC_MISSING_FRAMEWORK_PATH is required")
endif()

if(NOT DEFINED SYNC_STUB_FRAMEWORK_PATH)
  message(FATAL_ERROR "SYNC_STUB_FRAMEWORK_PATH is required")
endif()

function(expect_reason label expected)
  execute_process(
    COMMAND "${SYNC_SYPHON_PROBE}" ${ARGN}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "${label}: probe exited ${status}; stderr=${stderr}")
  endif()
  set(want "{\"available\":false,\"reason\":\"${expected}\"}\n")
  if(NOT stdout STREQUAL want)
    message(FATAL_ERROR "${label}: expected ${want} but got ${stdout}")
  endif()
endfunction()

# A path that resolves to no bundle at all.
expect_reason("missing framework"
  "no Syphon.framework was found in any searched location"
  "${SYNC_MISSING_FRAMEWORK_PATH}")

# The packaged-app defect: a real, loadable Mach-O in a framework directory
# that carries none of Syphon's Objective-C classes.
expect_reason("stub framework"
  "the Syphon.framework that loaded does not provide SyphonMetalServer"
  "${SYNC_STUB_FRAMEWORK_PATH}")
