#include "test_harness.hpp"

#include <string>
#include <vector>

#include "../src/owner_dispatch_queue.hpp"

namespace {

using noisefactor::sync::companion::OwnerDispatchQueue;

}  // namespace

SYNC_TEST(owner_dispatch_queue_runs_queued_callbacks_in_order_on_drain) {
  OwnerDispatchQueue queue;
  std::vector<std::string> ran;
  int wakes = 0;
  queue.dispatch([&] { ran.push_back("first"); }, [&] { ++wakes; return true; });
  queue.dispatch([&] { ran.push_back("second"); }, [&] { ++wakes; return true; });
  SYNC_REQUIRE(ran.empty());
  SYNC_REQUIRE(wakes == 2);
  SYNC_REQUIRE(queue.pending() == 2);
  queue.drain();
  SYNC_REQUIRE((ran == std::vector<std::string>{"first", "second"}));
  SYNC_REQUIRE(queue.pending() == 0);
}

SYNC_TEST(owner_dispatch_queue_runs_inline_when_the_wake_cannot_be_posted) {
  OwnerDispatchQueue queue;
  bool ran = false;
  queue.dispatch([&] { ran = true; }, [] { return false; });
  SYNC_REQUIRE(ran);
  SYNC_REQUIRE(queue.pending() == 0);
  queue.drain();
  SYNC_REQUIRE(ran);
}

SYNC_TEST(owner_dispatch_queue_runs_inline_once_the_owner_is_gone) {
  OwnerDispatchQueue queue;
  int wakes = 0;
  std::vector<int> ran;
  queue.dispatch([&] { ran.push_back(1); }, [&] { ++wakes; return true; });
  queue.mark_owner_gone();
  SYNC_REQUIRE(queue.owner_gone());
  // Queued before the owner left: still run by the final drain.
  queue.drain();
  SYNC_REQUIRE((ran == std::vector<int>{1}));
  // Dispatched after: runs now, never wakes anyone, leaves nothing behind.
  queue.dispatch([&] { ran.push_back(2); }, [&] { ++wakes; return true; });
  SYNC_REQUIRE((ran == std::vector<int>{1, 2}));
  SYNC_REQUIRE(wakes == 1);
  SYNC_REQUIRE(queue.pending() == 0);
}

SYNC_TEST(owner_dispatch_queue_drains_callbacks_queued_while_draining) {
  OwnerDispatchQueue queue;
  std::vector<int> ran;
  queue.dispatch(
      [&] {
        ran.push_back(1);
        queue.dispatch([&] { ran.push_back(2); }, [] { return true; });
      },
      [] { return true; });
  queue.drain();
  SYNC_REQUIRE((ran == std::vector<int>{1, 2}));
  SYNC_REQUIRE(queue.pending() == 0);
}
