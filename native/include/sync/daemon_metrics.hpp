#pragma once

#include <atomic>
#include <cstdint>

namespace noisefactor::sync {

enum class ReceiveStatus;

struct DaemonMetricsSnapshot {
  std::uint64_t received_frames = 0;
  std::uint64_t accepted_frames = 0;
  std::uint64_t dropped_frames = 0;
  std::uint64_t rejected_frames = 0;
  std::uint64_t failed_frames = 0;
  std::uint64_t camera_driving_frames = 0;
  std::uint64_t camera_writes = 0;
  std::uint64_t camera_queue_replacements = 0;
  std::uint64_t camera_backpressure_drops = 0;
  std::uint64_t camera_write_failures = 0;
  std::uint64_t camera_reopen_attempts = 0;
  std::uint64_t camera_idle_frames = 0;
  std::uint64_t camera_last_write_ms = 0;
};

class DaemonMetrics {
 public:
  void note_receive(ReceiveStatus status) noexcept;
  void note_camera_driving_frame() noexcept;
  void note_camera_write(std::uint64_t now_ms, bool idle) noexcept;
  void note_camera_queue_replacement() noexcept;
  void note_camera_backpressure() noexcept;
  void note_camera_write_failure() noexcept;
  void note_camera_reopen_attempt() noexcept;
  [[nodiscard]] auto snapshot() const noexcept -> DaemonMetricsSnapshot;

 private:
  std::atomic<std::uint64_t> received_frames_{0};
  std::atomic<std::uint64_t> accepted_frames_{0};
  std::atomic<std::uint64_t> dropped_frames_{0};
  std::atomic<std::uint64_t> rejected_frames_{0};
  std::atomic<std::uint64_t> failed_frames_{0};
  std::atomic<std::uint64_t> camera_driving_frames_{0};
  std::atomic<std::uint64_t> camera_writes_{0};
  std::atomic<std::uint64_t> camera_queue_replacements_{0};
  std::atomic<std::uint64_t> camera_backpressure_drops_{0};
  std::atomic<std::uint64_t> camera_write_failures_{0};
  std::atomic<std::uint64_t> camera_reopen_attempts_{0};
  std::atomic<std::uint64_t> camera_idle_frames_{0};
  std::atomic<std::uint64_t> camera_last_write_ms_{0};
};

}  // namespace noisefactor::sync
