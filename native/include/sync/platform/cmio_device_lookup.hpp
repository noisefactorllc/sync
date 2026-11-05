#pragma once

#include <string_view>

namespace noisefactor::sync::camera {

// True when CoreMediaIO lists a device with this UID. This is the first test
// the camera sink makes, and the one that fails as DeviceNotFound. It only
// reads the device list and each device's UID: it opens no stream and copies
// no queue, so unlike constructing a CmioCameraSink it never makes the caller a
// sink client of the extension.
[[nodiscard]] auto cmio_device_present(std::string_view device_uid) noexcept -> bool;

}  // namespace noisefactor::sync::camera
