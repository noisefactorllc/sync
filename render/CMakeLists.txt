# sync-render: the render helper. It joins a seance session read-only,
# compiles the program with noisemaker-for-qt, renders it headless on the GPU,
# and writes frames into the render ring (native/include/sync/render) that
# syncd publishes. Built only with SYNC_BUILD_RENDER, because it needs Qt 6
# and a desktop OpenGL 4.1 driver, neither of which the rest of Sync does.

# Qt 6.9 is the floor. Media inputs need QPermissions (Qt 6.5), and
# noisemaker-for-qt supports Qt 6.9 and later: before 6.9 Qt cannot set a
# variable font's weight axis, so filter/text would draw every weight in the
# bundled Nunito's ExtraLight default. Ubuntu 24.04's Qt 6.4.2 does not build
# this helper (CI run 36023181792), so a Linux package cannot depend on that
# distribution's Qt.
find_package(Qt6 6.9 REQUIRED COMPONENTS Core Gui Multimedia OpenGL Network WebSockets)

# Pinned by commit, never a branch, so a Sync build is reproducible. For local
# work against a checkout, configure with
#   -DFETCHCONTENT_SOURCE_DIR_NOISEMAKER_QT=/path/to/noisemaker-for-qt
FetchContent_Declare(noisemaker_qt
  GIT_REPOSITORY https://github.com/noisefactorllc/noisemaker-for-qt.git
  GIT_TAG 7675d7931ccf38a4c24768c882f829ea3f8f2341
  SOURCE_SUBDIR qt
)
FetchContent_MakeAvailable(noisemaker_qt)

# MIDI input for midi() steps: RtMidi, the sibling of the RtAudio syncd pins,
# pinned the same way by commit tarball and hash (tag 6.0.0).
set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
# RtAudio already defines "uninstall"; RtMidi names its own through a cache
# variable, which is all that stands between the two.
set(RTMIDI_TARGETNAME_UNINSTALL "rtmidi-uninstall" CACHE STRING "" FORCE)
FetchContent_Declare(rtmidi
  URL https://codeload.github.com/thestk/rtmidi/tar.gz/1e5b49925aa60065db52de44c366d446a902547b
  URL_HASH SHA256=c5090c14edb3e3f82dd63589041f516bb24b78091aa82574fdc17d33ee62361e
)
FetchContent_MakeAvailable(rtmidi)

# Third-party notices for the packaged helper, written by
# scripts/render-notices.mjs: the helper's own dependencies and every
# component in Qt's software bill of materials for the modules that ship.
# Cached so the bundle targets in the top-level CMakeLists.txt see them.
# Newer noisemaker-for-qt also carries acorn's notice (its JavaScript syntax
# check is ported from acorn) and the bundled Nunito font's SIL OFL.
set(sync_render_notice_components
  "noisemaker-for-qt=https://github.com/noisefactorllc/noisemaker-for-qt=${noisemaker_qt_SOURCE_DIR}/LICENSE"
  "RtMidi=https://github.com/thestk/rtmidi=${rtmidi_SOURCE_DIR}/LICENSE")
if(EXISTS "${noisemaker_qt_SOURCE_DIR}/qt/noisemaker/compiler/acorn-LICENSE.txt")
  list(APPEND sync_render_notice_components
    "acorn (JavaScript syntax check in noisemaker-for-qt)=https://github.com/acornjs/acorn=${noisemaker_qt_SOURCE_DIR}/qt/noisemaker/compiler/acorn-LICENSE.txt")
endif()
if(EXISTS "${noisemaker_qt_SOURCE_DIR}/qt/noisemaker/fonts/Nunito/OFL.txt")
  list(APPEND sync_render_notice_components
    "Nunito font (bundled with noisemaker-for-qt)=https://github.com/googlefonts/nunito=${noisemaker_qt_SOURCE_DIR}/qt/noisemaker/fonts/Nunito/OFL.txt")
endif()
list(JOIN sync_render_notice_components "|" sync_render_notice_components)
set(SYNC_RENDER_NOTICE_COMPONENTS "${sync_render_notice_components}"
  CACHE INTERNAL "License files of the render helper's own dependencies")
