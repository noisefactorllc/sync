#include "test_harness.hpp"

#include "../src/audio_probe_report.hpp"

#include <string>
#include <string_view>

namespace probe = noisefactor::sync::audio::probe;

namespace {

SYNC_TEST(probe_failure_class_names_are_stable_strings) {
  // The PowerShell driver parses these exact field values; renaming one is a
  // protocol change for scripts/test-windows-audio.ps1, not a local edit.
  SYNC_REQUIRE(probe::failure_class_name(probe::FailureClass::kQualified) ==
               std::string("qualified"));
  SYNC_REQUIRE(probe::failure_class_name(probe::FailureClass::kStartup) ==
               std::string("startup"));
  SYNC_REQUIRE(probe::failure_class_name(probe::FailureClass::kIntegrity) ==
               std::string("integrity"));
}

SYNC_TEST(probe_classifies_unqualified_captures_as_startup_or_integrity) {
  // No format and no frames: the endpoint never delivered anything, so the
  // attempt is retryable startup behavior.
  SYNC_REQUIRE(probe::classify_failure(false, false, 0) == probe::FailureClass::kStartup);
  // A format but no frames: the stream opened and produced nothing.
  SYNC_REQUIRE(probe::classify_failure(false, true, 0) == probe::FailureClass::kStartup);
  // Frames were produced and a check failed: the failure is substantive.
  SYNC_REQUIRE(probe::classify_failure(false, true, 480) ==
               probe::FailureClass::kIntegrity);
  // A qualified capture is never a failure.
  SYNC_REQUIRE(probe::classify_failure(true, true, 480) == probe::FailureClass::kQualified);
  SYNC_REQUIRE(probe::classify_failure(true, false, 0) == probe::FailureClass::kQualified);
}

SYNC_TEST(probe_never_qualifies_a_capture_that_threw_partway_through) {
  // A capture whose read() threw partway through the window (driver error,
  // input overflow past the first buffer, stream not running) is partial
  // whatever the statistics say: it can never report qualified on less than
  // the full window, and the thrown text reaches the report's failure field.
  SYNC_REQUIRE(probe::capture_qualified(true, std::string_view()) == true);
  SYNC_REQUIRE(probe::capture_qualified(true, std::string_view("")) == true);
  SYNC_REQUIRE(probe::capture_qualified(true,
      std::string_view("Native audio capture stopped: stream not running")) == false);
  SYNC_REQUIRE(probe::capture_qualified(false, std::string_view()) == false);
  SYNC_REQUIRE(probe::capture_qualified(false,
      std::string_view("Native audio capture stopped: driver error")) == false);
}

}  // namespace
