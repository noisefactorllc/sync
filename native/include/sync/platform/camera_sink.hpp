#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
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

  // Windows. The camera is a Media Foundation virtual camera there, so it
  // fails in entirely different ways than a CoreMediaIO extension does.
  //
  // The running Windows build is older than 22000, where
  // MFCreateVirtualCamera does not exist.
  NotSupported,
  // SyncCamera.dll's CLSID is not under HKLM, so the frame server cannot load
  // it. The tray's Enable Sync Camera line is what fixes this.
  SourceNotRegistered,
  // MFCreateVirtualCamera itself refused, most often because camera privacy
  // settings deny access.
  VirtualCameraRefused,
  // The section exists but its DACL refused this account.
  SectionAccessDenied,
  // The section was opened but is not the size this build expects, which
  // means the media source and the daemon are different versions.
  SectionVersionMismatch,

  // Linux. The v4l2loopback output device is a kernel-owned /dev/videoN;
  // selection, permissions, format negotiation, and writes fail separately.
  DevicePermissionDenied,
  WrongDevice,
  FormatRejected,
  DeviceWriteFailed,
};

[[nodiscard]] constexpr auto describe(CameraSinkUnavailableReason reason) noexcept -> const char* {
  switch (reason) {
    case CameraSinkUnavailableReason::None:
      return "no error";
    case CameraSinkUnavailableReason::DeviceNotFound:
#if defined(__linux__)
      return "device_not_found: no validated Sync Camera v4l2loopback output is present";
#else
      return "the Sync Camera extension is not installed, or has not been approved in System Settings";
#endif
    case CameraSinkUnavailableReason::SinkStreamMissing:
      return "the Sync Camera extension is a different version than this Sync";
    case CameraSinkUnavailableReason::QueueNotProvided:
      return "the Sync Camera extension did not provide its sink queue";
    case CameraSinkUnavailableReason::StreamNotStarted:
      return "the Sync Camera extension did not start its sink stream";
    case CameraSinkUnavailableReason::NotSupported:
      return "the camera needs Windows 11 (build 22000) or later";
    case CameraSinkUnavailableReason::SourceNotRegistered:
      return "the Sync Camera source is not registered; choose Enable Sync Camera from the Sync tray menu";
    case CameraSinkUnavailableReason::VirtualCameraRefused:
      return "Windows refused to create the Sync camera; camera privacy settings may be denying access";
    case CameraSinkUnavailableReason::SectionAccessDenied:
      return "the Sync Camera source refused this account access to its frame buffer";
    case CameraSinkUnavailableReason::SectionVersionMismatch:
      return "the Sync Camera source is a different version than this Sync; reinstall Sync";
    case CameraSinkUnavailableReason::DevicePermissionDenied:
      return "permission to write the Sync Camera device was denied";
    case CameraSinkUnavailableReason::WrongDevice:
      return "the selected video node is not the Sync v4l2loopback output";
    case CameraSinkUnavailableReason::FormatRejected:
      return "the Sync Camera device rejected 1920x1080 NV12 output";
    case CameraSinkUnavailableReason::DeviceWriteFailed:
      return "the Sync Camera device could not accept frames";
  }
  return "unknown camera problem";
}

// The phrase above plus the status that produced it, when there is one. A bare
// phrase told a user what to do but told nobody why the system said no, and
// that status is what a bug report needs. The two platforms number their
// failures differently, so each is named for what it is: an OSStatus is a
// small signed integer, an HRESULT is read as hex.
[[nodiscard]] inline auto describe_unavailability(CameraSinkUnavailableReason reason,
                                                  std::int32_t status) -> std::string {
  std::string text = describe(reason);
  if (status != 0) {
#if defined(_WIN32)
    // " (HRESULT 0x" + 8 hex digits + ")" is 21 characters plus a terminator.
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), " (HRESULT 0x%08X)", static_cast<unsigned>(status));
    text += buffer;
#elif defined(__linux__)
    text += " (errno ";
    text += std::to_string(status);
    text += ')';
#else
    text += " (OSStatus ";
    text += std::to_string(status);
    text += ')';
#endif
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

// Where fitted frames go. The production sinks are the CoreMediaIO client in
// cmio_camera_sink.hpp and the shared-ring writer in mf_camera_sink.hpp; tests
// inject a fake.
class CameraSink {
 public:
  virtual ~CameraSink() = default;
  [[nodiscard]] virtual auto available() const noexcept -> bool = 0;
  [[nodiscard]] virtual auto unavailable_reason() const noexcept
      -> CameraSinkUnavailableReason = 0;
  // The platform status behind unavailable_reason() -- a CoreMediaIO OSStatus
  // on macOS, an HRESULT on Windows -- or 0 when the reason carries no status
  // (not found, not installed, or available).
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
