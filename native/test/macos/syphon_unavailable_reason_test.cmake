# Every way Syphon discovery can end without a usable server class must name
# itself. `available:false` on its own sent an operator hunting for an
# afternoon in August 2026: a locally packaged app carried a framework that
# loads cleanly but defines no SyphonMetalServer, and nothing said so.
foreach(required IN ITEMS
    SYNC_SYPHON_PROBE
    SYNC_MISSING_FRAMEWORK_PATH
    SYNC_STUB_FRAMEWORK_PATH
    SYNC_INCOMPATIBLE_FRAMEWORK_PATH)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

foreach(fixture IN ITEMS SYNC_STUB_FRAMEWORK_PATH SYNC_INCOMPATIBLE_FRAMEWORK_PATH)
  if(NOT EXISTS "${${fixture}}/Syphon")
    # Without this the run fails on a confusing reason mismatch instead of the
    # real problem, which is that the fixture target was never built.
    message(FATAL_ERROR "${fixture} fixture is missing: ${${fixture}}")
  endif()
endforeach()

# Discovery falls through a rejected candidate to the next one, and the later
# candidates are paths this test does not own. On a machine where Syphon is
# installed somewhere discovery looks, every rejected fixture is followed by a
# real framework that succeeds -- so the truthful assertion there is that
# discovery succeeded, not that it failed with a particular reason.
#
# Asking the probe with no explicit path is exactly that question, and is
# better than testing a hardcoded location: it covers every candidate the
# consumer actually searches, including the private frameworks directory
# beside the probe, and cannot drift out of step with the search order.
execute_process(
  COMMAND "${SYNC_SYPHON_PROBE}"
  RESULT_VARIABLE ambient_status
  OUTPUT_VARIABLE ambient_stdout
  ERROR_VARIABLE ambient_stderr
)
if(NOT ambient_status EQUAL 0)
  message(FATAL_ERROR "ambient discovery: probe exited ${ambient_status}; stderr=${ambient_stderr}")
endif()
if(ambient_stdout MATCHES "\"available\":true")
  set(system_syphon_present TRUE)
  message(STATUS
    "sync_syphon_unavailable_reason: a usable Syphon is already discoverable, "
    "so each rejected fixture is expected to fall through to it and succeed")
else()
  set(system_syphon_present FALSE)
endif()

function(expect_probe label expected)
  execute_process(
    COMMAND "${SYNC_SYPHON_PROBE}" ${ARGN}
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
  )
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "${label}: probe exited ${status}; stderr=${stderr}")
  endif()
  if(NOT stdout STREQUAL "${expected}")
    message(FATAL_ERROR "${label}: expected ${expected} but got ${stdout}")
  endif()
endfunction()

# What a rejected candidate yields once discovery has run out of candidates.
function(expect_rejected label reason)
  if(system_syphon_present)
    expect_probe("${label}" "{\"available\":true,\"reason\":\"the Syphon runtime is available\"}\n" ${ARGN})
  else()
    expect_probe("${label}" "{\"available\":false,\"reason\":\"${reason}\"}\n" ${ARGN})
  endif()
endfunction()

# A path that resolves to no bundle at all.
expect_rejected("missing framework"
  "no Syphon.framework was found in any searched location"
  "${SYNC_MISSING_FRAMEWORK_PATH}")

# The packaged-app defect: a real, loadable Mach-O in a framework directory
# that carries none of Syphon's Objective-C classes.
expect_rejected("stub framework"
  "the Syphon.framework that loaded does not provide SyphonMetalServer"
  "${SYNC_STUB_FRAMEWORK_PATH}")

# A Syphon that registers the class without the selectors this daemon calls.
# Distinct from the stub on purpose: one says replace the framework, the other
# says the version is wrong.
#
# This leg needs no fall-through allowance, and that is the point. Objective-C
# class registration is process-global: with the fixture and a real framework
# both loaded the runtime warns that "one of the two will be used, which one
# is undefined", and in practice it keeps the first registration -- verified
# against a genuine Syphon.framework placed at the probe's private frameworks
# path, where discovery stayed incompatible rather than recovering.
#
# So the exact reason is asserted unconditionally. If that runtime behavior
# ever changed, this leg would start failing only on machines that have Syphon
# installed, which is precisely the machine-dependent failure this file's
# ambient check exists to avoid -- so treat a failure here as a real signal
# about the runtime, not as flake.
expect_probe("incompatible framework"
  "{\"available\":false,\"reason\":\"the Syphon.framework that loaded provides an incompatible SyphonMetalServer\"}\n"
  "${SYNC_INCOMPATIBLE_FRAMEWORK_PATH}")
