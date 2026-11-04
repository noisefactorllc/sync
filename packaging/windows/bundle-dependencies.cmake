# Copies every non-system runtime DLL that the bundle's executables and the
# camera source need into the staged bundle. This is the Windows counterpart
# to dylibbundler in scripts/package-macos.sh: the installed app must not
# depend on a libuv, OpenSSL, or Visual C++ runtime that happens to be on the
# build machine.
#
# CMake's own resolver is used rather than a bespoke import-table scan so the
# bundle carries exactly what the linker recorded, and so the exclusion rules
# for OS-provided DLLs stay the ones CMake maintains.
#
# The Visual C++ runtime is the exception to those rules. A build machine has
# it in System32, where the resolver finds it first and would exclude it as
# part of Windows. Each runtime DLL the import tables name is instead copied
# from SYNC_RUNTIME_DIR, the redistributable folder of the toolset that built
# the binaries.
#
# Run with:
#   cmake -DSYNC_BUNDLE_DIR=<dir> -DSYNC_RUNTIME_DIR=<dir> [-DSYNC_SEARCH_PATH=<dir>] -P this-script

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED SYNC_BUNDLE_DIR OR SYNC_BUNDLE_DIR STREQUAL "")
  message(FATAL_ERROR "bundle-dependencies: SYNC_BUNDLE_DIR is required")
endif()
if(NOT IS_DIRECTORY "${SYNC_BUNDLE_DIR}")
  message(FATAL_ERROR "bundle-dependencies: not a directory: ${SYNC_BUNDLE_DIR}")
endif()
if(NOT DEFINED SYNC_RUNTIME_DIR OR NOT EXISTS "${SYNC_RUNTIME_DIR}/vcruntime140.dll")
  message(FATAL_ERROR
    "bundle-dependencies: SYNC_RUNTIME_DIR must be the toolset's redistributable "
    "Visual C++ runtime folder, got '${SYNC_RUNTIME_DIR}'")
endif()

# A runtime DLL is post-included by its resolved path in System32, so the
# resolver reports it instead of excluding it; the copy below then takes the
# redistributable file of the same name. Windows file names ignore case, so
# each pattern does too.
file(GLOB _runtime_files LIST_DIRECTORIES false "${SYNC_RUNTIME_DIR}/*.dll")
set(_runtime_names)
set(_runtime_regexes)
foreach(_runtime IN LISTS _runtime_files)
  get_filename_component(_runtime_name "${_runtime}" NAME)
  string(TOLOWER "${_runtime_name}" _runtime_name)
  list(APPEND _runtime_names "${_runtime_name}")
  set(_runtime_pattern "")
  string(LENGTH "${_runtime_name}" _length)
  math(EXPR _last "${_length} - 1")
  foreach(_index RANGE ${_last})
    string(SUBSTRING "${_runtime_name}" ${_index} 1 _character)
    string(TOUPPER "${_character}" _upper)
    if(_character STREQUAL ".")
      string(APPEND _runtime_pattern "[.]")
    elseif(NOT _character STREQUAL _upper)
      string(APPEND _runtime_pattern "[${_character}${_upper}]")
    else()
      string(APPEND _runtime_pattern "${_character}")
    endif()
  endforeach()
  list(APPEND _runtime_regexes "(^|[/\\\\])${_runtime_pattern}$")
endforeach()

set(_executables
  "${SYNC_BUNDLE_DIR}/Sync.exe"
  "${SYNC_BUNDLE_DIR}/syncd.exe"
)
foreach(_executable IN LISTS _executables)
  if(NOT EXISTS "${_executable}")
    message(FATAL_ERROR "bundle-dependencies: missing ${_executable}")
  endif()
endforeach()
# The render helper is staged only by a build with SYNC_BUILD_RENDER. Its Qt
# DLLs are already beside it (windeployqt); this finds anything else it links.
if(EXISTS "${SYNC_BUNDLE_DIR}/sync-render.exe")
  list(APPEND _executables "${SYNC_BUNDLE_DIR}/sync-render.exe")
endif()

# The camera source is loaded by the Windows Frame Server rather than started,
# so it is a library here, and it needs the same runtime the executables do.
set(_libraries "${SYNC_BUNDLE_DIR}/SyncCamera.dll")
if(NOT EXISTS "${_libraries}")
  message(FATAL_ERROR "bundle-dependencies: missing ${_libraries}")
endif()

set(_search_directories "${SYNC_BUNDLE_DIR}")
if(DEFINED SYNC_SEARCH_PATH AND NOT SYNC_SEARCH_PATH STREQUAL "")
  if(NOT IS_DIRECTORY "${SYNC_SEARCH_PATH}")
    message(FATAL_ERROR "bundle-dependencies: search path is not a directory: ${SYNC_SEARCH_PATH}")
  endif()
  list(APPEND _search_directories "${SYNC_SEARCH_PATH}")
endif()

file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES ${_executables}
  LIBRARIES ${_libraries}
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
  POST_INCLUDE_REGEXES
    ${_runtime_regexes}
  POST_EXCLUDE_REGEXES
    "[Ss]ystem32"
    "[Ww]inSxS"
)

foreach(_dependency IN LISTS _resolved)
  get_filename_component(_name "${_dependency}" NAME)
  string(TOLOWER "${_name}" _lower_name)
  if(EXISTS "${SYNC_BUNDLE_DIR}/${_name}")
    continue()
  endif()
  list(FIND _runtime_names "${_lower_name}" _runtime_index)
  if(_runtime_index GREATER -1)
    list(GET _runtime_files ${_runtime_index} _runtime)
    message(STATUS "bundling ${_name} from the Visual C++ redistributable")
    file(COPY "${_runtime}" DESTINATION "${SYNC_BUNDLE_DIR}")
    continue()
  endif()
  message(STATUS "bundling ${_name}")
  file(COPY "${_dependency}" DESTINATION "${SYNC_BUNDLE_DIR}")
endforeach()

# A runtime DLL the build machine never installed does not resolve at all.
# The import tables still name it, so it is copied from the redistributable
# folder like the rest. Searching that folder instead would resolve the same
# runtime to two places and stop the resolver.
set(_still_unresolved)
foreach(_dependency IN LISTS _unresolved)
  string(TOLOWER "${_dependency}" _lower_name)
  list(FIND _runtime_names "${_lower_name}" _runtime_index)
  if(_runtime_index GREATER -1)
    list(GET _runtime_files ${_runtime_index} _runtime)
    message(STATUS "bundling ${_dependency} from the Visual C++ redistributable")
    file(COPY "${_runtime}" DESTINATION "${SYNC_BUNDLE_DIR}")
  else()
    list(APPEND _still_unresolved "${_dependency}")
  endif()
endforeach()
set(_unresolved ${_still_unresolved})

# An unresolved dependency means the installed app would fail to start on a
# machine that lacks that DLL. Failing here is the whole point of the step, so
# it must never be downgraded to a warning.
if(_unresolved)
  message(FATAL_ERROR
    "bundle-dependencies: unresolved runtime dependencies: ${_unresolved}\n"
    "Pass -DSYNC_SEARCH_PATH=<dir> pointing at the directory holding them.")
endif()
