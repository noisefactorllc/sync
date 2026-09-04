#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <sync/frame_receiver.hpp>
#include <sync/platform/camera_identity.hpp>
#include <sync/platform/camera_publisher.hpp>
#include <sync/platform/camera_sink.hpp>

namespace {

using noisefactor::sync::PublishResult;
using noisefactor::sync::camera::CameraFramePublisher;
using noisefactor::sync::camera::CameraSink;
using noisefactor::sync::camera::CameraSinkFrame;
using noisefactor::sync::camera::CameraSinkSubmit;
using noisefactor::sync::camera::CameraSinkUnavailableReason;
using noisefactor::sync::camera::describe;
using noisefactor::sync::camera::kCanvas;
using noisefactor::sync::protocol::FrameView;

struct FakeSink final : CameraSink {
  bool is_available = true;
  CameraSinkUnavailableReason reason = CameraSinkUnavailableReason::None;
  std::int32_t status = 0;
  CameraSinkSubmit next_result = CameraSinkSubmit::Accepted;
  std::size_t submitted = 0;
  std::uint32_t last_width = 0;
  std::uint32_t last_height = 0;
  std::uint64_t last_presentation_time_us = 0;
  std::array<std::uint8_t, 4> last_first_pixel{};
  std::array<std::uint8_t, 4> last_center_pixel{};

  auto available() const noexcept -> bool override { return is_available; }
  auto unavailable_reason() const noexcept -> CameraSinkUnavailableReason override {
    return reason;
  }
  auto unavailable_status() const noexcept -> std::int32_t override { return status; }
  auto submit(const CameraSinkFrame& frame) noexcept -> CameraSinkSubmit override {
    ++submitted;
    last_width = frame.width;
    last_height = frame.height;
    last_presentation_time_us = frame.presentation_time_us;
    for (std::size_t i = 0; i < 4; ++i) {
      last_first_pixel[i] = static_cast<std::uint8_t>(frame.bgra[i]);
    }
    const std::size_t center = static_cast<std::size_t>(frame.height / 2) * frame.row_stride +
                               static_cast<std::size_t>(frame.width / 2) * 4U;
    for (std::size_t i = 0; i < 4; ++i) {
      last_center_pixel[i] = static_cast<std::uint8_t>(frame.bgra[center + i]);
    }
    return next_result;
  }
};

[[nodiscard]] auto make_frame(std::span<const std::byte> payload,
                              std::uint64_t sequence = 1) noexcept -> FrameView {
  return {
      .version = 1,
      .header_bytes = 64,
      .flags = 1,
      .pixel_format = 1,
      .color_space = 1,
      .alpha_mode = 1,
      .width = 2,
      .height = 2,
      .row_stride = 8,
      .payload_bytes = 16,
      .sequence = sequence,
      .presentation_time_us = 5'000,
      .top_down = true,
      .payload = payload,
  };
}

const std::array<std::byte, 16> kRedPayload{
    std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
    std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
    std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
    std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};

}  // namespace

SYNC_TEST(camera_publisher_reports_sink_availability_and_reason) {
  FakeSink sink;
  sink.is_available = false;
  sink.reason = CameraSinkUnavailableReason::DeviceNotFound;
  CameraFramePublisher publisher(sink);
  SYNC_REQUIRE(!publisher.available());
  SYNC_REQUIRE(publisher.unavailable_reason() == CameraSinkUnavailableReason::DeviceNotFound);
#if defined(__linux__)
  SYNC_REQUIRE(std::string(describe(publisher.unavailable_reason())).find("device_not_found") !=
               std::string::npos);
#else
  SYNC_REQUIRE(std::string(describe(publisher.unavailable_reason())).find("System Settings") !=
               std::string::npos);
#endif
}

SYNC_TEST(camera_publisher_never_declines_a_sender_and_the_oldest_drives) {
  FakeSink sink;
  CameraFramePublisher publisher(sink);
  SYNC_REQUIRE(publisher.open_sender("a", "first"));
  SYNC_REQUIRE(publisher.open_sender("b", "second"));
  SYNC_REQUIRE(publisher.driving_sender() == "a");
  SYNC_REQUIRE(publisher.publish("b", make_frame(kRedPayload)) == PublishResult::Accepted);
  SYNC_REQUIRE(sink.submitted == 0);
  SYNC_REQUIRE(publisher.publish("a", make_frame(kRedPayload)) == PublishResult::Accepted);
  SYNC_REQUIRE(sink.submitted == 1);
  publisher.close_sender("a");
  SYNC_REQUIRE(publisher.driving_sender() == "b");
  SYNC_REQUIRE(publisher.publish("b", make_frame(kRedPayload)) == PublishResult::Accepted);
  SYNC_REQUIRE(sink.submitted == 2);
  publisher.close_sender("b");
  SYNC_REQUIRE(publisher.driving_sender().empty());
}

