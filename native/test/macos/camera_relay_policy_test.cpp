#include "test_harness.hpp"

#include <sync/platform/camera_relay_policy.hpp>

namespace {

using noisefactor::sync::camera::CameraRelayPolicy;
using Action = noisefactor::sync::camera::CameraRelayPolicy::Action;

}  // namespace

SYNC_TEST(relay_policy_emits_black_only_while_a_consumer_listens_and_no_client_frames_arrive) {
  CameraRelayPolicy policy(/*idle_interval_ns=*/100);
  SYNC_REQUIRE(policy.tick(0) == Action::None);
  policy.source_started();
  SYNC_REQUIRE(policy.tick(50) == Action::None);
  SYNC_REQUIRE(policy.tick(100) == Action::EmitBlack);
  SYNC_REQUIRE(policy.tick(150) == Action::None);
  SYNC_REQUIRE(policy.tick(200) == Action::EmitBlack);
  policy.client_frame_arrived(210);
  SYNC_REQUIRE(policy.tick(300) == Action::None);
  SYNC_REQUIRE(policy.tick(310) == Action::EmitBlack);
  policy.source_stopped();
  SYNC_REQUIRE(policy.tick(1'000) == Action::None);
}

SYNC_TEST(relay_policy_counts_consumers_so_one_stop_does_not_silence_a_second_listener) {
  CameraRelayPolicy policy(100);
  policy.source_started();
  policy.source_started();
  policy.source_stopped();
  SYNC_REQUIRE(policy.source_active());
  SYNC_REQUIRE(policy.tick(100) == Action::EmitBlack);
  policy.source_stopped();
  SYNC_REQUIRE(!policy.source_active());
  SYNC_REQUIRE(policy.tick(10'000) == Action::None);
}

SYNC_TEST(relay_policy_defaults_to_thirty_frames_per_second_and_rejects_a_zero_interval) {
  SYNC_REQUIRE(CameraRelayPolicy().idle_interval_ns() == CameraRelayPolicy::kDefaultIdleIntervalNs);
  SYNC_REQUIRE(CameraRelayPolicy(0).idle_interval_ns() == CameraRelayPolicy::kDefaultIdleIntervalNs);
}
