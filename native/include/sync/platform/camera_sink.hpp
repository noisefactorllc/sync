#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace noisefactor::sync::camera {

// Why the camera could not be reached. The daemon prints the matching phrase
// on stderr when the provider is selected but unavailable, so each one names
// a different thing for the user to do.
enum class CameraSinkUnavailableReason : std::uint8_t {
  None = 0,
  DeviceNotFound,
  SinkStreamMissing,
  // The extension is installed and its sink stream was found, but
  // CMIOStreamCopyBufferQueue did not hand over a queue.
  QueueNotProvided,
  // The queue was obtained, but CMIODeviceStartStream refused to start the
  // sink stream.
  StreamNotStarted,
};

[[nodiscard]] constexpr auto describe(CameraSinkUnavailableReason reason) noexcept -> const char* {
  switch (reason) {
    case CameraSinkUnavailableReason::None:
      return "no error";
    case CameraSinkUnavailableReason::DeviceNotFound:
      return "the Sync Camera extension is not installed, or has not been approved in System Settings";
    case CameraSinkUnavailableReason::SinkStreamMissing:
      return "the Sync Camera extension is a different version than this Sync";
    case CameraSinkUnavailableReason::QueueNotProvided:
      return "the Sync Camera extension did not provide its sink queue";
    case CameraSinkUnavailableReason::StreamNotStarted:
      return "the Sync Camera extension did not start its sink stream";
  }
  return "unknown camera problem";
}

// The phrase above plus the CoreMediaIO status that produced it, when there is
// one. A bare phrase told a user what to do but told nobody why macOS said no;
// the OSStatus is what a bug report needs.
[[nodiscard]] inline auto describe_unavailability(CameraSinkUnavailableReason reason,
                                                  std::int32_t status) -> std::string {
  std::string text = describe(reason);
  if (status != 0) {
    text += " (OSStatus ";
    text += std::to_string(status);
    text += ')';
  }
  return text;
}

// One fitted frame: top-down 32BGRA, opaque, exactly the advertised canvas.
struct CameraSinkFrame {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::size_t row_stride = 0;
  std::span<const std::byte> bgra;
  std::uint64_t presentation_time_us = 0;
};

enum class CameraSinkSubmit : std::uint8_t {
  Accepted,
  Backpressured,
  Failed,
};

// Where fitted frames go. The production sink is the CoreMediaIO client in
// cmio_camera_sink.hpp; tests inject a fake.
class CameraSink {
 public:
  virtual ~CameraSink() = default;
  [[nodiscard]] virtual auto available() const noexcept -> bool = 0;
  [[nodiscard]] virtual auto unavailable_reason() const noexcept
      -> CameraSinkUnavailableReason = 0;
  // The CoreMediaIO OSStatus behind unavailable_reason(), or 0 when the reason
  // carries no status (not found, not installed, or available).
  [[nodiscard]] virtual auto unavailable_status() const noexcept -> std::int32_t { return 0; }
  // Whether submit() would accept a frame right now. Fitting a frame to the
  // canvas is the expensive step, so the provider asks first and reports
  // backpressure without converting when the answer is no. A sink with no
  // notion of capacity answers yes and lets submit() decide.
  [[nodiscard]] virtual auto has_capacity() const noexcept -> bool { return true; }
  // The frame is a borrowed view valid only for this call.
  virtual auto submit(const CameraSinkFrame& frame) noexcept -> CameraSinkSubmit = 0;
};

}  // namespace noisefactor::sync::camera