set(SYNC_RENDER_QT_SBOM_DIR "${QT6_INSTALL_PREFIX}/sbom"
  CACHE INTERNAL "Qt's SPDX software bill of materials")
set(SYNC_RENDER_QT_MODULES "qtbase,qtmultimedia,qtwebsockets,qtsvg"
  CACHE INTERNAL "Qt modules the packaged helper ships")
find_program(SYNC_NODE node)
# Keep the warnings-as-errors gate off pinned upstream code, as for RtAudio.
if(MSVC)
  target_compile_options(rtmidi PRIVATE /WX-)
else()
  target_compile_options(rtmidi PRIVATE -Wno-error)
endif()

add_library(sync_render_helper STATIC
  src/audio_capture.cpp
  src/event_log.cpp
  src/media_inputs.cpp
  src/midi_input.cpp
  src/program_compiler.cpp
  src/render_engine.cpp
  src/seance_client.cpp
  src/seance_document.cpp
)
set_target_properties(sync_render_helper PROPERTIES AUTOMOC ON)
target_compile_features(sync_render_helper PUBLIC cxx_std_20)
target_include_directories(sync_render_helper PUBLIC src)
target_link_libraries(sync_render_helper PUBLIC
  noisemaker-qt::noisemaker-qt
  Qt6::Gui
  Qt6::Multimedia
  Qt6::Network
  Qt6::WebSockets
  rtmidi
  sync_audio_native
  sync_render_ring
)

add_executable(sync-render src/main.cpp)
target_link_libraries(sync-render PRIVATE sync_render_helper)
if(APPLE)
  # macOS shows a camera or microphone permission request only for an
  # executable that states why it wants access. A bare executable has no
  # bundle to carry an Info.plist, so the plist is linked into the binary.
  target_link_options(sync-render PRIVATE
    "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist")
  set_property(TARGET sync-render APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist")
  # The app bundle carries the Qt this helper links; macdeployqt copies it in
  # (scripts/package-macos.sh). find_program caches the path, so the bundle
  # target in the top-level CMakeLists.txt sees it outside this directory.
  find_program(SYNC_MACDEPLOYQT macdeployqt
    HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" REQUIRED)
endif()
if(WIN32)
  # The installer carries the Qt this helper links; windeployqt copies it in
  # (scripts/package-windows.ps1). Cached, like SYNC_MACDEPLOYQT above.
  find_program(SYNC_WINDEPLOYQT windeployqt
    HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" REQUIRED)
endif()

# The helper loads its runtime data from disk: effect definitions and
# shaders, and in newer noisemaker-for-qt the bundled font (fonts/) and the
# built-in meshes (share/). The build places every data directory the pinned
# port provides next to the binary, where the helper looks first after its
# flag and environment variable, so a built tree runs as it is. The packagers
# copy this whole tree.
set(SYNC_RENDER_DATA_COPY)
foreach(data_directory IN ITEMS effects shaders fonts share)
  if(IS_DIRECTORY "${NOISEMAKER_QT_DATA_ROOT}/${data_directory}")
    list(APPEND SYNC_RENDER_DATA_COPY COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${NOISEMAKER_QT_DATA_ROOT}/${data_directory}"
      "$<TARGET_FILE_DIR:sync-render>/noisemaker/${data_directory}")
  endif()
endforeach()
add_custom_command(TARGET sync-render POST_BUILD ${SYNC_RENDER_DATA_COPY} VERBATIM)

add_executable(sync_render_helper_tests
  test/audio_capture_test.cpp
  test/media_inputs_test.cpp
  test/midi_input_test.cpp
  test/qt_test_main.cpp
  test/render_engine_test.cpp
  test/seance_client_test.cpp
  test/seance_document_test.cpp
)
target_include_directories(sync_render_helper_tests PRIVATE ${PROJECT_SOURCE_DIR}/native/test)
target_compile_definitions(sync_render_helper_tests PRIVATE
  SYNC_RENDER_TEST_DATA_ROOT="${NOISEMAKER_QT_DATA_ROOT}"
)
target_link_libraries(sync_render_helper_tests PRIVATE sync_render_helper)
add_test(NAME sync_render_helper_tests COMMAND sync_render_helper_tests)
