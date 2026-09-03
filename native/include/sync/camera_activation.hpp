#pragma once

#include <cstdint>
#include <string_view>

namespace noisefactor::sync::camera {

// Where Sync's request to make itself a camera stands. The menu shows one line
// for it; the titles below are that line.
//
// The two platforms get there differently -- macOS activates a system
// extension the user approves in System Settings, Windows registers a COM
// server behind a UAC prompt -- but the line reads the same way in both, so
// the states live together and each platform uses the ones that apply.
enum class CameraActivationState : std::uint8_t {
  Unknown,
  // macOS.
  NotInApplications,
  Requesting,
  NeedsApproval,
  // Windows. The media source needs its CLSID under HKLM, which needs an
  // administrator, so the line raises a UAC prompt rather than sending the
  // user to a settings pane.
  NeedsElevation,
  Registering,
  // Both.
  Active,
  ActiveAfterReboot,
  NotSupported,
  Failed,
};

[[nodiscard]] auto camera_activation_title(CameraActivationState state) noexcept -> const char*;

// True for the one state where selecting the menu line should take the user
// to System Settings to finish the job.
[[nodiscard]] auto camera_activation_opens_settings(CameraActivationState state) noexcept -> bool;

// True for the states where selecting the line should do anything at all.
// What the click does is per-platform; this only answers whether the line is
// live, which is what the menu needs to know to grey it out.
[[nodiscard]] auto camera_activation_is_actionable(CameraActivationState state) noexcept -> bool;

// macOS activates a system extension only for an app under /Applications.
// Anything else (a mounted DMG, ~/Applications, a lookalike folder name)
// must not even submit the request, or the user gets an error they cannot
// act on.
[[nodiscard]] auto bundle_is_in_applications(std::string_view bundle_path) noexcept -> bool;

}  // namespace noisefactor::sync::camera