SYNC_TEST(camera_publisher_fits_frames_to_the_canvas_before_submitting) {
  FakeSink sink;
  CameraFramePublisher publisher(sink);
  SYNC_REQUIRE(publisher.open_sender("a", "first"));
  SYNC_REQUIRE(publisher.publish("a", make_frame(kRedPayload)) == PublishResult::Accepted);
  SYNC_REQUIRE(sink.last_width == kCanvas.width);
  SYNC_REQUIRE(sink.last_height == kCanvas.height);
  SYNC_REQUIRE(sink.last_presentation_time_us == 5'000);
  // A square red frame into a 16:9 canvas: the first canvas pixel sits in
  // the left pillarbox, the center pixel is the scaled red as BGRA.
  SYNC_REQUIRE((sink.last_first_pixel == std::array<std::uint8_t, 4>{0, 0, 0, 255}));
  SYNC_REQUIRE((sink.last_center_pixel == std::array<std::uint8_t, 4>{0, 0, 255, 255}));
}

SYNC_TEST(camera_publisher_maps_sink_results_and_rejects_bad_input) {
  FakeSink sink;
  CameraFramePublisher publisher(sink);
  SYNC_REQUIRE(publisher.open_sender("a", "first"));
  sink.next_result = CameraSinkSubmit::Backpressured;
  SYNC_REQUIRE(publisher.publish("a", make_frame(kRedPayload)) == PublishResult::Backpressured);
  sink.next_result = CameraSinkSubmit::Failed;
  SYNC_REQUIRE(publisher.publish("a", make_frame(kRedPayload)) == PublishResult::Failed);
  SYNC_REQUIRE(!publisher.poll_failure(0).has_value());

  FrameView bad = make_frame(kRedPayload);
  bad.pixel_format = 9;
  sink.next_result = CameraSinkSubmit::Accepted;
  SYNC_REQUIRE(publisher.publish("a", bad) == PublishResult::Failed);
  SYNC_REQUIRE(publisher.publish("unknown", make_frame(kRedPayload)) == PublishResult::Failed);
  SYNC_REQUIRE(!publisher.open_sender("", "x"));
  SYNC_REQUIRE(!publisher.open_sender("a", "x"));
  SYNC_REQUIRE(!publisher.open_sender("a", "again"));
  SYNC_REQUIRE(!publisher.open_sender(std::string(200, 'i'), "x"));
}

SYNC_TEST(camera_publisher_publishes_nothing_while_unavailable) {
  FakeSink sink;
  sink.is_available = false;
  sink.reason = CameraSinkUnavailableReason::StreamNotStarted;
  sink.status = -67;
  CameraFramePublisher publisher(sink);
  SYNC_REQUIRE(publisher.unavailable_status() == -67);
  SYNC_REQUIRE(publisher.open_sender("a", "first"));
  SYNC_REQUIRE(publisher.publish("a", make_frame(kRedPayload)) == PublishResult::Failed);
  SYNC_REQUIRE(sink.submitted == 0);
}

// Each reason names a different thing to do, and a CoreMediaIO refusal carries
// the status macOS returned, since a bare phrase leaves a bug report empty.
SYNC_TEST(camera_unavailability_phrases_are_distinct_and_carry_the_status) {
  using noisefactor::sync::camera::describe_unavailability;
  const CameraSinkUnavailableReason all[] = {
      CameraSinkUnavailableReason::DeviceNotFound,
      CameraSinkUnavailableReason::SinkStreamMissing,
      CameraSinkUnavailableReason::QueueNotProvided,
      CameraSinkUnavailableReason::StreamNotStarted,
  };
  for (std::size_t outer = 0; outer < 4; ++outer) {
    SYNC_REQUIRE(std::string(describe(all[outer])).size() > 0);
    for (std::size_t inner = outer + 1; inner < 4; ++inner) {
      SYNC_REQUIRE(std::string(describe(all[outer])) != std::string(describe(all[inner])));
    }
  }
  SYNC_REQUIRE(describe_unavailability(CameraSinkUnavailableReason::DeviceNotFound, 0) ==
               std::string(describe(CameraSinkUnavailableReason::DeviceNotFound)));
  // The status is an OSStatus on macOS and an HRESULT on Windows. They are
  // read differently -- a small signed integer against a hex word -- and
  // labelling a Windows HRESULT "OSStatus" would send a bug report looking up
  // the wrong number, so the wording follows the platform.
#if defined(_WIN32)
  SYNC_REQUIRE(describe_unavailability(CameraSinkUnavailableReason::VirtualCameraRefused,
                                       static_cast<std::int32_t>(0x80070005)) ==
               std::string(describe(CameraSinkUnavailableReason::VirtualCameraRefused)) +
                   " (HRESULT 0x80070005)");
  SYNC_REQUIRE(describe_unavailability(CameraSinkUnavailableReason::SectionAccessDenied, -67) ==
               std::string(describe(CameraSinkUnavailableReason::SectionAccessDenied)) +
                   " (HRESULT 0xFFFFFFBD)");
#elif defined(__linux__)
  SYNC_REQUIRE(describe_unavailability(CameraSinkUnavailableReason::DevicePermissionDenied,
                                       13) ==
               std::string(describe(CameraSinkUnavailableReason::DevicePermissionDenied)) +
                   " (errno 13)");
#else
  SYNC_REQUIRE(describe_unavailability(CameraSinkUnavailableReason::StreamNotStarted, -67) ==
               std::string(describe(CameraSinkUnavailableReason::StreamNotStarted)) +
                   " (OSStatus -67)");
  SYNC_REQUIRE(describe_unavailability(CameraSinkUnavailableReason::QueueNotProvided, 1852797029) ==
               std::string(describe(CameraSinkUnavailableReason::QueueNotProvided)) +
                   " (OSStatus 1852797029)");
#endif
}

SYNC_TEST(camera_publisher_is_bounded_in_senders) {
  FakeSink sink;
  CameraFramePublisher publisher(sink);
  for (std::size_t i = 0; i < CameraFramePublisher::kMaximumSenderEntries; ++i) {
    SYNC_REQUIRE(publisher.open_sender("s" + std::to_string(i), "n"));
  }
  SYNC_REQUIRE(!publisher.open_sender("overflow", "n"));
  publisher.close_sender("s3");
  SYNC_REQUIRE(publisher.open_sender("overflow", "n"));
  SYNC_REQUIRE(publisher.driving_sender() == "s0");
}

// Fitting a frame is the most expensive thing the camera provider does. When
// the sink already holds its full queue, the provider must report
// backpressure before converting, not after.
SYNC_TEST(camera_publisher_skips_fitting_when_the_sink_has_no_capacity) {
  struct FullSink final : CameraSink {
    std::size_t submitted = 0;
    std::size_t capacity_polls = 0;
    bool capacity = false;
    auto available() const noexcept -> bool override { return true; }
    auto unavailable_reason() const noexcept -> CameraSinkUnavailableReason override {
      return CameraSinkUnavailableReason::None;
    }
    auto has_capacity() const noexcept -> bool override {
      ++const_cast<FullSink*>(this)->capacity_polls;
      return capacity;
    }
    auto submit(const CameraSinkFrame&) noexcept -> CameraSinkSubmit override {
      ++submitted;
      return CameraSinkSubmit::Accepted;
    }
  };
  FullSink sink;
  CameraFramePublisher publisher(sink);
  SYNC_REQUIRE(publisher.open_sender("a", "first"));
  SYNC_REQUIRE(publisher.publish("a", make_frame(kRedPayload)) == PublishResult::Backpressured);
  SYNC_REQUIRE(sink.submitted == 0);
  SYNC_REQUIRE(sink.capacity_polls == 1);
  sink.capacity = true;
  SYNC_REQUIRE(publisher.publish("a", make_frame(kRedPayload, 2)) == PublishResult::Accepted);
  SYNC_REQUIRE(sink.submitted == 1);
}

// The scratch the fitter uses lives with the provider, so a long run of frames
// does not allocate per frame. Observable through the sink: many frames, one
// conversion buffer, the same result every time.
SYNC_TEST(camera_publisher_fits_many_frames_with_stable_output) {
  FakeSink sink;
  CameraFramePublisher publisher(sink);
  SYNC_REQUIRE(publisher.open_sender("a", "first"));
  for (std::uint64_t sequence = 1; sequence <= 32; ++sequence) {
    SYNC_REQUIRE(publisher.publish("a", make_frame(kRedPayload, sequence)) ==
                 PublishResult::Accepted);
    SYNC_REQUIRE((sink.last_first_pixel == std::array<std::uint8_t, 4>{0, 0, 0, 255}));
    SYNC_REQUIRE((sink.last_center_pixel == std::array<std::uint8_t, 4>{0, 0, 255, 255}));
  }
  SYNC_REQUIRE(sink.submitted == 32);
}
