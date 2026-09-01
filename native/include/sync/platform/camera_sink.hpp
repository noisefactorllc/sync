#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace noisefactor::sync::camera {

// Why the camera could not be reached. The daemon prints the matching phrase
// on stderr when the provider is selected but unavailable, so each one names
// a different thing for the user to do.
enum class CameraSinkUnavailableReason : std::uint8_t {
  None = 0,
  DeviceNotFound,
  SinkStreamMissing,
  ConnectionRefused,
};

[[nodiscard]] constexpr auto describe(CameraSinkUnavailableReason reason) noexcept -> const char* {
  switch (reason) {
    case CameraSinkUnavailableReason::None:
      return "no error";
    case CameraSinkUnavailableReason::DeviceNotFound:
      return "the Sync Camera extension is not installed, or has not been approved in System Settings";
    case CameraSinkUnavailableReason::SinkStreamMissing:
      return "the Sync Camera extension is a different version than this Sync";
    case CameraSinkUnavailableReason::ConnectionRefused:
      return "the Sync Camera extension did not accept a connection";
  }
  return "unknown camera problem";
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
  // The frame is a borrowed view valid only for this call.
  virtual auto submit(const CameraSinkFrame& frame) noexcept -> CameraSinkSubmit = 0;
};

}  // namespace noisefactor::sync::camera
