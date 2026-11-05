#include "test_harness.hpp"

#include <string>

#include <sync/camera_activation.hpp>

namespace {

using noisefactor::sync::camera::bundle_is_in_applications;
using noisefactor::sync::camera::CameraLaunchSequence;
using noisefactor::sync::camera::camera_activation_is_actionable;
using noisefactor::sync::camera::camera_activation_opens_settings;
using noisefactor::sync::camera::camera_activation_title;
using noisefactor::sync::camera::CameraActivationState;

}  // namespace

SYNC_TEST(camera_activation_only_trusts_the_real_applications_folder) {
  SYNC_REQUIRE(bundle_is_in_applications("/Applications/Sync.app"));
  SYNC_REQUIRE(bundle_is_in_applications("/Applications/Utilities/Sync.app"));
  SYNC_REQUIRE(!bundle_is_in_applications("/Applications-old/Sync.app"));
  SYNC_REQUIRE(!bundle_is_in_applications("/Users/x/Applications/Sync.app"));
  SYNC_REQUIRE(!bundle_is_in_applications("/Volumes/Sync/Sync.app"));
  SYNC_REQUIRE(!bundle_is_in_applications("/Applications/"));
  SYNC_REQUIRE(!bundle_is_in_applications(""));
}

SYNC_TEST(camera_activation_titles_tell_the_user_what_to_do_next) {
  SYNC_REQUIRE(std::string(camera_activation_title(CameraActivationState::NotInApplications)) ==
               "Camera: move Sync to Applications");
  SYNC_REQUIRE(std::string(camera_activation_title(CameraActivationState::NeedsApproval)) ==
               "Camera: approve in System Settings…");
  SYNC_REQUIRE(std::string(camera_activation_title(CameraActivationState::Active)) == "Camera: on");
  SYNC_REQUIRE(std::string(camera_activation_title(CameraActivationState::ActiveAfterReboot)) ==
               "Camera: on after restart");
  SYNC_REQUIRE(std::string(camera_activation_title(CameraActivationState::Failed)) ==
               "Camera: unavailable");
  SYNC_REQUIRE(std::string(camera_activation_title(CameraActivationState::Requesting)) ==
               "Camera: setting up…");
  SYNC_REQUIRE(std::string(camera_activation_title(CameraActivationState::Unknown)) ==
               "Camera: unknown");
}

SYNC_TEST(camera_activation_marks_which_states_open_system_settings) {
  SYNC_REQUIRE(camera_activation_opens_settings(CameraActivationState::NeedsApproval));
  SYNC_REQUIRE(!camera_activation_opens_settings(CameraActivationState::Active));
  SYNC_REQUIRE(!camera_activation_opens_settings(CameraActivationState::NotInApplications));
  SYNC_REQUIRE(!camera_activation_opens_settings(CameraActivationState::Failed));
}

SYNC_TEST(camera_activation_offers_the_windows_enable_line) {
  SYNC_REQUIRE(std::string(camera_activation_title(CameraActivationState::NeedsElevation)) ==
               "Enable Sync Camera…");
  SYNC_REQUIRE(std::string(camera_activation_title(CameraActivationState::NotSupported)) ==
               "Camera: needs Windows 11");
}

SYNC_TEST(camera_activation_lines_are_live_only_when_clicking_would_help) {
  SYNC_REQUIRE(camera_activation_is_actionable(CameraActivationState::NeedsElevation));
  SYNC_REQUIRE(camera_activation_is_actionable(CameraActivationState::NeedsApproval));
  // Worth another try: the usual cause is a declined prompt.
  SYNC_REQUIRE(camera_activation_is_actionable(CameraActivationState::Failed));

  SYNC_REQUIRE(!camera_activation_is_actionable(CameraActivationState::Active));
  SYNC_REQUIRE(!camera_activation_is_actionable(CameraActivationState::Registering));
  SYNC_REQUIRE(!camera_activation_is_actionable(CameraActivationState::Requesting));
  // Nothing a click can do about an OS that lacks the API.
  SYNC_REQUIRE(!camera_activation_is_actionable(CameraActivationState::NotSupported));
}

