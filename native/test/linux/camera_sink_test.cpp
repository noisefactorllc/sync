#include "../test_harness.hpp"

#include <sync/camera/nv12.hpp>
#include <sync/daemon_metrics.hpp>
#include <sync/platform/camera_identity.hpp>
#include <sync/platform/linux_camera_sink.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace camera = noisefactor::sync::camera;

std::atomic<bool> wall_clock_enabled{true};

std::uint64_t controlled_clock_ms() noexcept {
  if (!wall_clock_enabled.load(std::memory_order_relaxed)) return 0;
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

class DeviceOps final : public camera::LinuxCameraDeviceOps {
 public:
  DeviceOps() {
    std::copy_n("v4l2 loopback", 13, capabilities.driver);
    std::copy_n("Sync Camera", 11, capabilities.card);
    capabilities.capabilities = V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_READWRITE;
  }

  auto enumerate(std::span<std::array<char, 64>> output) noexcept
      -> std::size_t override {
    if (!discoverable) return 0;
    constexpr std::string_view path = "/dev/video9";
    std::copy(path.begin(), path.end(), output[0].begin());
    return 1;
  }
  auto open_no_follow(std::string_view) noexcept -> int override {
    std::lock_guard lock(mutex);
    ++open_calls;
    if (!discoverable) {
      errno = ENOENT;
      return -1;
    }
    return next_descriptor++;
  }
  auto validate_character_device(int) noexcept -> bool override {
    return character_device;
  }
  auto query_capabilities(int, v4l2_capability& output) noexcept
      -> int override {
    output = capabilities;
    return query_result;
  }
  auto set_nv12_format(int, v4l2_format& format) noexcept -> int override {
    if (format_result < 0) {
      errno = EINVAL;
      return -1;
    }
    format.fmt.pix.width = camera::kCanvas.width;
    format.fmt.pix.height = camera::kCanvas.height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    format.fmt.pix.bytesperline = camera::kCanvas.width;
    format.fmt.pix.sizeimage = static_cast<std::uint32_t>(camera::nv12_size_bytes(
        camera::kCanvas.width, camera::kCanvas.height, camera::kCanvas.width));
    return 0;
  }
  auto set_frame_rate(int, v4l2_streamparm&) noexcept -> int override {
    return 0;
  }
  auto write_frame(int, std::span<const std::byte> frame) noexcept
      -> std::pair<std::ptrdiff_t, std::int32_t> override {
    std::unique_lock lock(mutex);
    ++write_calls;
    if (hold_first && write_calls == 1) {
      first_entered = true;
      condition.notify_all();
      condition.wait(lock, [&] { return release_first; });
    }
    const auto result = scripted.empty()
                            ? std::pair{static_cast<std::ptrdiff_t>(frame.size()),
                                        std::int32_t{0}}
                            : scripted.front();
    if (!scripted.empty()) scripted.erase(scripted.begin());
    if (result.first == static_cast<std::ptrdiff_t>(frame.size())) {
      written_first_bytes.push_back(frame.front());
    }
    condition.notify_all();
    return result;
  }
  void close_descriptor(int descriptor) noexcept override {
    std::lock_guard lock(mutex);
    closed.push_back(descriptor);
  }

  bool wait_for_first() {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(2),
                              [&] { return first_entered; });
  }
  bool wait_for_writes(unsigned count) {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(3),
                              [&] { return write_calls >= count; });
  }
  void release() {
    std::lock_guard lock(mutex);
    release_first = true;
    condition.notify_all();
  }

  std::mutex mutex;
  std::condition_variable condition;
  bool discoverable = true;
  bool character_device = true;
  int query_result = 0;
  int format_result = 0;
  v4l2_capability capabilities{};
  bool hold_first = false;
  bool first_entered = false;
  bool release_first = false;
  int next_descriptor = 20;
  unsigned open_calls = 0;
  unsigned write_calls = 0;
  std::vector<int> closed;
  std::vector<std::byte> written_first_bytes;
  std::vector<std::pair<std::ptrdiff_t, std::int32_t>> scripted;
};

class HealthObserver {
 public:
  static void changed(void* context, bool healthy,
                      camera::CameraSinkUnavailableReason,
                      std::int32_t) noexcept {
    auto& observer = *static_cast<HealthObserver*>(context);
    {
      std::lock_guard lock(observer.mutex);
      observer.states.push_back(healthy);
    }
    observer.condition.notify_all();
  }

  bool wait_for_recovery() {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(3), [&] {
      bool saw_unhealthy = false;
      for (const bool healthy : states) {
        if (!healthy) saw_unhealthy = true;
        if (saw_unhealthy && healthy) return true;
      }
      return false;
    });
  }

 private:
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<bool> states;
};

std::vector<std::byte> frame(std::byte blue) {
  const std::size_t stride =
      static_cast<std::size_t>(camera::kCanvas.width) * 4U;
  std::vector<std::byte> value(stride * camera::kCanvas.height);
  for (std::size_t index = 0; index < value.size(); index += 4) {
    value[index] = blue;
    value[index + 1] = std::byte{0};
    value[index + 2] = std::byte{0};
    value[index + 3] = std::byte{0xff};
  }
  return value;
}

