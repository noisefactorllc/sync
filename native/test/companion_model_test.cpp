#include "test_harness.hpp"

#include <sync/companion_model.hpp>

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace {

using noisefactor::sync::companion::CompanionModel;
using noisefactor::sync::companion::CompanionState;
using noisefactor::sync::companion::HealthSnapshot;
using noisefactor::sync::companion::AvailableProviders;
using noisefactor::sync::companion::RecoverySchedule;

AvailableProviders providers(std::initializer_list<std::string_view> ids) {
  AvailableProviders result;
  for (const std::string_view id : ids) {
    (void)result.add(id);
  }
  return result;
}

HealthSnapshot healthy(std::size_t senders = 0) {
  return {
      .reachable = true,
      .compatible = true,
      .product = "Sync",
      .version = "0.2.7",
      .providers = providers({"syphon"}),
      .active_senders = senders,
  };
}

SYNC_TEST(companion_model_tracks_owned_and_external_health) {
  CompanionModel model("0.2.7");
  SYNC_REQUIRE(model.state() == CompanionState::Stopped);

  model.begin_start();
  SYNC_REQUIRE(model.state() == CompanionState::Starting);
  model.helper_started(42);
  model.observe_health(healthy(2), 100);
  SYNC_REQUIRE(model.state() == CompanionState::Running);
  SYNC_REQUIRE(model.owned_pid() == std::optional<int>(42));
  SYNC_REQUIRE(model.health().active_senders == std::optional<std::size_t>(2));

  CompanionModel external("0.2.7");
  external.observe_health(healthy(1), 100);
  SYNC_REQUIRE(external.state() == CompanionState::External);
  SYNC_REQUIRE(!external.owned_pid().has_value());
}

SYNC_TEST(companion_model_distinguishes_startup_failure_conflict_and_exit) {
  CompanionModel model("0.2.7");
  model.begin_start();
  model.helper_started(99);
  SYNC_REQUIRE(!model.observe_health_failure(100));
  SYNC_REQUIRE(model.state() == CompanionState::Starting);

  auto incompatible = healthy();
  incompatible.compatible = false;
  incompatible.version = "9.0.0";
  model.observe_health(std::move(incompatible), 200);
  SYNC_REQUIRE(model.state() == CompanionState::PortConflict);

  const auto recovery = model.helper_exited(70, false, 1'000);
  SYNC_REQUIRE(recovery.has_value());
  SYNC_REQUIRE(model.state() == CompanionState::Recovering);
  SYNC_REQUIRE(model.last_exit_status() == std::optional<int>(70));
  SYNC_REQUIRE(!model.owned_pid().has_value());

  // A duplicate notification cannot consume another attempt or replace the
  // already-reserved deadline.
  const auto duplicate = model.helper_exited(70, false, 1'001);
  SYNC_REQUIRE(duplicate.has_value());
  SYNC_REQUIRE(duplicate->generation == recovery->generation);
  SYNC_REQUIRE(duplicate->attempt == recovery->attempt);
  SYNC_REQUIRE(duplicate->due_ms == recovery->due_ms);

  model.manual_restart();
  SYNC_REQUIRE(model.state() == CompanionState::Starting);
  SYNC_REQUIRE(!model.last_exit_status().has_value());
  model.helper_started(100);
  SYNC_REQUIRE(!model.helper_exited(0, true, 2'000).has_value());
  SYNC_REQUIRE(model.state() == CompanionState::Stopped);
  SYNC_REQUIRE(!model.observe_health_failure(2'001));
  SYNC_REQUIRE(model.state() == CompanionState::Stopped);
}

SYNC_TEST(companion_model_recovery_uses_exact_delays_and_a_three_attempt_ceiling) {
  CompanionModel model("0.2.7");
  model.begin_start();
  model.helper_started(41);
  model.observe_health(healthy(), 100);

  const auto first = model.helper_exited(70, false, 1'000);
  SYNC_REQUIRE(first == std::optional<RecoverySchedule>({
      .generation = first->generation,
      .attempt = 1,
      .due_ms = 1'250,
  }));
  SYNC_REQUIRE(model.recovery_attempts() == 1);
  SYNC_REQUIRE(!model.begin_recovery_attempt(*first, 1'249));
  SYNC_REQUIRE(model.begin_recovery_attempt(*first, 1'250));
  SYNC_REQUIRE(!model.begin_recovery_attempt(*first, 1'250));

  const auto second = model.recovery_launch_failed(-1, 1'300);
  SYNC_REQUIRE(second.has_value());
  SYNC_REQUIRE(second->attempt == 2);
  SYNC_REQUIRE(second->due_ms == 2'300);
  SYNC_REQUIRE(model.begin_recovery_attempt(*second, 2'300));

  const auto third = model.recovery_launch_failed(-1, 2'350);
  SYNC_REQUIRE(third.has_value());
  SYNC_REQUIRE(third->attempt == 3);
  SYNC_REQUIRE(third->due_ms == 6'350);
  SYNC_REQUIRE(model.begin_recovery_attempt(*third, 6'350));

  SYNC_REQUIRE(!model.recovery_launch_failed(-1, 6'400).has_value());
  SYNC_REQUIRE(model.recovery_attempts() == 3);
  SYNC_REQUIRE(model.state() == CompanionState::RecoveryExhausted);
}

SYNC_TEST(companion_model_recovery_resets_only_after_sixty_healthy_seconds) {
  CompanionModel model("0.2.7");
  model.begin_start();
  model.helper_started(41);

  auto first = model.helper_exited(70, false, 100);
  SYNC_REQUIRE(first.has_value());
  SYNC_REQUIRE(model.begin_recovery_attempt(*first, 350));
  model.helper_started(42, 350);
  model.observe_health(healthy(), 400);
  SYNC_REQUIRE(model.state() == CompanionState::Running);
  model.observe_health(healthy(), 60'399);

  auto second = model.helper_exited(70, false, 60'399);
  SYNC_REQUIRE(second.has_value());
  SYNC_REQUIRE(second->attempt == 2);
  SYNC_REQUIRE(second->due_ms == 61'399);

  CompanionModel stable("0.2.7");
  stable.begin_start();
  stable.helper_started(51);
  auto stable_first = stable.helper_exited(70, false, 100);
  SYNC_REQUIRE(stable_first.has_value());
  SYNC_REQUIRE(stable.begin_recovery_attempt(*stable_first, 350));
  stable.helper_started(52, 350);
  stable.observe_health(healthy(), 400);
  stable.observe_health(healthy(), 60'400);
  SYNC_REQUIRE(stable.recovery_attempts() == 0);

  auto reset_first = stable.helper_exited(70, false, 60'500);
  SYNC_REQUIRE(reset_first.has_value());
  SYNC_REQUIRE(reset_first->attempt == 1);
  SYNC_REQUIRE(reset_first->due_ms == 60'750);
}

SYNC_TEST(companion_model_bounds_unhealthy_recovery_and_cancels_stale_work) {
  CompanionModel model("0.2.7");
  model.begin_start();
  model.helper_started(41);
  const auto first = model.helper_exited(70, false, 100);
  SYNC_REQUIRE(first.has_value());
  SYNC_REQUIRE(model.begin_recovery_attempt(*first, 350));
  model.helper_started(42, 350);
  SYNC_REQUIRE(!model.observe_health_failure(5'349));
  SYNC_REQUIRE(model.observe_health_failure(5'350));
  SYNC_REQUIRE(!model.observe_health_failure(5'351));

  const auto second = model.helper_exited(9, false, 5'400);
  SYNC_REQUIRE(second.has_value());
  SYNC_REQUIRE(second->attempt == 2);
  const std::uint64_t stale_generation = second->generation;
  model.cancel_recovery();
  SYNC_REQUIRE(model.recovery_generation() != stale_generation);
  SYNC_REQUIRE(!model.begin_recovery_attempt(*second, second->due_ms));

  model.begin_start();
  model.helper_started(50);
  SYNC_REQUIRE(!model.helper_exited(0, true, 10'000).has_value());
  SYNC_REQUIRE(model.state() == CompanionState::Stopped);
}

SYNC_TEST(companion_model_recovery_preflight_never_replaces_a_listener) {
  CompanionModel external("0.2.7");
  external.begin_start();
  external.helper_started(41);
  const auto external_attempt = external.helper_exited(70, false, 100);
  SYNC_REQUIRE(external_attempt.has_value());
  SYNC_REQUIRE(external.begin_recovery_attempt(*external_attempt, 350));
  const std::uint64_t external_generation = external.recovery_generation();
  external.observe_health(healthy(), 400);
  SYNC_REQUIRE(external.state() == CompanionState::External);
  SYNC_REQUIRE(!external.recovery_active());
  SYNC_REQUIRE(external.recovery_generation() != external_generation);

  CompanionModel conflict("0.2.7");
  conflict.begin_start();
  conflict.helper_started(51);
  const auto conflict_attempt = conflict.helper_exited(70, false, 100);
  SYNC_REQUIRE(conflict_attempt.has_value());
  SYNC_REQUIRE(conflict.begin_recovery_attempt(*conflict_attempt, 350));
  auto incompatible = healthy();
  incompatible.compatible = false;
  const auto retry = conflict.recovery_preflight_conflict(
      std::move(incompatible), 400);
  SYNC_REQUIRE(retry.has_value());
  SYNC_REQUIRE(retry->attempt == 2);
  SYNC_REQUIRE(retry->due_ms == 1'400);
  SYNC_REQUIRE(conflict.state() == CompanionState::Recovering);
  SYNC_REQUIRE(conflict.recovery_active());
  SYNC_REQUIRE(conflict.begin_recovery_attempt(*retry, 1'400));
  auto second_incompatible = healthy();
  second_incompatible.compatible = false;
  const auto final_retry = conflict.recovery_preflight_conflict(
      std::move(second_incompatible), 1'450);
  SYNC_REQUIRE(final_retry.has_value());
  SYNC_REQUIRE(final_retry->attempt == 3);
  SYNC_REQUIRE(final_retry->due_ms == 5'450);
  SYNC_REQUIRE(conflict.begin_recovery_attempt(*final_retry, 5'450));
  auto final_incompatible = healthy();
  final_incompatible.compatible = false;
  SYNC_REQUIRE(!conflict.recovery_preflight_conflict(
                    std::move(final_incompatible), 5'500)
                    .has_value());
  SYNC_REQUIRE(conflict.state() == CompanionState::RecoveryExhausted);
}

SYNC_TEST(companion_model_bounds_stderr_and_emits_safe_diagnostics) {
  CompanionModel model("0.2.7");
  std::string noise(70 * 1024, 'a');
  noise.replace(0, 10, "discarded!");
  model.append_stderr(noise);
  SYNC_REQUIRE(model.recent_stderr().size() == CompanionModel::kMaximumStderrBytes);
  SYNC_REQUIRE(model.recent_stderr().find("discarded!") == std::string::npos);

  model.helper_started(42);
  model.observe_health(healthy(3), 100);
  const std::string diagnostics = model.diagnostics();
  SYNC_REQUIRE(diagnostics.find("Sync 0.2.7") != std::string::npos);
  SYNC_REQUIRE(diagnostics.find("State: Running") != std::string::npos);
  SYNC_REQUIRE(diagnostics.find("Active senders: 3") != std::string::npos);
  SYNC_REQUIRE(diagnostics.find("token") == std::string::npos);
  SYNC_REQUIRE(diagnostics.find("origin") == std::string::npos);
}

} // namespace
