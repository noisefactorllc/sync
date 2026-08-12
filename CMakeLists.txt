cmake_minimum_required(VERSION 3.20)

project(noisefactor_sync_protocol VERSION 0.2.0 LANGUAGES CXX)

set(SYNC_PRODUCT_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Sync product version reported by the native companion")
set(SYNC_SYPHON_FRAMEWORK_PATH "" CACHE PATH
    "Path to the Syphon.framework bundled by the macOS packaging target")
set(SYNC_DYLIB_SEARCH_PATH "" CACHE PATH
    "Additional directory searched for macOS bundle dependencies")

find_package(Threads REQUIRED)

find_package(OpenSSL 3 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)

add_library(sync_protocol
  native/src/control.cpp
  native/src/frame_receiver.cpp
  native/src/origin.cpp
  native/src/pairing.cpp
  native/src/authority_worker.cpp
  native/src/pairing_store.cpp
  native/src/publisher_hub.cpp
  native/src/protocol.cpp
  native/src/secure_memory.cpp
  native/src/websocket.cpp
)
target_compile_features(sync_protocol PUBLIC cxx_std_20)
target_include_directories(sync_protocol PUBLIC native/include)
target_compile_definitions(sync_protocol PUBLIC
  SYNC_PRODUCT_VERSION="${SYNC_PRODUCT_VERSION}"
)
target_link_libraries(sync_protocol PUBLIC OpenSSL::Crypto Threads::Threads)

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
target_link_libraries(sync_protocol_tests PRIVATE sync_protocol PkgConfig::LIBUV)
target_compile_definitions(sync_protocol_tests PRIVATE SYNC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(syncd
  native/src/main.cpp
  native/src/server.cpp
)
target_compile_features(syncd PRIVATE cxx_std_20)
target_link_libraries(syncd PRIVATE sync_cli PkgConfig::LIBUV)

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
target_link_libraries(sync_pairing_test_server PRIVATE sync_protocol PkgConfig::LIBUV)

if(APPLE)
  enable_language(OBJCXX)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
  find_library(SERVICE_MANAGEMENT_FRAMEWORK ServiceManagement REQUIRED)
  find_library(CORE_FOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  find_library(METAL_FRAMEWORK Metal REQUIRED)

  add_library(sync_companion_model
    native/src/platform/macos/companion_model.cpp
  )
  target_compile_features(sync_companion_model PUBLIC cxx_std_20)
  target_include_directories(sync_companion_model PUBLIC native/include)

  add_executable(sync_companion_model_tests
    native/test/macos/companion_model_test.cpp
    native/test/test_main.cpp
  )
  target_compile_features(sync_companion_model_tests PRIVATE cxx_std_20)
  target_link_libraries(sync_companion_model_tests PRIVATE sync_companion_model)
  add_test(NAME sync_companion_model_tests COMMAND sync_companion_model_tests)

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
      packaging/macos/Sync.svg
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
else()
  target_link_libraries(syncd PRIVATE sync_protocol)
endif()
