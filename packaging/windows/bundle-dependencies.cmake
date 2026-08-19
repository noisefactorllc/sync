# Copies every non-system runtime DLL that Sync.exe and syncd.exe need into the
# staged bundle. This is the Windows counterpart to dylibbundler in
# scripts/package-macos.sh: the installed app must not depend on a libuv,
# OpenSSL, or MSVC runtime that happens to be on the build machine.
#
# CMake's own resolver is used rather than a bespoke import-table scan so the
# bundle carries exactly what the linker recorded, and so the exclusion rules
# for OS-provided DLLs stay the ones CMake maintains.
#
# Run with:
#   cmake -DSYNC_BUNDLE_DIR=<dir> [-DSYNC_SEARCH_PATH=<dir>] -P this-script

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED SYNC_BUNDLE_DIR OR SYNC_BUNDLE_DIR STREQUAL "")
  message(FATAL_ERROR "bundle-dependencies: SYNC_BUNDLE_DIR is required")
endif()
if(NOT IS_DIRECTORY "${SYNC_BUNDLE_DIR}")
  message(FATAL_ERROR "bundle-dependencies: not a directory: ${SYNC_BUNDLE_DIR}")
endif()

set(_executables
  "${SYNC_BUNDLE_DIR}/Sync.exe"
  "${SYNC_BUNDLE_DIR}/syncd.exe"
)
foreach(_executable IN LISTS _executables)
  if(NOT EXISTS "${_executable}")
    message(FATAL_ERROR "bundle-dependencies: missing ${_executable}")
  endif()
endforeach()

set(_search_directories "${SYNC_BUNDLE_DIR}")
if(DEFINED SYNC_SEARCH_PATH AND NOT SYNC_SEARCH_PATH STREQUAL "")
  if(NOT IS_DIRECTORY "${SYNC_SEARCH_PATH}")
    message(FATAL_ERROR "bundle-dependencies: search path is not a directory: ${SYNC_SEARCH_PATH}")
  endif()
  list(APPEND _search_directories "${SYNC_SEARCH_PATH}")
endif()

file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES ${_executables}
  RESOLVED_DEPENDENCIES_VAR _resolved
  UNRESOLVED_DEPENDENCIES_VAR _unresolved
  DIRECTORIES ${_search_directories}
  # api-ms-win-* and ext-ms-* are the OS API sets; the rest live in System32 and
  # are part of Windows itself. Copying any of them would be both redundant and,
  # for the UCRT, wrong.
  PRE_EXCLUDE_REGEXES
    "^api-ms-win-.*"
    "^ext-ms-.*"
    "^hvsifiletrust\\.dll$"
    "^pdmutilities\\.dll$"
  POST_EXCLUDE_REGEXES
    "[Ss]ystem32"
    "[Ww]inSxS"
)

foreach(_dependency IN LISTS _resolved)
  get_filename_component(_name "${_dependency}" NAME)
  if(EXISTS "${SYNC_BUNDLE_DIR}/${_name}")
    continue()
  endif()
  message(STATUS "bundling ${_name}")
  file(COPY "${_dependency}" DESTINATION "${SYNC_BUNDLE_DIR}")
endforeach()

# An unresolved dependency means the installed app would fail to start on a
# machine that lacks that DLL. Failing here is the whole point of the step, so
# it must never be downgraded to a warning.
if(_unresolved)
  message(FATAL_ERROR
    "bundle-dependencies: unresolved runtime dependencies: ${_unresolved}\n"
    "Pass -DSYNC_SEARCH_PATH=<dir> pointing at the directory holding them.")
endif()