camera::CameraSinkFrame sink_frame(const std::vector<std::byte>& bytes) {
  return {.width = camera::kCanvas.width,
          .height = camera::kCanvas.height,
          .row_stride = static_cast<std::size_t>(camera::kCanvas.width) * 4U,
          .bgra = bytes,
          .presentation_time_us = 1};
}

}  // namespace

SYNC_TEST(linux_camera_sink_maps_initialization_failures_and_closes_once) {
  DeviceOps missing;
  missing.discoverable = false;
  camera::LinuxCameraSink missing_sink({.device_operations = &missing});
  SYNC_REQUIRE(!missing_sink.available());
  SYNC_REQUIRE(missing_sink.unavailable_reason() ==
               camera::CameraSinkUnavailableReason::DeviceNotFound);

  DeviceOps denied;
  denied.discoverable = false;
  camera::LinuxCameraSink denied_sink(
      {.device_path = "/dev/video9", .device_operations = &denied});
  SYNC_REQUIRE(denied_sink.unavailable_reason() ==
               camera::CameraSinkUnavailableReason::DeviceNotFound);

  DeviceOps valid;
  {
    camera::LinuxCameraSink sink({.device_operations = &valid});
    SYNC_REQUIRE(sink.available());
    SYNC_REQUIRE(sink.healthy());
    SYNC_REQUIRE(sink.device_path() == "/dev/video9");
  }
  SYNC_REQUIRE(valid.closed.size() == 1);
}

SYNC_TEST(linux_camera_sink_keeps_only_the_newest_complete_queued_frame) {
  DeviceOps operations;
  operations.hold_first = true;
  wall_clock_enabled.store(false, std::memory_order_relaxed);
  noisefactor::sync::DaemonMetrics metrics;
  const auto a = frame(std::byte{0x10});
  const auto b = frame(std::byte{0x80});
  const auto c = frame(std::byte{0xf0});
  camera::LinuxCameraSink sink({.device_operations = &operations,
                                .metrics = &metrics,
                                .clock_ms = controlled_clock_ms});
  SYNC_REQUIRE(sink.submit(sink_frame(a)) == camera::CameraSinkSubmit::Accepted);
  SYNC_REQUIRE(operations.wait_for_first());
  SYNC_REQUIRE(sink.submit(sink_frame(b)) == camera::CameraSinkSubmit::Accepted);
  SYNC_REQUIRE(sink.submit(sink_frame(c)) == camera::CameraSinkSubmit::Accepted);
  wall_clock_enabled.store(true, std::memory_order_relaxed);
  operations.release();
  SYNC_REQUIRE(operations.wait_for_writes(2));
  const auto snapshot = metrics.snapshot();
  SYNC_REQUIRE(snapshot.camera_queue_replacements == 1);
  SYNC_REQUIRE(snapshot.camera_driving_frames == 3);
  SYNC_REQUIRE(operations.written_first_bytes.size() >= 2);
  SYNC_REQUIRE(operations.written_first_bytes[0] !=
               operations.written_first_bytes[1]);
}

SYNC_TEST(linux_camera_sink_isolates_backpressure_and_recovers_device_loss) {
  DeviceOps operations;
  operations.hold_first = true;
  wall_clock_enabled.store(false, std::memory_order_relaxed);
  const auto size = static_cast<std::ptrdiff_t>(camera::nv12_size_bytes(
      camera::kCanvas.width, camera::kCanvas.height, camera::kCanvas.width));
  operations.scripted = {{-1, EAGAIN}, {-1, ENODEV}, {size, 0}};
  noisefactor::sync::DaemonMetrics metrics;
  HealthObserver health;
  const auto bytes = frame(std::byte{0x44});
  camera::LinuxCameraSink sink(
      {.device_operations = &operations,
       .metrics = &metrics,
       .clock_ms = controlled_clock_ms,
       .health_changed = HealthObserver::changed,
       .health_context = &health});
  SYNC_REQUIRE(sink.submit(sink_frame(bytes)) == camera::CameraSinkSubmit::Accepted);
  SYNC_REQUIRE(operations.wait_for_first());
  SYNC_REQUIRE(sink.submit(sink_frame(bytes)) == camera::CameraSinkSubmit::Accepted);
  wall_clock_enabled.store(true, std::memory_order_relaxed);
  operations.release();
  SYNC_REQUIRE(operations.wait_for_writes(2));
  SYNC_REQUIRE(health.wait_for_recovery());
  SYNC_REQUIRE(sink.healthy());
  SYNC_REQUIRE(operations.open_calls >= 2);
  const auto snapshot = metrics.snapshot();
  SYNC_REQUIRE(snapshot.camera_backpressure_drops >= 1);
  SYNC_REQUIRE(snapshot.camera_write_failures >= 1);
  SYNC_REQUIRE(snapshot.camera_reopen_attempts >= 1);
}

SYNC_TEST(linux_camera_unavailability_formats_errno_not_osstatus) {
  const std::string text = camera::describe_unavailability(
      camera::CameraSinkUnavailableReason::DevicePermissionDenied, EACCES);
  SYNC_REQUIRE(text.find("errno 13") != std::string::npos);
  SYNC_REQUIRE(text.find("OSStatus") == std::string::npos);
}
