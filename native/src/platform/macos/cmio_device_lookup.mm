#include <sync/platform/cmio_device_lookup.hpp>

#import <CoreFoundation/CoreFoundation.h>
#import <CoreMediaIO/CMIOHardware.h>

#include <string>
#include <vector>

namespace noisefactor::sync::camera {

namespace {

[[nodiscard]] auto device_ids(std::vector<CMIODeviceID>& out) noexcept -> bool {
  CMIOObjectPropertyAddress address{kCMIOHardwarePropertyDevices, kCMIOObjectPropertyScopeGlobal,
                                    kCMIOObjectPropertyElementMain};
  UInt32 size = 0;
  if (CMIOObjectGetPropertyDataSize(kCMIOObjectSystemObject, &address, 0, nullptr, &size) !=
      kCMIOHardwareNoError) {
    return false;
  }
  try {
    out.assign(size / sizeof(CMIODeviceID), kCMIOObjectUnknown);
  } catch (...) {
    return false;
  }
  if (out.empty()) return true;
  UInt32 used = 0;
  return CMIOObjectGetPropertyData(kCMIOObjectSystemObject, &address, 0, nullptr, size, &used,
                                   out.data()) == kCMIOHardwareNoError;
}

[[nodiscard]] auto device_uid(CMIODeviceID device) noexcept -> std::string {
  CMIOObjectPropertyAddress address{kCMIODevicePropertyDeviceUID, kCMIOObjectPropertyScopeGlobal,
                                    kCMIOObjectPropertyElementMain};
  CFStringRef uid = nullptr;
  UInt32 used = 0;
  if (CMIOObjectGetPropertyData(device, &address, 0, nullptr, sizeof(uid), &used, &uid) !=
          kCMIOHardwareNoError ||
      uid == nullptr) {
    return {};
  }
  char buffer[256] = {0};
  const bool ok = CFStringGetCString(uid, buffer, sizeof(buffer), kCFStringEncodingUTF8);
  CFRelease(uid);
  return ok ? std::string(buffer) : std::string{};
}

}  // namespace

auto cmio_device_present(std::string_view device_uid_wanted) noexcept -> bool {
  // A list that cannot be read counts as absent, as it does for the sink.
  std::vector<CMIODeviceID> devices;
  if (!device_ids(devices)) return false;
  for (const CMIODeviceID device : devices) {
    if (device_uid(device) == device_uid_wanted) return true;
  }
  return false;
}

}  // namespace noisefactor::sync::camera