SYNC_TEST(camera_activation_keeps_the_settings_deep_link_to_macos_approval) {
  // The macOS menu item opens System Settings; the Windows one must not, or a
  // UAC-shaped problem would send the user to a pane that cannot fix it.
  SYNC_REQUIRE(camera_activation_opens_settings(CameraActivationState::NeedsApproval));
  SYNC_REQUIRE(!camera_activation_opens_settings(CameraActivationState::NeedsElevation));
}

SYNC_TEST(camera_activation_restart_line_names_the_restart_and_is_not_a_button) {
  SYNC_REQUIRE(std::string(camera_activation_title(CameraActivationState::NeedsRestart)) ==
               "Camera: restart your Mac to finish updating");
  SYNC_REQUIRE(!camera_activation_is_actionable(CameraActivationState::NeedsRestart));
  SYNC_REQUIRE(!camera_activation_opens_settings(CameraActivationState::NeedsRestart));
}

namespace {

using Step = CameraLaunchSequence::HelperStep;
using Action = CameraLaunchSequence::HelperAction;

constexpr Action kNothing{};
constexpr Action kStartNow{.step = Step::Start};
constexpr Action kStartAfterPublish{.step = Step::Start,
                                    .delay_ms = CameraLaunchSequence::kDevicePublishDelayMs};
constexpr Action kRestartAfterPublish{.step = Step::Restart,
                                      .delay_ms = CameraLaunchSequence::kDevicePublishDelayMs};

}  // namespace

SYNC_TEST(camera_launch_holds_the_helper_until_the_activation_completes) {
  CameraLaunchSequence sequence;
  SYNC_REQUIRE(sequence.launched(true) == kNothing);
  SYNC_REQUIRE(!sequence.helper_released());
  // One start, once the device can be published; no start-then-restart.
  SYNC_REQUIRE(sequence.completed(100) == kStartAfterPublish);
  SYNC_REQUIRE(sequence.helper_released());
  SYNC_REQUIRE(sequence.activation_wait_elapsed() == kNothing);
}

SYNC_TEST(camera_launch_starts_the_helper_at_once_without_an_activation_request) {
  CameraLaunchSequence sequence;
  SYNC_REQUIRE(sequence.launched(false) == kStartNow);
  SYNC_REQUIRE(sequence.activation_wait_elapsed() == kNothing);
}

SYNC_TEST(camera_launch_starts_the_helper_when_the_request_ends_any_other_way) {
  {
    CameraLaunchSequence sequence;
    SYNC_REQUIRE(sequence.launched(true) == kNothing);
    SYNC_REQUIRE(sequence.failed() == kStartNow);
    SYNC_REQUIRE(sequence.activation_wait_elapsed() == kNothing);
  }
  {
    CameraLaunchSequence sequence;
    SYNC_REQUIRE(sequence.launched(true) == kNothing);
    SYNC_REQUIRE(sequence.completes_after_reboot() == kStartNow);
    SYNC_REQUIRE(sequence.activation_wait_elapsed() == kNothing);
  }
  {
    // Approval waits on the user; Syphon and NDI do not.
    CameraLaunchSequence sequence;
    SYNC_REQUIRE(sequence.launched(true) == kNothing);
    SYNC_REQUIRE(sequence.needs_approval() == kStartNow);
    SYNC_REQUIRE(sequence.activation_wait_elapsed() == kNothing);
  }
}

SYNC_TEST(camera_launch_starts_the_helper_when_the_request_never_answers) {
  CameraLaunchSequence sequence;
  SYNC_REQUIRE(sequence.launched(true) == kNothing);
  SYNC_REQUIRE(sequence.activation_wait_elapsed() == kStartNow);
  SYNC_REQUIRE(sequence.activation_wait_elapsed() == kNothing);
}

