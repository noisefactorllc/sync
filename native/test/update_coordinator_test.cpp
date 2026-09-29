#include "test_harness.hpp"
#include <sync/update_coordinator.hpp>
#include <atomic>
#include <thread>

using noisefactor::sync::update::Coordinator;
namespace { constexpr auto digest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"; }

SYNC_TEST(update_unknown_and_active_work_never_reserve) {
  Coordinator c;
  SYNC_REQUIRE(!c.reserve(digest, 100000));
  c.set_external_state(true, 100000);
  SYNC_REQUIRE(c.begin_activity(100001));
  SYNC_REQUIRE(!c.reserve(digest, 200000));
  c.end_activity(200000);
  SYNC_REQUIRE(!c.reserve(digest, 259999));
  SYNC_REQUIRE(c.reserve(digest, 260000).has_value());
}
SYNC_TEST(update_reservation_excludes_admission_and_requires_exact_identity) {
  Coordinator c; c.set_external_state(true, 1);
  auto token = c.reserve(digest, 60001);
  SYNC_REQUIRE(token.has_value());
  SYNC_REQUIRE(!c.begin_activity(60002));
  SYNC_REQUIRE(!c.commit(*token + 1, digest, 60002));
  SYNC_REQUIRE(!c.commit(*token, std::string(64, 'a'), 60002));
  SYNC_REQUIRE(c.commit(*token, digest, 60002));
  SYNC_REQUIRE(!c.begin_activity(900000));
  SYNC_REQUIRE(!c.cancel(*token + 1, 900000));
  SYNC_REQUIRE(c.cancel(*token, 900000));
  SYNC_REQUIRE(c.begin_activity(900001));
}
SYNC_TEST(update_preparation_expiry_and_clock_reversal_fail_closed) {
  Coordinator c; c.set_external_state(true, 100);
  auto token = c.reserve(digest, 60100); SYNC_REQUIRE(token.has_value());
  SYNC_REQUIRE(!c.commit(*token, digest, 90100));
  SYNC_REQUIRE(c.begin_activity(90101)); c.end_activity(90102);
  SYNC_REQUIRE(!c.reserve(digest, 80000));
  SYNC_REQUIRE(!c.reserve(digest, 140000));
  SYNC_REQUIRE(c.reserve(digest, 150102).has_value());
}
SYNC_TEST(update_invalid_digest_and_unknown_transition_cannot_apply) {
  Coordinator c; c.set_external_state(true, 0);
  SYNC_REQUIRE(!c.reserve("bad", 60000));
  SYNC_REQUIRE(!c.reserve(std::string(64, 'A'), 60000));
  auto token = c.reserve(digest, 60000); SYNC_REQUIRE(token.has_value());
  c.set_external_state(false, 60001);
  SYNC_REQUIRE(!c.commit(*token, digest, 60002));
  SYNC_REQUIRE(!c.cancel(*token, 60003));
  SYNC_REQUIRE(!c.reserve(digest, 900000));
}
SYNC_TEST(update_repeated_idle_observation_does_not_reset_idle_clock) {
  Coordinator c; c.set_external_state(true, 5);
  for (unsigned i=10;i<60005;i+=10) c.set_external_state(true,i);
  SYNC_REQUIRE(c.reserve(digest,60005).has_value());
}
SYNC_TEST(update_reservation_and_admission_are_mutually_exclusive) {
  for (unsigned n=0;n<100;++n) {
    Coordinator c; c.set_external_state(true,0);
    std::atomic<bool> start=false; bool admitted=false; bool reserved=false;
    std::thread a([&]{while(!start.load()){} admitted=c.begin_activity(60000);});
    std::thread b([&]{while(!start.load()){} reserved=c.reserve(digest,60000).has_value();});
    start=true;a.join();b.join();
    SYNC_REQUIRE(admitted != reserved);
  }
}
SYNC_TEST(update_busy_transition_invalidates_uncommitted_idle_proof) {
  Coordinator c; c.set_external_state(true, 0);
  const auto token = c.reserve(digest, 60000);
  SYNC_REQUIRE(token.has_value());
  c.set_external_state(false, 60001);
  c.set_external_state(true, 60002);
  SYNC_REQUIRE(!c.commit(*token, digest, 60003));
  SYNC_REQUIRE(!c.reserve(digest, 120001));
  SYNC_REQUIRE(c.reserve(digest, 120002).has_value());
}
SYNC_TEST(update_older_reading_from_another_thread_is_ordered_not_refused) {
  // The control service can observe a later reading before the daemon loop
  // takes the mutex with an earlier one. That must not refuse a browser or
  // drop the idle proof for the rest of the daemon's life.
  Coordinator c; c.set_external_state(true, 0);
  SYNC_REQUIRE(c.ready(60001));
  SYNC_REQUIRE(c.begin_activity(60000));
  c.end_activity(60000);
  SYNC_REQUIRE(!c.ready(60001));
  SYNC_REQUIRE(!c.reserve(digest, 120000));
  SYNC_REQUIRE(c.ready(120001));
  SYNC_REQUIRE(c.reserve(digest, 120001).has_value());
}
SYNC_TEST(update_readiness_probe_never_reserves_or_delays_admission) {
  Coordinator c;
  SYNC_REQUIRE(!c.ready(0));
  c.set_external_state(true, 0);
  SYNC_REQUIRE(!c.ready(59999));
  SYNC_REQUIRE(c.ready(60000));
  SYNC_REQUIRE(c.ready(60001));
  SYNC_REQUIRE(c.begin_activity(60002));
  SYNC_REQUIRE(!c.ready(60003));
}
