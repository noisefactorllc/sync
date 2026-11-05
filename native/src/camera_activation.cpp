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
    case CameraActivationState::NeedsRestart:
      return "Camera: restart your Mac to finish updating";
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
    // Only a restart of the Mac helps, and the line says so.
    case CameraActivationState::NeedsRestart:
      return false;
  }
  return false;
}

auto bundle_is_in_applications(std::string_view bundle_path) noexcept -> bool {
  constexpr std::string_view kPrefix = "/Applications/";
  return bundle_path.size() > kPrefix.size() && bundle_path.substr(0, kPrefix.size()) == kPrefix;
}

auto CameraLaunchSequence::release(HelperAction action) noexcept -> HelperAction {
  // The helper is released once. Every later answer either has nothing to do,
  // or restarts the helper that is already running (see completed).
  if (released_) return {};
  released_ = true;
  return action;
}

auto CameraLaunchSequence::launched(bool activation_requested) noexcept -> HelperAction {
  if (activation_requested) return {};
  return release({.step = HelperStep::Start});
}

auto CameraLaunchSequence::activation_wait_elapsed() noexcept -> HelperAction {
  return release({.step = HelperStep::Start});
}

auto CameraLaunchSequence::needs_approval() noexcept -> HelperAction {
  // Approval can take as long as the user likes, and nothing is replaced
  // until it comes, so Syphon and NDI do not wait for it.
  return release({.step = HelperStep::Start});
}

auto CameraLaunchSequence::completed(std::uint64_t now_ms) noexcept -> HelperAction {
  completed_ = true;
  completed_at_ms_ = now_ms;
  if (!released_) {
    released_ = true;
    return {.step = HelperStep::Start, .delay_ms = kDevicePublishDelayMs};
  }
  // A helper already runs (the wait ran out, approval was pending, or the
  // user chose Restart Sync). It computed its providers before the device
  // existed, so it starts again once the device is published.
  return {.step = HelperStep::Restart, .delay_ms = kDevicePublishDelayMs};
}

auto CameraLaunchSequence::completes_after_reboot() noexcept -> HelperAction {
  return release({.step = HelperStep::Start});
}

auto CameraLaunchSequence::failed() noexcept -> HelperAction {
  return release({.step = HelperStep::Start});
}

void CameraLaunchSequence::helper_started(std::uint64_t now_ms) noexcept {
  released_ = true;
  helper_started_ = true;
  helper_started_at_ms_ = now_ms;
}

auto CameraLaunchSequence::helper_reported(bool camera_available) noexcept -> bool {
  // A helper that started before the activation completed reports on the old
  // extension, so only a helper started after it counts.
  if (camera_available || recheck_requested_ || !completed_ || !helper_started_ ||
      helper_started_at_ms_ < completed_at_ms_) {
    return false;
  }
  recheck_requested_ = true;
  return true;
}

auto CameraLaunchSequence::rechecked(bool camera_available, bool device_present) noexcept -> bool {
  if (!recheck_requested_) return false;
  if (!camera_available && !device_present) needs_restart_ = true;
  return needs_restart_;
}

}  // namespace noisefactor::sync::camera
