#pragma once

#include <cstdint>
#include <string_view>

namespace noisefactor::sync::camera {

// Where Sync.app's request to activate the camera extension stands. The menu
// shows one line for it; the titles below are that line.
enum class CameraActivationState : std::uint8_t {
  Unknown,
  NotInApplications,
  Requesting,
  NeedsApproval,
  Active,
  ActiveAfterReboot,
  Failed,
};

[[nodiscard]] auto camera_activation_title(CameraActivationState state) noexcept -> const char*;

// True for the one state where selecting the menu line should take the user
// to System Settings to finish the job.
[[nodiscard]] auto camera_activation_opens_settings(CameraActivationState state) noexcept -> bool;

// macOS activates a system extension only for an app under /Applications.
// Anything else (a mounted DMG, ~/Applications, a lookalike folder name)
// must not even submit the request, or the user gets an error they cannot
// act on.
[[nodiscard]] auto bundle_is_in_applications(std::string_view bundle_path) noexcept -> bool;

}  // namespace noisefactor::sync::camera
