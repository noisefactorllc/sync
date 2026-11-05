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
  // macOS. The activation finished in this launch, yet CoreMediaIO still lists
  // no Sync Camera a few seconds after the helper reported it missing. macOS
  // can fail this way while it replaces an older version of the extension:
  // launchd is still removing the old extension's job when the new one is
  // submitted under the same label, the submit fails with "Operation already
  // in progress", and the new extension never runs although System Settings
  // lists it as enabled. Restarting the Mac is the only way out.
  NeedsRestart,
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

// When the macOS app starts its helper, and when its camera line asks for a
// restart.
//
// The helper opens the camera's sink stream as it starts. A helper started
// beside an open activation request is a client of the old extension while
// macOS replaces it, and is restarted a moment later anyway. So the helper
// waits for the request to answer, within a bound. Other processes on the Mac
// (Control Center, avconferenced) also hold the old extension, so this cannot
// prevent the replacement failure described at NeedsRestart; it only keeps Sync
// from being one more client of it.
//
// Every method takes the event, never the clock: the app owns the timers and
// passes the time in, so the order of events is testable without one.
class CameraLaunchSequence {
 public:
  // The longest the helper waits for the activation request to answer. A
  // request that can answer does so within milliseconds, whether it replaces
  // the extension or finds it already active, and a request that waits for the
  // user says so at once (needs_approval), when the helper starts. So this
  // bound only covers a request that never answers. It leaves sysextd room on
  // a Mac still busy with login, and it is the longest Syphon and NDI wait.
  static constexpr std::uint64_t kActivationWaitMs = 10'000;
  // CoreMediaIO publishes the device a moment after the activation completes.
  // A helper started in that gap finds no camera.
  static constexpr std::uint64_t kDevicePublishDelayMs = 3'000;
  // After the helper reports the camera missing, the app looks for the device
  // once more after this long before it asks for a restart, so a publication
  // that is only slow is not taken for the failure.
  static constexpr std::uint64_t kDeviceRecheckDelayMs = 5'000;

  enum class HelperStep : std::uint8_t { None, Start, Restart };
  struct HelperAction {
    HelperStep step = HelperStep::None;
    std::uint64_t delay_ms = 0;
    auto operator==(const HelperAction&) const -> bool = default;
  };

  // At launch. Without an activation request (Sync is not in /Applications)
  // nothing is replaced, and the helper starts now.
  [[nodiscard]] auto launched(bool activation_requested) noexcept -> HelperAction;
  // kActivationWaitMs after launch.
  [[nodiscard]] auto activation_wait_elapsed() noexcept -> HelperAction;
  // The request answers, one of these per answer.
  [[nodiscard]] auto needs_approval() noexcept -> HelperAction;
  [[nodiscard]] auto completed(std::uint64_t now_ms) noexcept -> HelperAction;
  [[nodiscard]] auto completes_after_reboot() noexcept -> HelperAction;
  [[nodiscard]] auto failed() noexcept -> HelperAction;

  // Each start of a helper the app owns, from any path: launch, recovery, or
  // Restart Sync chosen before the request answered.
  void helper_started(std::uint64_t now_ms) noexcept;

  // The app's own helper answered a health probe. True once, when the app
  // should look for the device again after kDeviceRecheckDelayMs: the
  // activation completed in this launch, this helper started after it, and it
  // has no camera.
  [[nodiscard]] auto helper_reported(bool camera_available) noexcept -> bool;

  // The second look. True when the camera line should ask for a restart: the
  // helper still has no camera and CoreMediaIO lists no Sync Camera device.
  [[nodiscard]] auto rechecked(bool camera_available, bool device_present) noexcept -> bool;

  [[nodiscard]] auto helper_released() const noexcept -> bool { return released_; }
  [[nodiscard]] auto needs_restart() const noexcept -> bool { return needs_restart_; }

 private:
  [[nodiscard]] auto release(HelperAction action) noexcept -> HelperAction;

  bool released_ = false;
  bool completed_ = false;
  std::uint64_t completed_at_ms_ = 0;
  bool helper_started_ = false;
  std::uint64_t helper_started_at_ms_ = 0;
  bool recheck_requested_ = false;
  bool needs_restart_ = false;
};

}  // namespace noisefactor::sync::camera
