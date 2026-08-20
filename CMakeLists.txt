cmake_minimum_required(VERSION 3.21)

project(noisefactor_sync_protocol VERSION 0.2.0 LANGUAGES CXX)

set(SYNC_PRODUCT_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Sync product version reported by the native companion")
set(SYNC_SYPHON_FRAMEWORK_PATH "" CACHE PATH
    "Path to the Syphon.framework bundled by the macOS packaging target")
set(SYNC_SPOUT_LIBRARY_PATH "" CACHE FILEPATH
    "Path to the SpoutLibrary.dll bundled by the Windows packaging target")
set(SYNC_DYLIB_SEARCH_PATH "" CACHE PATH
    "Additional directory searched for macOS bundle dependencies")
set(SYNC_WINDOWS_DEPENDENCY_PATH "" CACHE PATH
    "Additional directory searched for Windows bundle dependencies")

# Both of these have to precede every target definition to reach them all.
if(WIN32)
  # Every Windows source in this tree calls the ...W form of the Win32 API and
  # holds wide strings directly, so UNICODE is a property of the whole build,
  # not of one library's usage requirements. It was previously carried on
  # sync_protocol's PUBLIC interface, which meant a target linking it
  # PRIVATELY through another library silently compiled without it -- and a
  # resource id like IDI_APPLICATION then expands to its narrow form.
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
endif()

# Sources are UTF-8 and some carry non-ASCII text (prompt wording, test
# fixtures). Without /utf-8 MSVC reads them in the machine's ANSI codepage,
# which silently mangles those strings on a non-Western-European machine.
if(MSVC)
  add_compile_options(/utf-8)
  # /EHsc must be stated here, not left to CMake's default CMAKE_CXX_FLAGS.
  # That default is a *variable*, so any caller passing -DCMAKE_CXX_FLAGS=...
  # replaces it wholesale and silently drops /EHsc -- which is precisely what
  # a CI job passing an empty string for "no extra flags" did. Without it MSVC
  # compiles throw/catch but emits no unwind code for automatic objects, so
  # destructors do not run while an exception propagates. The websocket
  # payload guard, whose entire job is to wipe a token from memory during
  # unwinding, then does nothing at all -- a security guarantee quietly
  # downgraded by a build flag. Setting it as a compile option makes it part
  # of the project rather than something every caller has to remember to
  # preserve. Repeating it (the warnings matrix entry also names it) is
  # harmless.
  add_compile_options(/EHsc)
endif()

find_package(Threads REQUIRED)

find_package(OpenSSL 3 REQUIRED)

# libuv reaches this project two ways: a CONFIG package from vcpkg or Conan
# (the practical route on Windows) and pkg-config from Homebrew or a distro.
# Preferring CONFIG keeps the Windows build from needing a pkg-config port.
find_package(libuv CONFIG QUIET)
if(TARGET libuv::uv)
  set(SYNC_LIBUV_TARGET libuv::uv)
elseif(TARGET libuv::uv_a)
  set(SYNC_LIBUV_TARGET libuv::uv_a)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)
  set(SYNC_LIBUV_TARGET PkgConfig::LIBUV)
endif()

# The pairing store's durability, permission, and symlink guarantees are
# expressed in platform primitives that have no common subset, so the seam is
# a whole file rather than a scattering of #ifdefs.
if(WIN32)
  set(SYNC_PAIRING_STORE_PLATFORM_SOURCE
      native/src/platform/windows/pairing_store_fs.cpp)
else()
  set(SYNC_PAIRING_STORE_PLATFORM_SOURCE
      native/src/platform/posix/pairing_store_fs.cpp)
endif()

add_library(sync_protocol
  native/src/control.cpp
  native/src/frame_receiver.cpp
  native/src/origin.cpp
  native/src/pairing.cpp
  native/src/authority_worker.cpp
  native/src/pairing_store.cpp
  ${SYNC_PAIRING_STORE_PLATFORM_SOURCE}
  native/src/publisher_hub.cpp
  native/src/protocol.cpp
  native/src/secure_memory.cpp
  native/src/websocket.cpp
)
target_compile_features(sync_protocol PUBLIC cxx_std_20)
target_include_directories(sync_protocol PUBLIC native/include)
target_include_directories(sync_protocol PRIVATE native/src)
target_compile_definitions(sync_protocol PUBLIC
  SYNC_PRODUCT_VERSION="${SYNC_PRODUCT_VERSION}"
)
target_link_libraries(sync_protocol PUBLIC OpenSSL::Crypto Threads::Threads)
if(WIN32)
  # Advapi32 supplies the process-token and DACL calls the pairing store's
  # owner-only permissions are built from. Randomness already comes from
  # OpenSSL's RAND_priv_bytes on every platform, so no Windows CSPRNG is
  # linked here.
  # ws2_32 additionally supplies inet_pton, which origin.cpp uses as an IPv6
  # literal parser (see the comment there -- it needs no Winsock init).
  target_link_libraries(sync_protocol PUBLIC advapi32 ws2_32)
