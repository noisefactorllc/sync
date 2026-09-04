#include "../test_harness.hpp"

#include <sync/daemon_metrics.hpp>
#include <sync/frame_receiver.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

namespace {

using noisefactor::sync::DaemonMetrics;
using noisefactor::sync::ReceiveStatus;

SYNC_TEST(daemon_metrics_classifies_each_receive_result_once) {
  DaemonMetrics metrics;
  metrics.note_receive(ReceiveStatus::Accepted);
  metrics.note_receive(ReceiveStatus::DroppedBackpressure);
  metrics.note_receive(ReceiveStatus::DroppedStale);
  metrics.note_receive(ReceiveStatus::RejectedMalformed);
  metrics.note_receive(ReceiveStatus::RejectedSender);
  metrics.note_receive(ReceiveStatus::PublishFailed);

  const auto snapshot = metrics.snapshot();
  SYNC_REQUIRE(snapshot.received_frames == 6);
  SYNC_REQUIRE(snapshot.accepted_frames == 1);
  SYNC_REQUIRE(snapshot.dropped_frames == 2);
  SYNC_REQUIRE(snapshot.rejected_frames == 2);
  SYNC_REQUIRE(snapshot.failed_frames == 1);
}

SYNC_TEST(daemon_metrics_count_camera_events_and_keep_latest_write_monotonic) {
  DaemonMetrics metrics;
  metrics.note_camera_driving_frame();
  metrics.note_camera_write(40, false);
  metrics.note_camera_write(20, true);
  metrics.note_camera_queue_replacement();
  metrics.note_camera_backpressure();
  metrics.note_camera_write_failure();
  metrics.note_camera_reopen_attempt();

  const auto snapshot = metrics.snapshot();
  SYNC_REQUIRE(snapshot.camera_driving_frames == 1);
  SYNC_REQUIRE(snapshot.camera_writes == 2);
  SYNC_REQUIRE(snapshot.camera_queue_replacements == 1);
  SYNC_REQUIRE(snapshot.camera_backpressure_drops == 1);
  SYNC_REQUIRE(snapshot.camera_write_failures == 1);
  SYNC_REQUIRE(snapshot.camera_reopen_attempts == 1);
  SYNC_REQUIRE(snapshot.camera_idle_frames == 1);
  SYNC_REQUIRE(snapshot.camera_last_write_ms == 40);
}

SYNC_TEST(daemon_metrics_snapshots_are_monotonic_during_concurrent_writes) {
  DaemonMetrics metrics;
  std::atomic<bool> running = true;
  constexpr std::uint64_t iterations = 20'000;
  std::thread writer([&] {
    for (std::uint64_t index = 1; index <= iterations; ++index) {
      metrics.note_receive(ReceiveStatus::Accepted);
      metrics.note_camera_write(index, false);
    }
    running.store(false, std::memory_order_release);
  });

  std::uint64_t received = 0;
  std::uint64_t writes = 0;
  std::uint64_t last_write = 0;
  while (running.load(std::memory_order_acquire)) {
    const auto snapshot = metrics.snapshot();
    SYNC_REQUIRE(snapshot.received_frames >= received);
    SYNC_REQUIRE(snapshot.camera_writes >= writes);
    SYNC_REQUIRE(snapshot.camera_last_write_ms >= last_write);
    received = snapshot.received_frames;
    writes = snapshot.camera_writes;
    last_write = snapshot.camera_last_write_ms;
  }
  writer.join();

  const auto final = metrics.snapshot();
  SYNC_REQUIRE(final.received_frames == iterations);
  SYNC_REQUIRE(final.accepted_frames == iterations);
  SYNC_REQUIRE(final.camera_writes == iterations);
  SYNC_REQUIRE(final.camera_last_write_ms == iterations);
}

}  // namespace
