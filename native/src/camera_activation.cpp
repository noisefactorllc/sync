#include <sync/camera_activation.hpp>

namespace noisefactor::sync::camera {

auto camera_activation_title(CameraActivationState state) noexcept -> const char* {
  switch (state) {
    case CameraActivationState::Unknown:
      return "Camera: unknown";
    case CameraActivationState::NotInApplications:
      return "Camera: move Sync to Applications";
    case CameraActivationState::Requesting:
      return "Camera: setting up…";
    case CameraActivationState::NeedsApproval:
      return "Camera: approve in System Settings…";
    case CameraActivationState::NeedsElevation:
      return "Enable Sync Camera…";
    case CameraActivationState::Registering:
      return "Camera: setting up…";
    case CameraActivationState::Active:
      return "Camera: on";
    case CameraActivationState::ActiveAfterReboot:
      return "Camera: on after restart";
    case CameraActivationState::NotSupported:
      return "Camera: needs Windows 11";
    case CameraActivationState::Failed:
      return "Camera: unavailable";
  }
  return "Camera: unknown";
}

auto camera_activation_opens_settings(CameraActivationState state) noexcept -> bool {
  return state == CameraActivationState::NeedsApproval;
}

auto camera_activation_is_actionable(CameraActivationState state) noexcept -> bool {
  switch (state) {
    case CameraActivationState::NeedsApproval:
    case CameraActivationState::NeedsElevation:
    // A failed attempt is worth offering again: the usual cause is a declined
    // UAC prompt or a denied approval, both of which the user can change their
    // mind about.
    case CameraActivationState::Failed:
      return true;
    case CameraActivationState::Unknown:
    case CameraActivationState::NotInApplications:
    case CameraActivationState::Requesting:
    case CameraActivationState::Registering:
    case CameraActivationState::Active:
    case CameraActivationState::ActiveAfterReboot:
    case CameraActivationState::NotSupported:
      return false;
  }
  return false;
}

auto bundle_is_in_applications(std::string_view bundle_path) noexcept -> bool {
  constexpr std::string_view kPrefix = "/Applications/";
  return bundle_path.size() > kPrefix.size() && bundle_path.substr(0, kPrefix.size()) == kPrefix;
}

}  // namespace noisefactor::sync::camera
