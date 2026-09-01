#include "test_harness.hpp"

#include <string>

#include <sync/camera_activation.hpp>

namespace {

using noisefactor::sync::camera::bundle_is_in_applications;
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
