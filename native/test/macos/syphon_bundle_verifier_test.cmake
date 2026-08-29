foreach(required IN ITEMS
    SYNC_BUNDLE_VERIFIER
    SYNC_INFO_PLIST
    SYNC_STUB_FRAMEWORK_BINARY
    SYNC_TEST_DIRECTORY
    SYNC_PRODUCT_VERSION)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()

set(bundle "${SYNC_TEST_DIRECTORY}/Sync.app")
set(contents "${bundle}/Contents")
set(framework_binary "${contents}/Frameworks/Syphon.framework/Syphon")

file(REMOVE_RECURSE "${SYNC_TEST_DIRECTORY}")
file(MAKE_DIRECTORY
  "${contents}/MacOS"
  "${contents}/Frameworks/Syphon.framework"
  "${contents}/Resources"
)
file(COPY_FILE "${SYNC_INFO_PLIST}" "${contents}/Info.plist")
file(COPY_FILE "${SYNC_STUB_FRAMEWORK_BINARY}" "${contents}/MacOS/Sync")
file(COPY_FILE "${SYNC_STUB_FRAMEWORK_BINARY}" "${contents}/MacOS/syncd")
file(COPY_FILE "${SYNC_STUB_FRAMEWORK_BINARY}" "${framework_binary}")
file(WRITE "${contents}/Resources/Sync.icns" "")
file(WRITE "${contents}/Resources/LICENSE.txt" "")
file(WRITE "${contents}/Resources/Third-Party-Notices.txt" "")

execute_process(
  COMMAND /usr/bin/nm -gU "${framework_binary}"
  RESULT_VARIABLE nm_status
  OUTPUT_VARIABLE fixture_symbols
  ERROR_VARIABLE nm_stderr
)
if(NOT nm_status EQUAL 0)
  message(FATAL_ERROR "could not inspect fixture symbols: ${nm_stderr}")
endif()
if(NOT fixture_symbols MATCHES "_OBJC_CLASS_\\\$_SyphonMetalServerFake")
  message(FATAL_ERROR "class-less fixture is missing its near-name decoy symbol")
endif()

# Keep this fixture focused on the Syphon-content check. The local compiler's
# deployment target can be newer than the package's advertised minimum.
execute_process(
  COMMAND /usr/bin/plutil -replace LSMinimumSystemVersion -string 99.0
    "${contents}/Info.plist"
  RESULT_VARIABLE plist_status
  ERROR_VARIABLE plist_stderr
)
if(NOT plist_status EQUAL 0)
  message(FATAL_ERROR "could not prepare fixture plist: ${plist_stderr}")
endif()

execute_process(
  COMMAND "${SYNC_BUNDLE_VERIFIER}" "${bundle}" "${SYNC_PRODUCT_VERSION}"
  RESULT_VARIABLE verifier_status
  OUTPUT_VARIABLE verifier_stdout
  ERROR_VARIABLE verifier_stderr
)
if(verifier_status EQUAL 0)
  message(FATAL_ERROR
    "bundle verifier accepted a class-less Syphon framework: ${verifier_stdout}")
endif()
if(NOT verifier_stderr MATCHES "does not export SyphonMetalServer")
  message(FATAL_ERROR
    "bundle verifier failed for the wrong reason: ${verifier_stderr}")
endif()
