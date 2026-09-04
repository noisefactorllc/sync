#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include <linux/videodev2.h>

namespace noisefactor::sync::camera {

enum class LinuxCameraDeviceError {
  None,
  NotFound,
  Ambiguous,
  InvalidPath,
  OpenDenied,
  NotCharacterDevice,
  WrongCard,
  WrongDriver,
  MissingOutputCapability,
  FormatRejected,
  InvalidFormatBounds,
  Io,
};

struct LinuxCameraFormat {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::size_t y_stride = 0;
  std::size_t size_image = 0;
};

struct LinuxCameraOpenResult {
  LinuxCameraDeviceError error = LinuxCameraDeviceError::Io;
  std::int32_t native_error = 0;
  int descriptor = -1;
  std::array<char, 64> path{};
  LinuxCameraFormat format{};
};

class LinuxCameraDeviceOps {
 public:
  virtual ~LinuxCameraDeviceOps() = default;
  virtual auto enumerate(std::span<std::array<char, 64>> output) noexcept
      -> std::size_t = 0;
  virtual auto open_no_follow(std::string_view path) noexcept -> int = 0;
  virtual auto validate_character_device(int descriptor) noexcept -> bool = 0;
  virtual auto query_capabilities(int descriptor,
                                  v4l2_capability& output) noexcept -> int = 0;
  virtual auto set_nv12_format(int descriptor, v4l2_format& format) noexcept
      -> int = 0;
  virtual auto set_frame_rate(int descriptor, v4l2_streamparm& rate) noexcept
      -> int = 0;
  virtual auto write_frame(int descriptor,
                           std::span<const std::byte> frame) noexcept
      -> std::pair<std::ptrdiff_t, std::int32_t> = 0;
  virtual void close_descriptor(int descriptor) noexcept = 0;
};

[[nodiscard]] auto default_linux_camera_device_ops() noexcept
    -> LinuxCameraDeviceOps&;
[[nodiscard]] auto open_linux_camera(
    std::string_view explicit_path,
    LinuxCameraDeviceOps& operations) noexcept -> LinuxCameraOpenResult;
[[nodiscard]] auto probe_linux_camera(
    std::string_view explicit_path,
    LinuxCameraDeviceOps& operations) noexcept -> LinuxCameraOpenResult;

}  // namespace noisefactor::sync::camera
