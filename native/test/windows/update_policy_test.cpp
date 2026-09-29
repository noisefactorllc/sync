#include "test_harness.hpp"
#include "../../src/platform/windows/update_policy.hpp"

using namespace noisefactor::sync::windows_update;

namespace {
Configuration valid_configuration() {
  return {"https://downloads.noisefactor.io/updates/sync/preview/windows-x64.xml",
          "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=",
          std::string(64, 'a'), std::string(64, 'b')};
}
}

SYNC_TEST(windows_update_requires_all_trust_inputs) {
  auto config = valid_configuration();
  SYNC_REQUIRE(configuration_error(config).empty());
  config.eddsa_public_key.clear();
  SYNC_REQUIRE(!configuration_error(config).empty());
  config = valid_configuration();
  config.publisher_sha256.clear();
  SYNC_REQUIRE(!configuration_error(config).empty());
  config = valid_configuration();
  config.framework_sha256.clear();
  SYNC_REQUIRE(!configuration_error(config).empty());
}

SYNC_TEST(windows_update_feed_is_exact_preview_origin_and_target) {
  auto config = valid_configuration();
  for (const auto* feed : {
      "http://downloads.noisefactor.io/updates/sync/preview/windows-x64.xml",
      "https://downloads.noisefactor.io.evil.test/updates/sync/preview/windows-x64.xml",
      "https://downloads.noisefactor.io@evil.test/updates/sync/preview/windows-x64.xml",
      "https://downloads.noisefactor.io/updates/sync/stable/windows-x64.xml",
      "https://downloads.noisefactor.io/updates/sync/preview/macos.xml",
      "https://downloads.noisefactor.io/updates/sync/preview/windows-x64.xml?override=1"}) {
    config.feed_url = feed;
    SYNC_REQUIRE(!configuration_error(config).empty());
  }
}

SYNC_TEST(windows_update_rejects_invalid_trust_encodings) {
  auto config = valid_configuration();
  config.eddsa_public_key = "not-a-public-key";
  SYNC_REQUIRE(!configuration_error(config).empty());
  config = valid_configuration();
  config.eddsa_public_key = std::string(43, 'A') + '=';
  SYNC_REQUIRE(!configuration_error(config).empty());
  config = valid_configuration();
  config.publisher_sha256[0] = 'z';
  SYNC_REQUIRE(!configuration_error(config).empty());
  config = valid_configuration();
  config.framework_sha256.resize(63);
  SYNC_REQUIRE(!configuration_error(config).empty());
}

SYNC_TEST(windows_update_digest_match_rejects_empty_partial_and_wrong_pins) {
  const std::string lower(64, 'a');
  const std::string upper(64, 'A');
  SYNC_REQUIRE(digest_matches(lower, upper));
  SYNC_REQUIRE(!digest_matches("", ""));
  SYNC_REQUIRE(!digest_matches(lower, lower.substr(0, 63)));
  SYNC_REQUIRE(!digest_matches(lower, std::string(64, 'b')));
}


SYNC_TEST(windows_update_schedules_at_the_daily_boundary) {
  CheckSchedule schedule;
  SYNC_REQUIRE(schedule.automatic_due(100, true));
  schedule.last_check = 100;
  SYNC_REQUIRE(!schedule.automatic_due(86499, true));
  SYNC_REQUIRE(schedule.automatic_due(86500, true));
  schedule.enabled = false;
  SYNC_REQUIRE(!schedule.automatic_due(86500, true));
}

SYNC_TEST(windows_update_pause_expires_and_resume_keeps_cadence) {
  CheckSchedule schedule;
  schedule.last_check = 100;
  schedule.pause_for_day(200);
  SYNC_REQUIRE(schedule.paused(86599));
  SYNC_REQUIRE(!schedule.automatic_due(86599, true));
  SYNC_REQUIRE(!schedule.paused(86600));
  SYNC_REQUIRE(schedule.automatic_due(86600, true));
  schedule.pause_for_day(300);
  schedule.resume();
  SYNC_REQUIRE(!schedule.paused(301));
  SYNC_REQUIRE(!schedule.automatic_due(301, true));
}

SYNC_TEST(windows_update_indefinite_pause_and_manual_check_survive_restart) {
  CheckSchedule schedule;
  schedule.pause_indefinitely();
  SYNC_REQUIRE(!schedule.automatic_due(1'000'000, true));
  // Manual checks update the cadence without changing the persisted pause.
  schedule.last_check = 1'000'000;
  CheckSchedule restarted = schedule;
  SYNC_REQUIRE(restarted.paused(2'000'000));
  restarted.resume();
  SYNC_REQUIRE(restarted.automatic_due(2'000'000, true));
}

SYNC_TEST(windows_update_clock_rollback_does_not_trigger_a_check_storm) {
  CheckSchedule schedule;
  schedule.last_check = 100000;
  SYNC_REQUIRE(!schedule.automatic_due(99999, true));
  SYNC_REQUIRE(!schedule.automatic_due(100000, true));
}

SYNC_TEST(windows_update_never_schedules_without_metadata_only_support) {
  CheckSchedule schedule;
  SYNC_REQUIRE(!schedule.automatic_due(100));
  schedule.enabled = true;
  schedule.last_check = 100;
  SYNC_REQUIRE(!schedule.automatic_due(86500, false));
  schedule.pause_indefinitely();
  schedule.resume();
  SYNC_REQUIRE(!schedule.automatic_due(86500));
}
