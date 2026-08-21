#include "test_harness.hpp"

#include "../src/companion_management.hpp"

#include <string>

namespace {

namespace companion = noisefactor::sync::companion;

SYNC_TEST(revocation_completion_is_inert_after_shutdown_starts) {
  int error_calls = 0;
  int refresh_calls = 0;
  companion::complete_revocation(
      true, false, "late failure",
      [&](std::string_view) { ++error_calls; }, [&] { ++refresh_calls; });
  SYNC_REQUIRE(error_calls == 0);
  SYNC_REQUIRE(refresh_calls == 0);
}

SYNC_TEST(revocation_completion_reports_failure_then_refreshes_while_running) {
  std::string error;
  int refresh_calls = 0;
  companion::complete_revocation(
      false, false, "store unavailable",
      [&](std::string_view message) { error = message; },
      [&] { ++refresh_calls; });
  SYNC_REQUIRE(error == "store unavailable");
  SYNC_REQUIRE(refresh_calls == 1);
}

SYNC_TEST(successful_revocation_refreshes_without_an_error) {
  int error_calls = 0;
  int refresh_calls = 0;
  companion::complete_revocation(
      false, true, {}, [&](std::string_view) { ++error_calls; },
      [&] { ++refresh_calls; });
  SYNC_REQUIRE(error_calls == 0);
  SYNC_REQUIRE(refresh_calls == 1);
}

}  // namespace