SYNC_TEST(camera_launch_restarts_a_helper_that_ran_before_the_activation_completed) {
  {
    // The wait ran out first.
    CameraLaunchSequence sequence;
    (void)sequence.launched(true);
    SYNC_REQUIRE(sequence.activation_wait_elapsed() == kStartNow);
    sequence.helper_started(10'000);
    SYNC_REQUIRE(sequence.completed(12'000) == kRestartAfterPublish);
  }
  {
    // Approval came later.
    CameraLaunchSequence sequence;
    (void)sequence.launched(true);
    SYNC_REQUIRE(sequence.needs_approval() == kStartNow);
    sequence.helper_started(5);
    SYNC_REQUIRE(sequence.completed(60'000) == kRestartAfterPublish);
  }
  {
    // The user chose Restart Sync before the request answered.
    CameraLaunchSequence sequence;
    (void)sequence.launched(true);
    sequence.helper_started(5);
    SYNC_REQUIRE(sequence.helper_released());
    SYNC_REQUIRE(sequence.activation_wait_elapsed() == kNothing);
    SYNC_REQUIRE(sequence.completed(10) == kRestartAfterPublish);
  }
}

SYNC_TEST(camera_launch_asks_for_a_restart_when_the_completed_camera_never_appears) {
  CameraLaunchSequence sequence;
  (void)sequence.launched(true);
  (void)sequence.completed(100);
  sequence.helper_started(3'100);
  SYNC_REQUIRE(sequence.helper_reported(false));
  // One second look per launch, however often the helper reports.
  SYNC_REQUIRE(!sequence.helper_reported(false));
  SYNC_REQUIRE(!sequence.needs_restart());
  SYNC_REQUIRE(sequence.rechecked(false, false));
  SYNC_REQUIRE(sequence.needs_restart());
}

SYNC_TEST(camera_launch_keeps_the_line_when_the_second_look_finds_the_camera) {
  {
    // The device was only slow to publish.
    CameraLaunchSequence sequence;
    (void)sequence.launched(true);
    (void)sequence.completed(100);
    sequence.helper_started(3'100);
    SYNC_REQUIRE(sequence.helper_reported(false));
    SYNC_REQUIRE(!sequence.rechecked(false, true));
    SYNC_REQUIRE(!sequence.needs_restart());
  }
  {
    // A restarted helper has the camera now.
    CameraLaunchSequence sequence;
    (void)sequence.launched(true);
    (void)sequence.completed(100);
    sequence.helper_started(3'100);
    SYNC_REQUIRE(sequence.helper_reported(false));
    SYNC_REQUIRE(!sequence.rechecked(true, false));
    SYNC_REQUIRE(!sequence.needs_restart());
  }
}

SYNC_TEST(camera_launch_does_not_look_again_when_the_helper_has_the_camera) {
  CameraLaunchSequence sequence;
  (void)sequence.launched(true);
  (void)sequence.completed(100);
  sequence.helper_started(3'100);
  SYNC_REQUIRE(!sequence.helper_reported(true));
  SYNC_REQUIRE(!sequence.rechecked(false, false));
  SYNC_REQUIRE(!sequence.needs_restart());
}

SYNC_TEST(camera_launch_ignores_a_helper_started_before_the_activation_completed) {
  CameraLaunchSequence sequence;
  (void)sequence.launched(true);
  SYNC_REQUIRE(sequence.activation_wait_elapsed() == kStartNow);
  sequence.helper_started(10'000);
  (void)sequence.completed(12'000);
  // Still the helper from before the replacement: its report is about the old
  // extension, so it is not evidence either way.
  SYNC_REQUIRE(!sequence.helper_reported(false));
  sequence.helper_started(15'000);
  SYNC_REQUIRE(sequence.helper_reported(false));
}

SYNC_TEST(camera_launch_keeps_todays_line_when_the_activation_did_not_complete) {
  {
    // A first install still waiting for approval.
    CameraLaunchSequence sequence;
    (void)sequence.launched(true);
    (void)sequence.needs_approval();
    sequence.helper_started(5);
    SYNC_REQUIRE(!sequence.helper_reported(false));
    SYNC_REQUIRE(!sequence.rechecked(false, false));
    SYNC_REQUIRE(!sequence.needs_restart());
  }
  {
    // A denied approval.
    CameraLaunchSequence sequence;
    (void)sequence.launched(true);
    (void)sequence.failed();
    sequence.helper_started(5);
    SYNC_REQUIRE(!sequence.helper_reported(false));
    SYNC_REQUIRE(!sequence.rechecked(false, false));
  }
  {
    // macOS finishes it at the next restart and the line already says so.
    CameraLaunchSequence sequence;
    (void)sequence.launched(true);
    (void)sequence.completes_after_reboot();
    sequence.helper_started(5);
    SYNC_REQUIRE(!sequence.helper_reported(false));
  }
  {
    // Not in /Applications: no request, nothing replaced.
    CameraLaunchSequence sequence;
    (void)sequence.launched(false);
    sequence.helper_started(5);
    SYNC_REQUIRE(!sequence.helper_reported(false));
  }
}
