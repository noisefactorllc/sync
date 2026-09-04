#include <sync/daemon_metrics.hpp>

#include <sync/frame_receiver.hpp>

namespace noisefactor::sync {

void DaemonMetrics::note_receive(ReceiveStatus status) noexcept {
  received_frames_.fetch_add(1, std::memory_order_relaxed);
  switch (status) {
    case ReceiveStatus::Accepted:
      accepted_frames_.fetch_add(1, std::memory_order_relaxed);
      break;
    case ReceiveStatus::DroppedBackpressure:
    case ReceiveStatus::DroppedStale:
      dropped_frames_.fetch_add(1, std::memory_order_relaxed);
      break;
    case ReceiveStatus::RejectedMalformed:
    case ReceiveStatus::RejectedSender:
      rejected_frames_.fetch_add(1, std::memory_order_relaxed);
      break;
    case ReceiveStatus::PublishFailed:
      failed_frames_.fetch_add(1, std::memory_order_relaxed);
      break;
  }
}

void DaemonMetrics::note_camera_driving_frame() noexcept {
  camera_driving_frames_.fetch_add(1, std::memory_order_relaxed);
}

void DaemonMetrics::note_camera_write(std::uint64_t now_ms,
                                      bool idle) noexcept {
  camera_writes_.fetch_add(1, std::memory_order_relaxed);
  if (idle) camera_idle_frames_.fetch_add(1, std::memory_order_relaxed);
  std::uint64_t previous =
      camera_last_write_ms_.load(std::memory_order_relaxed);
  while (previous < now_ms &&
         !camera_last_write_ms_.compare_exchange_weak(
             previous, now_ms, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

void DaemonMetrics::note_camera_queue_replacement() noexcept {
  camera_queue_replacements_.fetch_add(1, std::memory_order_relaxed);
}

void DaemonMetrics::note_camera_backpressure() noexcept {
  camera_backpressure_drops_.fetch_add(1, std::memory_order_relaxed);
}

void DaemonMetrics::note_camera_write_failure() noexcept {
  camera_write_failures_.fetch_add(1, std::memory_order_relaxed);
}

void DaemonMetrics::note_camera_reopen_attempt() noexcept {
  camera_reopen_attempts_.fetch_add(1, std::memory_order_relaxed);
}

auto DaemonMetrics::snapshot() const noexcept -> DaemonMetricsSnapshot {
  return {
      .received_frames = received_frames_.load(std::memory_order_relaxed),
      .accepted_frames = accepted_frames_.load(std::memory_order_relaxed),
      .dropped_frames = dropped_frames_.load(std::memory_order_relaxed),
      .rejected_frames = rejected_frames_.load(std::memory_order_relaxed),
      .failed_frames = failed_frames_.load(std::memory_order_relaxed),
      .camera_driving_frames =
          camera_driving_frames_.load(std::memory_order_relaxed),
      .camera_writes = camera_writes_.load(std::memory_order_relaxed),
      .camera_queue_replacements =
          camera_queue_replacements_.load(std::memory_order_relaxed),
      .camera_backpressure_drops =
          camera_backpressure_drops_.load(std::memory_order_relaxed),
      .camera_write_failures =
          camera_write_failures_.load(std::memory_order_relaxed),
      .camera_reopen_attempts =
          camera_reopen_attempts_.load(std::memory_order_relaxed),
      .camera_idle_frames =
          camera_idle_frames_.load(std::memory_order_relaxed),
      .camera_last_write_ms =
          camera_last_write_ms_.load(std::memory_order_relaxed),
  };
}

}  // namespace noisefactor::sync
