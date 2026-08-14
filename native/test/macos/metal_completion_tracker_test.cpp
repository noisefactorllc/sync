#include "../test_harness.hpp"

#include "../../src/platform/macos/metal_completion_tracker.hpp"

#include <cstdint>
#include <memory>

namespace noisefactor::sync {
namespace {

SYNC_TEST(metal_completion_tracker_times_out_at_the_exact_watchdog_boundary) {
  auto latch = std::make_shared<detail::MetalFailureLatch>();
  detail::MetalCompletionTracker tracker(latch);
  SYNC_REQUIRE(tracker.try_begin(10'000));
  SYNC_REQUIRE(!tracker.available());
  SYNC_REQUIRE(!tracker.poll_watchdog(10'999, 1'000).has_value());

  const auto failure = tracker.poll_watchdog(11'000, 1'000);
  SYNC_REQUIRE(failure.has_value());
  SYNC_REQUIRE(failure->kind ==
               ProviderFailureKind::MetalWatchdogTimeout);
  SYNC_REQUIRE(!tracker.available());

  tracker.complete_success();
  SYNC_REQUIRE(!tracker.available());
  SYNC_REQUIRE(latch->failure()->kind ==
               ProviderFailureKind::MetalWatchdogTimeout);
}

SYNC_TEST(metal_failure_latch_keeps_the_first_terminal_error) {
  auto latch = std::make_shared<detail::MetalFailureLatch>();
  detail::MetalCompletionTracker first(latch);
  detail::MetalCompletionTracker second(latch);
  SYNC_REQUIRE(first.try_begin(100));
  SYNC_REQUIRE(second.try_begin(100));
  first.complete_failure(5, -9);
  second.complete_failure(4, -7);

  const auto failure = latch->failure();
  SYNC_REQUIRE(failure.has_value());
  SYNC_REQUIRE(failure->kind == ProviderFailureKind::MetalCommandFailed);
  SYNC_REQUIRE(failure->native_status == 5);
  SYNC_REQUIRE(failure->native_error_code == -9);
  SYNC_REQUIRE(latch->failed());
}

SYNC_TEST(metal_completion_tracker_success_releases_and_reuses_one_slot) {
  auto latch = std::make_shared<detail::MetalFailureLatch>();
  detail::MetalCompletionTracker tracker(latch);
  SYNC_REQUIRE(tracker.available());
  SYNC_REQUIRE(tracker.try_begin(1));
  SYNC_REQUIRE(!tracker.try_begin(2));
  tracker.complete_success();
  SYNC_REQUIRE(tracker.available());
  SYNC_REQUIRE(!latch->failure().has_value());
  SYNC_REQUIRE(tracker.try_begin(3));
  tracker.complete_success();
  SYNC_REQUIRE(tracker.available());
}

SYNC_TEST(metal_completion_tracker_cancels_only_before_terminal_state) {
  auto latch = std::make_shared<detail::MetalFailureLatch>();
  detail::MetalCompletionTracker cancelled(latch);
  SYNC_REQUIRE(cancelled.try_begin(1));
  cancelled.cancel_before_commit();
  SYNC_REQUIRE(cancelled.available());
  SYNC_REQUIRE(!latch->failed());

  detail::MetalCompletionTracker failed(latch);
  SYNC_REQUIRE(failed.try_begin(2));
  failed.complete_failure(5, 17);
  failed.cancel_before_commit();
  SYNC_REQUIRE(!failed.available());
  const auto failure = latch->failure();
  SYNC_REQUIRE(failure.has_value());
  SYNC_REQUIRE(failure->native_error_code == 17);
}

}  // namespace
}  // namespace noisefactor::sync