endif()

add_executable(sync_protocol_tests
  native/test/control_test.cpp
  native/test/frame_receiver_test.cpp
  native/test/publisher_hub_test.cpp
  native/test/server_contract_test.cpp
  native/test/test_main.cpp
  native/test/protocol_test.cpp
  native/test/websocket_test.cpp
  native/src/server.cpp
)
target_compile_features(sync_protocol_tests PRIVATE cxx_std_20)
target_link_libraries(sync_protocol_tests PRIVATE sync_protocol ${SYNC_LIBUV_TARGET})
target_compile_definitions(sync_protocol_tests PRIVATE SYNC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(syncd
  native/src/main.cpp
  native/src/server.cpp
)
target_compile_features(syncd PRIVATE cxx_std_20)
target_link_libraries(syncd PRIVATE sync_cli sync_ndi_publisher ${SYNC_LIBUV_TARGET})

enable_testing()
add_test(NAME sync_protocol_tests COMMAND sync_protocol_tests)

add_executable(sync_pairing_store_tests
  native/test/origin_test.cpp
  native/test/pairing_test.cpp
  native/test/pairing_store_test.cpp
  native/test/test_main.cpp
)
target_compile_features(sync_pairing_store_tests PRIVATE cxx_std_20)
target_link_libraries(sync_pairing_store_tests PRIVATE sync_protocol)
add_test(NAME sync_pairing_store_tests COMMAND sync_pairing_store_tests)

add_library(sync_cli
  native/src/cli.cpp
)
target_compile_features(sync_cli PUBLIC cxx_std_20)
target_include_directories(sync_cli PUBLIC native/src)
target_link_libraries(sync_cli PUBLIC sync_protocol)

add_executable(sync_cli_tests
  native/test/cli_test.cpp
  native/test/test_main.cpp
)
target_compile_features(sync_cli_tests PRIVATE cxx_std_20)
target_link_libraries(sync_cli_tests PRIVATE sync_cli)
add_test(NAME sync_cli_tests COMMAND sync_cli_tests)

add_executable(sync_pairing_test_server
  native/test/pairing_server_main.cpp
  native/src/server.cpp
)
target_compile_features(sync_pairing_test_server PRIVATE cxx_std_20)
target_compile_definitions(sync_pairing_test_server PRIVATE
  SYNC_PAIRING_PROMPT_DEADLINE_MS=525
)
target_link_libraries(sync_pairing_test_server PRIVATE sync_protocol ${SYNC_LIBUV_TARGET})

# The companion model is platform-neutral policy — supervision, recovery
# backoff, and health reporting — shared by the macOS menu-bar app and the
# Windows tray app, so it is built once for both.
add_library(sync_companion_model
  native/src/companion_model.cpp
)
target_compile_features(sync_companion_model PUBLIC cxx_std_20)
target_include_directories(sync_companion_model PUBLIC native/include)

add_executable(sync_companion_model_tests
  native/test/companion_model_test.cpp
  native/test/test_main.cpp
)
target_compile_features(sync_companion_model_tests PRIVATE cxx_std_20)
target_include_directories(sync_companion_model_tests PRIVATE native/test)
target_link_libraries(sync_companion_model_tests PRIVATE sync_companion_model)
add_test(NAME sync_companion_model_tests COMMAND sync_companion_model_tests)

# NDI is the one provider that is not platform-specific: its send path takes a
# CPU frame, so the same implementation serves every platform that can load the
# vendor's runtime.
add_library(sync_ndi_publisher
  native/src/platform/ndi_publisher.cpp
)
target_compile_features(sync_ndi_publisher PUBLIC cxx_std_20)
target_include_directories(sync_ndi_publisher PUBLIC native/include)
target_link_libraries(sync_ndi_publisher PUBLIC sync_protocol)
# Windows resolves the runtime through LoadLibraryExW; everywhere else needs
# the dynamic-loader library for dlopen.
if(NOT WIN32)
  target_link_libraries(sync_ndi_publisher PRIVATE ${CMAKE_DL_LIBS})
endif()

add_executable(sync_ndi_publisher_tests
  native/test/ndi_publisher_test.cpp
  native/test/test_main.cpp
)
target_compile_features(sync_ndi_publisher_tests PRIVATE cxx_std_20)
target_include_directories(sync_ndi_publisher_tests PRIVATE native/test)
target_link_libraries(sync_ndi_publisher_tests PRIVATE sync_ndi_publisher)
add_test(NAME sync_ndi_publisher_tests COMMAND sync_ndi_publisher_tests)

if(APPLE)
  enable_language(OBJCXX)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
  find_library(SERVICE_MANAGEMENT_FRAMEWORK ServiceManagement REQUIRED)
  find_library(CORE_FOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(METAL_FRAMEWORK Metal REQUIRED)

  add_library(sync_companion_process
    native/src/platform/macos/companion_process.mm
  )
  target_compile_features(sync_companion_process PUBLIC cxx_std_20)
  set_target_properties(sync_companion_process PROPERTIES OBJCXX_STANDARD 20)
  target_include_directories(sync_companion_process PUBLIC
    native/include native/src/platform/macos
  )
  target_compile_options(sync_companion_process PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(sync_companion_process
    PUBLIC sync_companion_model
    PRIVATE sync_protocol ${FOUNDATION_FRAMEWORK}
  )

  add_executable(sync_companion_process_tests
    native/test/macos/companion_process_test.mm
    native/test/test_main.cpp
  )
  target_compile_features(sync_companion_process_tests PRIVATE cxx_std_20)
  set_target_properties(sync_companion_process_tests PROPERTIES OBJCXX_STANDARD 20)
  target_compile_options(sync_companion_process_tests PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(sync_companion_process_tests PRIVATE
    sync_companion_process ${FOUNDATION_FRAMEWORK}
  )
  add_test(NAME sync_companion_process_tests COMMAND sync_companion_process_tests)

  configure_file(
    packaging/macos/Info.plist.in
    ${CMAKE_CURRENT_BINARY_DIR}/Sync-Info.plist
    @ONLY
  )
  add_executable(sync_menu MACOSX_BUNDLE
    native/src/platform/macos/app_main.mm
  )
  target_compile_features(sync_menu PRIVATE cxx_std_20)
  set_target_properties(sync_menu PROPERTIES
    OBJCXX_STANDARD 20
    OUTPUT_NAME Sync
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Sync-Info.plist
  )
  target_compile_options(sync_menu PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(sync_menu PRIVATE
    sync_companion_process ${APPKIT_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK} ${SERVICE_MANAGEMENT_FRAMEWORK}
  )

  set(SYNC_PACKAGE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/package)
  set(SYNC_BUNDLE_STAMP ${SYNC_PACKAGE_DIRECTORY}/.sync-bundle-complete)
  set(SYNC_DMG_PATH
      ${SYNC_PACKAGE_DIRECTORY}/Sync-${SYNC_PRODUCT_VERSION}-arm64.dmg)
  add_custom_command(
    OUTPUT ${SYNC_BUNDLE_STAMP}
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package-macos.sh bundle
      ${CMAKE_CURRENT_BINARY_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${SYNC_PRODUCT_VERSION}
      ${SYNC_SYPHON_FRAMEWORK_PATH}
      ${SYNC_DYLIB_SEARCH_PATH}
    DEPENDS
      sync_menu syncd
      LICENSE
      packaging/Sync.svg
      packaging/macos/Third-Party-Notices.txt
      scripts/package-macos.sh
      scripts/verify-macos-bundle.sh
    VERBATIM
  )
  add_custom_target(sync_macos_bundle DEPENDS ${SYNC_BUNDLE_STAMP})
  add_custom_command(
    OUTPUT ${SYNC_DMG_PATH}
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package-macos.sh dmg
      ${CMAKE_CURRENT_BINARY_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}
      ${SYNC_PRODUCT_VERSION}
    DEPENDS ${SYNC_BUNDLE_STAMP}
    VERBATIM
  )
  add_custom_target(sync_macos_dmg DEPENDS ${SYNC_DMG_PATH})

  add_library(sync_pairing_prompt
    native/src/platform/macos/pairing_prompt.mm
  )
  target_compile_features(sync_pairing_prompt PUBLIC cxx_std_20)
  set_target_properties(sync_pairing_prompt PROPERTIES OBJCXX_STANDARD 20)
  target_include_directories(sync_pairing_prompt PUBLIC native/include)
  target_compile_options(sync_pairing_prompt PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(sync_pairing_prompt
    PUBLIC sync_protocol
    PRIVATE ${FOUNDATION_FRAMEWORK} ${CORE_FOUNDATION_FRAMEWORK}
  )

  add_executable(sync_pairing_prompt_tests
    native/test/macos/pairing_prompt_test.mm
    native/test/test_main.cpp
  )
  target_compile_features(sync_pairing_prompt_tests PRIVATE cxx_std_20)
  set_target_properties(sync_pairing_prompt_tests PROPERTIES OBJCXX_STANDARD 20)
  target_compile_options(sync_pairing_prompt_tests PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(sync_pairing_prompt_tests
    PRIVATE sync_pairing_prompt ${FOUNDATION_FRAMEWORK}
  )
  add_test(NAME sync_pairing_prompt_tests COMMAND sync_pairing_prompt_tests)
  add_test(
    NAME sync_pairing_prompt_missing_pool_diagnostics
    COMMAND ${CMAKE_COMMAND}
      -DSYNC_PAIRING_PROMPT_TESTS=$<TARGET_FILE:sync_pairing_prompt_tests>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/native/test/macos/pairing_prompt_missing_pool_test.cmake
  )

  add_library(sync_metal_publisher
    native/src/platform/macos/metal_completion_tracker.cpp
    native/src/platform/macos/metal_frame_publisher.mm
  )
  target_compile_features(sync_metal_publisher PUBLIC cxx_std_20)
  set_target_properties(sync_metal_publisher PROPERTIES OBJCXX_STANDARD 20)
  target_include_directories(sync_metal_publisher PUBLIC native/include)
  target_compile_options(sync_metal_publisher PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(sync_metal_publisher
    PUBLIC sync_protocol
    PRIVATE ${FOUNDATION_FRAMEWORK} ${METAL_FRAMEWORK}
  )

  add_executable(sync_metal_publisher_tests
    native/test/macos/metal_completion_tracker_test.cpp
    native/test/macos/metal_frame_publisher_test.mm
    native/test/test_main.cpp
  )
  target_compile_features(sync_metal_publisher_tests PRIVATE cxx_std_20)
  set_target_properties(sync_metal_publisher_tests PROPERTIES OBJCXX_STANDARD 20)
  target_compile_definitions(sync_metal_publisher_tests PRIVATE
    SYNC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  )
  target_compile_options(sync_metal_publisher_tests PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(sync_metal_publisher_tests
    PRIVATE sync_metal_publisher ${FOUNDATION_FRAMEWORK} ${METAL_FRAMEWORK}
  )
  add_test(NAME sync_metal_publisher_tests COMMAND sync_metal_publisher_tests)

  add_executable(sync_metal_publisher_probe
    native/test/macos/metal_frame_publisher_probe.mm
  )
  target_compile_features(sync_metal_publisher_probe PRIVATE cxx_std_20)
  set_target_properties(sync_metal_publisher_probe PROPERTIES OBJCXX_STANDARD 20)
  target_compile_options(sync_metal_publisher_probe PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(sync_metal_publisher_probe
    PRIVATE sync_metal_publisher ${FOUNDATION_FRAMEWORK} ${METAL_FRAMEWORK}
  )
  add_test(
    NAME sync_metal_missing_pool_diagnostics
    COMMAND ${CMAKE_COMMAND}
      -DSYNC_METAL_PROBE=$<TARGET_FILE:sync_metal_publisher_probe>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/native/test/macos/metal_missing_pool_test.cmake
  )

  add_library(sync_syphon_consumer
    native/src/platform/macos/syphon_consumer.mm
  )
  target_compile_features(sync_syphon_consumer PUBLIC cxx_std_20)
  set_target_properties(sync_syphon_consumer PROPERTIES OBJCXX_STANDARD 20)
  target_include_directories(sync_syphon_consumer PUBLIC native/include)
  target_compile_options(sync_syphon_consumer PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(sync_syphon_consumer
    PRIVATE ${FOUNDATION_FRAMEWORK} ${METAL_FRAMEWORK}
  )

  add_executable(sync_syphon_consumer_tests
    native/test/macos/syphon_consumer_test.mm
    native/test/test_main.cpp
  )
  target_compile_features(sync_syphon_consumer_tests PRIVATE cxx_std_20)
  set_target_properties(sync_syphon_consumer_tests PROPERTIES OBJCXX_STANDARD 20)
  target_compile_options(sync_syphon_consumer_tests PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(sync_syphon_consumer_tests
    PRIVATE sync_syphon_consumer sync_metal_publisher
      ${FOUNDATION_FRAMEWORK} ${METAL_FRAMEWORK}
  )
  add_test(NAME sync_syphon_consumer_tests COMMAND sync_syphon_consumer_tests)

  add_executable(sync_syphon_discovery_probe
    native/test/macos/syphon_discovery_probe.mm
  )
  target_compile_features(sync_syphon_discovery_probe PRIVATE cxx_std_20)
  set_target_properties(sync_syphon_discovery_probe PROPERTIES OBJCXX_STANDARD 20)
  target_compile_options(sync_syphon_discovery_probe PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(sync_syphon_discovery_probe
    PRIVATE sync_syphon_consumer ${FOUNDATION_FRAMEWORK} ${METAL_FRAMEWORK}
  )
  add_test(NAME sync_syphon_discovery_probe COMMAND sync_syphon_discovery_probe)

  add_executable(sync_syphon_receiver_probe
    native/test/macos/syphon_receiver_probe.mm
  )
  target_compile_features(sync_syphon_receiver_probe PRIVATE cxx_std_20)
  set_target_properties(sync_syphon_receiver_probe PROPERTIES OBJCXX_STANDARD 20)
  target_compile_options(sync_syphon_receiver_probe PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(sync_syphon_receiver_probe
    PRIVATE ${FOUNDATION_FRAMEWORK} ${METAL_FRAMEWORK}
  )
  add_test(
    NAME sync_syphon_missing_pool_diagnostics
    COMMAND ${CMAKE_COMMAND}
      -DSYNC_SYPHON_PROBE=$<TARGET_FILE:sync_syphon_discovery_probe>
      -DSYNC_MISSING_FRAMEWORK_PATH=${CMAKE_CURRENT_BINARY_DIR}/missing/Syphon.framework
      -P ${CMAKE_CURRENT_SOURCE_DIR}/native/test/macos/syphon_missing_pool_test.cmake
  )

  set_source_files_properties(native/src/main.cpp PROPERTIES LANGUAGE OBJCXX)
  set_target_properties(syncd PROPERTIES OBJCXX_STANDARD 20)
  target_compile_options(syncd PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )
  target_link_libraries(syncd PRIVATE
    sync_pairing_prompt sync_metal_publisher sync_syphon_consumer
    ${FOUNDATION_FRAMEWORK} ${CORE_FOUNDATION_FRAMEWORK} ${METAL_FRAMEWORK}
  )
elseif(WIN32)
  add_library(sync_spout_publisher
    native/src/platform/windows/spout_publisher.cpp
  )
  target_compile_features(sync_spout_publisher PUBLIC cxx_std_20)
  target_include_directories(sync_spout_publisher PUBLIC native/include)
  target_link_libraries(sync_spout_publisher PUBLIC sync_protocol)

  add_executable(sync_spout_publisher_tests
    native/test/windows/spout_publisher_test.cpp
    native/test/test_main.cpp
  )
  target_compile_features(sync_spout_publisher_tests PRIVATE cxx_std_20)
  target_include_directories(sync_spout_publisher_tests PRIVATE native/test)
  target_link_libraries(sync_spout_publisher_tests PRIVATE sync_spout_publisher)
  add_test(NAME sync_spout_publisher_tests COMMAND sync_spout_publisher_tests)

  add_library(sync_pairing_prompt
    native/src/platform/windows/pairing_prompt.cpp
  )
  target_compile_features(sync_pairing_prompt PUBLIC cxx_std_20)
  target_include_directories(sync_pairing_prompt PUBLIC native/include)
  target_link_libraries(sync_pairing_prompt PUBLIC sync_protocol PRIVATE user32)

  add_executable(sync_pairing_prompt_tests
    native/test/windows/pairing_prompt_test.cpp
    native/test/test_main.cpp
  )
  target_compile_features(sync_pairing_prompt_tests PRIVATE cxx_std_20)
  target_include_directories(sync_pairing_prompt_tests PRIVATE native/test)
  target_link_libraries(sync_pairing_prompt_tests PRIVATE sync_pairing_prompt)
  add_test(NAME sync_pairing_prompt_tests COMMAND sync_pairing_prompt_tests)

  add_library(sync_companion_process
    native/src/platform/windows/companion_process.cpp
  )
  target_compile_features(sync_companion_process PUBLIC cxx_std_20)
  target_include_directories(sync_companion_process PUBLIC
    native/include native/src/platform/windows
  )
  # WinHTTP for the loopback health poll, ws2_32 for the raw-socket probe that
  # tells "nothing is listening" apart from "something else holds the port".
  # The sources also carry #pragma comment(lib, ...) for these; naming them
  # here as well keeps the link correct under non-MSVC toolchains, which
  # ignore that pragma.
  target_link_libraries(sync_companion_process
    PUBLIC sync_companion_model
    PRIVATE sync_protocol winhttp ws2_32 user32
  )

  # A controllable stand-in for syncd.exe, so the lifecycle tests can drive
  # behaviour the real helper cannot be asked for on demand -- ignoring a
  # console control event, or exiting instantly with a chosen status --
  # without binding TCP 53979 or touching the real pairing store. It is a
  # test fixture and is never installed or packaged.
  add_executable(sync_test_helper_process
    native/test/windows/test_helper_process.cpp
  )
  target_compile_features(sync_test_helper_process PRIVATE cxx_std_20)

  add_executable(sync_companion_process_tests
    native/test/windows/companion_process_test.cpp
    native/test/windows/companion_process_lifecycle_test.cpp
    native/test/test_main.cpp
  )
  target_compile_features(sync_companion_process_tests PRIVATE cxx_std_20)
  target_include_directories(sync_companion_process_tests PRIVATE native/test)
  # ws2_32 is linked PRIVATE into sync_companion_process, so it does not
  # propagate; the lifecycle tests open their own loopback listener.
  target_link_libraries(sync_companion_process_tests PRIVATE sync_companion_process ws2_32)
  # The lifecycle tests launch it by path from their own directory, so it
  # has to exist before they run -- ctest does not build dependencies.
  add_dependencies(sync_companion_process_tests sync_test_helper_process)
  add_test(NAME sync_companion_process_tests COMMAND sync_companion_process_tests)

  # The executable's icon is rasterised from the same artwork the macOS bundle
  # uses, so the two platforms cannot drift apart visually. ImageMagick is an
  # optional build dependency: without it the resource is simply omitted and
  # the app falls back to a stock icon, so a plain developer build still works.
  # Packaging requires it, and scripts/package-windows.ps1 fails fast if it is
  # missing there.
  find_program(SYNC_MAGICK NAMES magick)
  set(SYNC_ICON_PATH ${CMAKE_CURRENT_BINARY_DIR}/Sync.ico)
  if(SYNC_MAGICK)
    add_custom_command(
      OUTPUT ${SYNC_ICON_PATH}
      COMMAND ${SYNC_MAGICK} -background none
        ${CMAKE_CURRENT_SOURCE_DIR}/packaging/Sync.svg
        -define icon:auto-resize=256,128,64,48,32,24,16
        ${SYNC_ICON_PATH}
      DEPENDS packaging/Sync.svg
      COMMENT "Rasterising Sync.ico from packaging/Sync.svg"
      VERBATIM
    )
    add_custom_target(sync_windows_icon DEPENDS ${SYNC_ICON_PATH})
    # The RC compiler resolves the icon path relative to the .rc file, which
    # configure_file writes into the binary directory alongside the icon.
    set(SYNC_ICON_RESOURCE_LINE "IDI_SYNC_APP ICON \"Sync.ico\"")
  else()
    message(STATUS
      "ImageMagick (magick) not found: Sync.exe will be built without an "
      "embedded icon. Install it before packaging.")
    set(SYNC_ICON_RESOURCE_LINE "")
  endif()

  configure_file(
    packaging/windows/version.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/sync-version.rc
    @ONLY
  )

  # WIN32 rather than a console subsystem: the tray app must not flash a
  # console window when a user launches it from Explorer or at sign-in.
  add_executable(sync_menu WIN32
    native/src/platform/windows/app_main.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/sync-version.rc
  )
  target_compile_features(sync_menu PRIVATE cxx_std_20)
  set_target_properties(sync_menu PROPERTIES OUTPUT_NAME Sync)
  target_include_directories(sync_menu PRIVATE native/src/platform/windows)
  target_link_libraries(sync_menu PRIVATE
    sync_companion_process shell32 user32 advapi32
  )

  target_sources(syncd PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/sync-version.rc)
  target_link_libraries(syncd PRIVATE
    sync_pairing_prompt sync_spout_publisher user32
  )

  # Both executables embed the version resource, so both need the icon to
  # exist before their RC compile runs.
  if(SYNC_MAGICK)
    add_dependencies(sync_menu sync_windows_icon)
    add_dependencies(syncd sync_windows_icon)
  endif()
  # The RC compiler needs resource.h, which lives with the Windows sources.
  set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/sync-version.rc
    PROPERTIES INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/native/src/platform/windows)

  set(SYNC_PACKAGE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/package)
  set(SYNC_BUNDLE_STAMP ${SYNC_PACKAGE_DIRECTORY}/.sync-bundle-complete)
  set(SYNC_INSTALLER_PATH
      ${SYNC_PACKAGE_DIRECTORY}/Sync-${SYNC_PRODUCT_VERSION}-x64-Setup.exe)
  find_program(SYNC_POWERSHELL NAMES pwsh powershell REQUIRED)
  add_custom_command(
    OUTPUT ${SYNC_BUNDLE_STAMP}
    COMMAND ${SYNC_POWERSHELL} -NoProfile -ExecutionPolicy Bypass -File
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package-windows.ps1
      -Mode bundle
      -BuildDir ${CMAKE_CURRENT_BINARY_DIR}
      -SourceDir ${CMAKE_CURRENT_SOURCE_DIR}
      -Version ${SYNC_PRODUCT_VERSION}
      -SpoutLibrary ${SYNC_SPOUT_LIBRARY_PATH}
      -DependencySearchPath ${SYNC_WINDOWS_DEPENDENCY_PATH}
      -BinaryDir $<TARGET_FILE_DIR:syncd>
    DEPENDS
      sync_menu syncd
      LICENSE
      packaging/Sync.svg
      packaging/windows/Third-Party-Notices.txt
      packaging/windows/bundle-dependencies.cmake
      scripts/package-windows.ps1
      scripts/verify-windows-bundle.ps1
    VERBATIM
  )
  add_custom_target(sync_windows_bundle DEPENDS ${SYNC_BUNDLE_STAMP})
  add_custom_command(
    OUTPUT ${SYNC_INSTALLER_PATH}
    COMMAND ${SYNC_POWERSHELL} -NoProfile -ExecutionPolicy Bypass -File
      ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package-windows.ps1
      -Mode installer
      -BuildDir ${CMAKE_CURRENT_BINARY_DIR}
      -SourceDir ${CMAKE_CURRENT_SOURCE_DIR}
      -Version ${SYNC_PRODUCT_VERSION}
    DEPENDS ${SYNC_BUNDLE_STAMP} packaging/windows/Sync.iss
    VERBATIM
  )
  add_custom_target(sync_windows_installer DEPENDS ${SYNC_INSTALLER_PATH})
else()
  target_link_libraries(syncd PRIVATE sync_protocol)
endif()

# Every test gets a deadline, applied here rather than left to whoever invokes
# ctest. A hung test with no timeout consumes the whole CI job budget and then
# reports only that the job timed out -- no test name, and no log, because the
# logs of a cancelled job are never published. That is the worst possible
# failure mode: the one case where the signal is most needed is the one where
# there is none. With a deadline the run reports "test X timed out" and keeps
# going. The bound is deliberately loose; the slowest test here runs in about
# fifteen seconds, so this catches hangs without ever failing slow-but-working
# tests on a loaded runner.
get_property(SYNC_ALL_TESTS DIRECTORY PROPERTY TESTS)
if(SYNC_ALL_TESTS)
  set_tests_properties(${SYNC_ALL_TESTS} PROPERTIES TIMEOUT 300)
endif()
