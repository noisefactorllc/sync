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
    case CameraActivationState::Active:
      return "Camera: on";
    case CameraActivationState::ActiveAfterReboot:
      return "Camera: on after restart";
    case CameraActivationState::Failed:
      return "Camera: unavailable";
  }
  return "Camera: unknown";
}

auto camera_activation_opens_settings(CameraActivationState state) noexcept -> bool {
  return state == CameraActivationState::NeedsApproval;
}

auto bundle_is_in_applications(std::string_view bundle_path) noexcept -> bool {
  constexpr std::string_view kPrefix = "/Applications/";
  return bundle_path.size() > kPrefix.size() && bundle_path.substr(0, kPrefix.size()) == kPrefix;
}

}  // namespace noisefactor::sync::camera
