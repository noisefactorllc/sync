#include "../test_harness.hpp"

#include <sync/platform/companion_model.hpp>

#include <optional>
#include <string>

namespace {

using noisefactor::sync::companion::CompanionModel;
using noisefactor::sync::companion::CompanionState;
using noisefactor::sync::companion::HealthSnapshot;

HealthSnapshot healthy(std::size_t senders = 0) {
  return {
      .reachable = true,
      .compatible = true,
      .product = "Sync",
      .version = "0.2.7",
      .syphon_available = true,
      .active_senders = senders,
  };
}

SYNC_TEST(companion_model_tracks_owned_and_external_health) {
  CompanionModel model("0.2.7");
  SYNC_REQUIRE(model.state() == CompanionState::Stopped);

  model.begin_start();
  SYNC_REQUIRE(model.state() == CompanionState::Starting);
  model.helper_started(42);
  model.observe_health(healthy(2));
  SYNC_REQUIRE(model.state() == CompanionState::Running);
  SYNC_REQUIRE(model.owned_pid() == std::optional<int>(42));
  SYNC_REQUIRE(model.health().active_senders == std::optional<std::size_t>(2));

  CompanionModel external("0.2.7");
  external.observe_health(healthy(1));
  SYNC_REQUIRE(external.state() == CompanionState::External);
  SYNC_REQUIRE(!external.owned_pid().has_value());
}

SYNC_TEST(companion_model_distinguishes_startup_failure_conflict_and_exit) {
  CompanionModel model("0.2.7");
  model.begin_start();
  model.helper_started(99);
  model.observe_health_failure();
  SYNC_REQUIRE(model.state() == CompanionState::Starting);

  auto incompatible = healthy();
  incompatible.compatible = false;
  incompatible.version = "9.0.0";
  model.observe_health(std::move(incompatible));
  SYNC_REQUIRE(model.state() == CompanionState::PortConflict);

  model.helper_exited(70, false);
  SYNC_REQUIRE(model.state() == CompanionState::Failed);
  SYNC_REQUIRE(model.last_exit_status() == std::optional<int>(70));
  SYNC_REQUIRE(!model.owned_pid().has_value());

  model.manual_restart();
  SYNC_REQUIRE(model.state() == CompanionState::Starting);
  SYNC_REQUIRE(!model.last_exit_status().has_value());
  model.helper_started(100);
  model.helper_exited(0, true);
  SYNC_REQUIRE(model.state() == CompanionState::Stopped);
}

SYNC_TEST(companion_model_bounds_stderr_and_emits_safe_diagnostics) {
  CompanionModel model("0.2.7");
  std::string noise(70 * 1024, 'a');
  noise.replace(0, 10, "discarded!");
  model.append_stderr(noise);
  SYNC_REQUIRE(model.recent_stderr().size() == CompanionModel::kMaximumStderrBytes);
  SYNC_REQUIRE(model.recent_stderr().find("discarded!") == std::string::npos);

  model.helper_started(42);
  model.observe_health(healthy(3));
  const std::string diagnostics = model.diagnostics();
  SYNC_REQUIRE(diagnostics.find("Sync 0.2.7") != std::string::npos);
  SYNC_REQUIRE(diagnostics.find("State: Running") != std::string::npos);
  SYNC_REQUIRE(diagnostics.find("Active senders: 3") != std::string::npos);
  SYNC_REQUIRE(diagnostics.find("token") == std::string::npos);
  SYNC_REQUIRE(diagnostics.find("origin") == std::string::npos);
}

} // namespace
