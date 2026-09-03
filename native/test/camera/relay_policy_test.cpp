#include "test_harness.hpp"

#include <sync/platform/camera_relay_policy.hpp>

namespace {

using noisefactor::sync::camera::CameraRelayPolicy;
using Action = noisefactor::sync::camera::CameraRelayPolicy::Action;

}  // namespace

SYNC_TEST(relay_policy_emits_black_only_while_a_consumer_listens_and_no_client_frames_arrive) {
  CameraRelayPolicy policy(/*idle_interval_ns=*/100, /*idle_grace_ns=*/100);
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
  CameraRelayPolicy policy(100, 100);
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
  SYNC_REQUIRE(CameraRelayPolicy().idle_grace_ns() == CameraRelayPolicy::kDefaultIdleGraceNs);
  SYNC_REQUIRE(CameraRelayPolicy(100, 0).idle_grace_ns() == CameraRelayPolicy::kDefaultIdleGraceNs);
  // Grace shorter than the cadence would reintroduce the race; it is clamped.
  SYNC_REQUIRE(CameraRelayPolicy(100, 50).idle_grace_ns() == 100);
}

// Measured 2026-09-03 on the shipped 0.2.32 extension: a 30 fps sender whose
// frames land a few milliseconds late had black frames spliced in (5 of 148
// captured), because the 33 ms idle cadence doubled as the "sender is gone"
// threshold. A live sender must own the stream through ordinary jitter; black
// resumes only after a real gap, then at the idle cadence.
SYNC_TEST(relay_policy_never_splices_black_into_a_jittery_live_sender) {
  CameraRelayPolicy policy(/*idle_interval_ns=*/100, /*idle_grace_ns=*/750);
  policy.source_started();
  std::uint64_t now = 0;
  policy.client_frame_arrived(now);
  // Frames every 100 nominal, arriving up to 40 late; ticks every 100.
  const std::uint64_t lateness[] = {0, 40, 5, 38, 0, 40, 40, 0, 20, 39};
  for (std::uint64_t i = 1; i <= 10; ++i) {
    SYNC_REQUIRE(policy.tick(i * 100) == Action::None);
    now = i * 100 + lateness[i - 1];
    policy.client_frame_arrived(now);
  }
  // The sender stops: black waits out the grace period, then runs at cadence.
  SYNC_REQUIRE(policy.tick(now + 700) == Action::None);
  SYNC_REQUIRE(policy.tick(now + 750) == Action::EmitBlack);
  SYNC_REQUIRE(policy.tick(now + 800) == Action::None);
  SYNC_REQUIRE(policy.tick(now + 850) == Action::EmitBlack);
  // A returning sender silences black at once.
  policy.client_frame_arrived(now + 860);
  SYNC_REQUIRE(policy.tick(now + 950) == Action::None);
  SYNC_REQUIRE(policy.tick(now + 1'600) == Action::None);
  SYNC_REQUIRE(policy.tick(now + 1'610) == Action::EmitBlack);
}

// A viewer that opens the camera while no sender exists must not stare at
// nothing for the whole grace period: the first black still comes after one
// idle interval, exactly as before.
SYNC_TEST(relay_policy_shows_a_fresh_viewer_black_after_one_interval) {
  CameraRelayPolicy policy(100, 750);
  policy.source_started();
  SYNC_REQUIRE(policy.tick(50) == Action::None);
  SYNC_REQUIRE(policy.tick(100) == Action::EmitBlack);
  SYNC_REQUIRE(policy.tick(150) == Action::None);
  SYNC_REQUIRE(policy.tick(200) == Action::EmitBlack);
  // A viewer arriving after a sender went quiet does not inherit the old
  // sender's grace: its first tick paints black straight away.
  policy.client_frame_arrived(210);
  policy.source_stopped();
  policy.source_started();
  SYNC_REQUIRE(policy.tick(260) == Action::EmitBlack);
  SYNC_REQUIRE(policy.tick(300) == Action::None);
  SYNC_REQUIRE(policy.tick(360) == Action::EmitBlack);
}
