#pragma once

#include <cstdint>
#include <string_view>

namespace noisefactor::sync::camera {

// Identity shared by the daemon (client side) and the Sync Camera extension.
// Both halves must agree on these or the daemon cannot find its own camera.
inline constexpr std::string_view kExtensionBundleId = "io.noisefactor.sync.camera";
inline constexpr std::string_view kDeviceUid = "io.noisefactor.sync.camera.device";
inline constexpr std::string_view kDeviceName = "Sync Camera";

// The one format the camera advertises. Consumers negotiate a fixed format
// at open time, so the daemon fits every sender frame into this canvas.
struct CameraCanvas {
  std::uint32_t width = 1920;
  std::uint32_t height = 1080;
};

inline constexpr CameraCanvas kCanvas{};
inline constexpr std::uint32_t kMaximumFramesPerSecond = 60;
inline constexpr std::uint32_t kBytesPerPixel = 4;

}  // namespace noisefactor::sync::camera
