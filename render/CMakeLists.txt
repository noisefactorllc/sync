# sync-render: the render helper. It joins a seance session read-only,
# compiles the program with noisemaker-for-qt, renders it headless on the GPU,
# and writes frames into the render ring (native/include/sync/render) that
# syncd publishes. Built only with SYNC_BUILD_RENDER, because it needs Qt 6
# and a desktop OpenGL 4.1 driver, neither of which the rest of Sync does.

find_package(Qt6 REQUIRED COMPONENTS Core Gui Multimedia OpenGL Network WebSockets)

# Pinned by commit, never a branch, so a Sync build is reproducible. For local
# work against a checkout, configure with
#   -DFETCHCONTENT_SOURCE_DIR_NOISEMAKER_QT=/path/to/noisemaker-for-qt
FetchContent_Declare(noisemaker_qt
  GIT_REPOSITORY https://github.com/noisefactorllc/noisemaker-for-qt.git
  GIT_TAG 12c1d1c6f160e5fe30f482e6376f220b3ec982c4
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
endif()

# The helper loads effect definitions and shaders from disk at run time. The
# build places them next to the binary, where the helper looks first after
# its flag and environment variable, so a built tree runs as it is.
add_custom_command(TARGET sync-render POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${NOISEMAKER_QT_DATA_ROOT}/effects" "$<TARGET_FILE_DIR:sync-render>/noisemaker/effects"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${NOISEMAKER_QT_DATA_ROOT}/shaders" "$<TARGET_FILE_DIR:sync-render>/noisemaker/shaders"
  VERBATIM
)

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
