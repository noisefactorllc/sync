#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <vector>

#include <sync/platform/camera_identity.hpp>
#include <sync/platform/cmio_camera_sink.hpp>

// Machines running this suite, CI included, have no Sync Camera extension
// installed, so the only branch reachable here is "device not found". These
// tests pin that branch: discovery must fail quietly, the reason must be the
// one that tells a user to approve the extension, and submit() must refuse
// frames rather than touch a queue that was never obtained.

namespace {

using noisefactor::sync::camera::CameraSinkFrame;
using noisefactor::sync::camera::CameraSinkSubmit;
using noisefactor::sync::camera::CameraSinkUnavailableReason;
using noisefactor::sync::camera::CmioCameraSink;
using noisefactor::sync::camera::kCanvas;

}  // namespace

SYNC_TEST(cmio_camera_sink_without_the_extension_is_unavailable_and_refuses_frames) {
  CmioCameraSink sink({.device_uid = "io.noisefactor.sync.camera.does-not-exist"});
  SYNC_REQUIRE(!sink.available());
  SYNC_REQUIRE(sink.unavailable_reason() == CameraSinkUnavailableReason::DeviceNotFound);
  std::vector<std::byte> bytes(static_cast<std::size_t>(kCanvas.width) * kCanvas.height * 4,
                               std::byte{0});
  const CameraSinkFrame frame{
      .width = kCanvas.width,
      .height = kCanvas.height,
      .row_stride = static_cast<std::size_t>(kCanvas.width) * 4,
      .bgra = bytes,
      .presentation_time_us = 1,
  };
  SYNC_REQUIRE(sink.submit(frame) == CameraSinkSubmit::Failed);
}

SYNC_TEST(cmio_camera_sink_rejects_frames_that_are_not_the_advertised_canvas) {
  CmioCameraSink sink;
  std::array<std::byte, 16> bytes{};
  const CameraSinkFrame frame{
      .width = 2, .height = 2, .row_stride = 8, .bgra = bytes, .presentation_time_us = 1};
  SYNC_REQUIRE(sink.submit(frame) == CameraSinkSubmit::Failed);
}
