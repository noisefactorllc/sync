#pragma once

#if !defined(_WIN32)
#error "camera_registration.hpp is available only on Windows"
#endif

namespace noisefactor::sync::camera {

// Puts SyncCamera.dll's CLSID under HKLM so the frame server can load it, and
// takes it back out again. Both need administrator rights, which is why they
// are whole commands: the tray app runs the first one elevated through a UAC
// prompt, and the uninstaller runs the second.
//
// Both return a process exit code -- 0 on success, 1 on failure -- because
// that is all the caller across the elevation boundary can observe.
[[nodiscard]] auto register_camera_source() noexcept -> int;

// Removes the virtual camera device as well as the registration, so the
// device does not linger in every picker after Sync is gone.
[[nodiscard]] auto unregister_camera_source() noexcept -> int;

}  // namespace noisefactor::sync::camera
