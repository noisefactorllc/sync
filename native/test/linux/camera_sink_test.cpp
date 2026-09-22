#include "../test_harness.hpp"

#include <sync/camera/frame_ring.hpp>
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
#include <fcntl.h>
#include <mutex>
#include <span>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace camera = noisefactor::sync::camera;

struct PosixShmReaderMapping {
  int fd = -1;
  void* view = nullptr;
  std::size_t size = 0;
  std::string path;

  explicit PosixShmReaderMapping(std::string_view file_path) : path(file_path) {
    size = camera::frame_ring_bytes();
    fd = ::open(path.c_str(), O_RDWR);
    if (fd >= 0) {
      view = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (view == MAP_FAILED) {
        view = nullptr;
        ::close(fd);
        fd = -1;
      }
    }
  }

  ~PosixShmReaderMapping() {
    if (view != nullptr) ::munmap(view, size);
    if (fd >= 0) ::close(fd);
  }

  [[nodiscard]] bool valid() const noexcept { return view != nullptr; }

  [[nodiscard]] std::span<std::byte> mapping() noexcept {
    return {static_cast<std::byte*>(view), size};
  }

  PosixShmReaderMapping(const PosixShmReaderMapping&) = delete;
  PosixShmReaderMapping& operator=(const PosixShmReaderMapping&) = delete;
};

auto test_temp_path() -> std::string {
  static std::atomic<std::uint64_t> counter{0};
  const auto id = counter.fetch_add(1, std::memory_order_relaxed);
  return "/tmp/sync_test_linux_camera_shm_" + std::to_string(::getpid()) + "_" +
         std::to_string(id) + ".frames";
}

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
    capabilities.capabilities = V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_READWRITE |
                                V4L2_CAP_EXT_PIX_FORMAT;
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

SYNC_TEST(linux_camera_sink_survives_asymmetric_v4l2_disconnect_while_shm_consumer_remains_active) {
  const auto shm_file = test_temp_path();
  DeviceOps operations;
  operations.hold_first = true;
  wall_clock_enabled.store(false, std::memory_order_relaxed);
  const auto size = static_cast<std::ptrdiff_t>(camera::nv12_size_bytes(
      camera::kCanvas.width, camera::kCanvas.height, camera::kCanvas.width));
  operations.scripted = {{-1, ENODEV}, {-1, ENODEV}, {size, 0}};
  noisefactor::sync::DaemonMetrics metrics;
  HealthObserver health;

  camera::LinuxCameraSink sink({
      .shm_path = shm_file,
      .enable_shm = true,
      .device_operations = &operations,
      .metrics = &metrics,
      .clock_ms = controlled_clock_ms,
      .health_changed = HealthObserver::changed,
      .health_context = &health,
  });

  SYNC_REQUIRE(sink.available());
  SYNC_REQUIRE(sink.healthy());

  PosixShmReaderMapping shm(shm_file);
  SYNC_REQUIRE(shm.valid());
  camera::FrameRingReader reader(shm.mapping());

  // Mark demand from the SHM reader
  reader.mark_demand(camera::camera_clock_us());

  const auto frame1 = frame(std::byte{0x11});
  const auto frame2 = frame(std::byte{0x22});
  const auto frame3 = frame(std::byte{0x33});

  // Submit frame 1: accepted by both SHM and queued to V4L2
  SYNC_REQUIRE(sink.submit(sink_frame(frame1)) == camera::CameraSinkSubmit::Accepted);
  SYNC_REQUIRE(operations.wait_for_first());

  // Release V4L2 background thread to encounter ENODEV
  wall_clock_enabled.store(true, std::memory_order_relaxed);
  operations.release();

  // Keep submitting frames while V4L2 is failing
  reader.mark_demand(camera::camera_clock_us());
  SYNC_REQUIRE(sink.submit(sink_frame(frame2)) == camera::CameraSinkSubmit::Accepted);
  reader.mark_demand(camera::camera_clock_us());
  SYNC_REQUIRE(sink.submit(sink_frame(frame3)) == camera::CameraSinkSubmit::Accepted);

  // The SHM reader reads the latest frame despite V4L2 failure
  const std::size_t stride = static_cast<std::size_t>(camera::kCanvas.width) * 4U;
  std::vector<std::byte> read_buf(stride * camera::kCanvas.height);
  SYNC_REQUIRE(reader.read_latest(read_buf, stride));
  SYNC_REQUIRE(read_buf[0] == std::byte{0x33});

  // Wait for V4L2 to recover
  SYNC_REQUIRE(health.wait_for_recovery());
  SYNC_REQUIRE(sink.healthy());

  // Submit frame 4: both consumers receive it
  const auto frame4 = frame(std::byte{0x44});
  reader.mark_demand(camera::camera_clock_us());
  SYNC_REQUIRE(sink.submit(sink_frame(frame4)) == camera::CameraSinkSubmit::Accepted);
  SYNC_REQUIRE(reader.read_latest(read_buf, stride));
  SYNC_REQUIRE(read_buf[0] == std::byte{0x44});

  SYNC_REQUIRE(operations.open_calls >= 2);
}

SYNC_TEST(linux_camera_sink_survives_asymmetric_shm_disconnect_while_v4l2_consumer_remains_active) {
  const auto shm_file = test_temp_path();
  DeviceOps operations;
  wall_clock_enabled.store(true, std::memory_order_relaxed);
  noisefactor::sync::DaemonMetrics metrics;

  camera::LinuxCameraSink sink({
      .shm_path = shm_file,
      .enable_shm = true,
      .device_operations = &operations,
      .metrics = &metrics,
  });

  SYNC_REQUIRE(sink.available());
  SYNC_REQUIRE(sink.healthy());

  PosixShmReaderMapping shm(shm_file);
  SYNC_REQUIRE(shm.valid());
  camera::FrameRingReader reader(shm.mapping());

  // Assert demand initially
  reader.mark_demand(camera::camera_clock_us());

  const auto frame1 = frame(std::byte{0xaa});
  SYNC_REQUIRE(sink.submit(sink_frame(frame1)) == camera::CameraSinkSubmit::Accepted);
  SYNC_REQUIRE(operations.wait_for_writes(1));

  const std::size_t stride = static_cast<std::size_t>(camera::kCanvas.width) * 4U;
  std::vector<std::byte> read_buf(stride * camera::kCanvas.height);
  SYNC_REQUIRE(reader.read_latest(read_buf, stride));
  SYNC_REQUIRE(read_buf[0] == std::byte{0xaa});

  // Disconnect SHM consumer: set last_demand_us in ring header to 1 so demand expires
  auto* header = reinterpret_cast<camera::FrameRingHeader*>(shm.mapping().data());
  header->last_demand_us.store(1, std::memory_order_release);

  // Submit frames with expired SHM consumer: V4L2 continues receiving frames at 60 Hz
  const auto frame2 = frame(std::byte{0xbb});
  const auto frame3 = frame(std::byte{0xcc});
  SYNC_REQUIRE(sink.submit(sink_frame(frame2)) == camera::CameraSinkSubmit::Accepted);
  SYNC_REQUIRE(sink.submit(sink_frame(frame3)) == camera::CameraSinkSubmit::Accepted);
  SYNC_REQUIRE(operations.wait_for_writes(3));

  // SHM consumer reconnects by reasserting demand
  reader.mark_demand(camera::camera_clock_us());
  const auto frame4 = frame(std::byte{0xdd});
  SYNC_REQUIRE(sink.submit(sink_frame(frame4)) == camera::CameraSinkSubmit::Accepted);
  SYNC_REQUIRE(operations.wait_for_writes(4));
  SYNC_REQUIRE(reader.read_latest(read_buf, stride));
  SYNC_REQUIRE(read_buf[0] == std::byte{0xdd});
}

SYNC_TEST(linux_camera_sink_asymmetric_zero_copy_submit_written_survives_consumer_disconnect_and_reconnect) {
  const auto shm_file = test_temp_path();
  DeviceOps operations;
  wall_clock_enabled.store(true, std::memory_order_relaxed);

  camera::LinuxCameraSink sink({
      .shm_path = shm_file,
      .enable_shm = true,
      .device_operations = &operations,
  });

  SYNC_REQUIRE(sink.available());

  PosixShmReaderMapping shm(shm_file);
  SYNC_REQUIRE(shm.valid());
  camera::FrameRingReader reader(shm.mapping());

  auto* header = reinterpret_cast<camera::FrameRingHeader*>(shm.mapping().data());

  struct RenderCtx {
    std::byte pattern{0};
  };

  const auto direct_writer = [](void* ctx, std::span<std::byte> dest, std::size_t) noexcept -> bool {
    auto& r = *static_cast<RenderCtx*>(ctx);
    std::fill(dest.begin(), dest.end(), r.pattern);
    return true;
  };

  // Scenario A: When SHM has no demand and only V4L2 is active, submit_written returns Unsupported
  header->last_demand_us.store(1, std::memory_order_release);
  RenderCtx ctx{std::byte{0x10}};
  SYNC_REQUIRE(sink.submit_written(direct_writer, &ctx, 100) ==
               camera::CameraSinkWrite::Unsupported);

  // Scenario B: Dual active (both SHM demand active and V4L2 healthy)
  reader.mark_demand(camera::camera_clock_us());
  ctx.pattern = std::byte{0x77};
  SYNC_REQUIRE(sink.submit_written(direct_writer, &ctx, 200) ==
               camera::CameraSinkWrite::Accepted);
  SYNC_REQUIRE(operations.wait_for_writes(1));

  const std::size_t stride = static_cast<std::size_t>(camera::kCanvas.width) * 4U;
  std::vector<std::byte> read_buf(stride * camera::kCanvas.height);
  SYNC_REQUIRE(reader.read_latest(read_buf, stride));
  SYNC_REQUIRE(read_buf[0] == std::byte{0x77});

  // Scenario C: V4L2 disconnected (write failure / unhealthy), but SHM active
  operations.hold_first = true;
  wall_clock_enabled.store(false, std::memory_order_relaxed);
  const auto size = static_cast<std::ptrdiff_t>(camera::nv12_size_bytes(
      camera::kCanvas.width, camera::kCanvas.height, camera::kCanvas.width));
  operations.scripted = {{-1, ENODEV}, {-1, ENODEV}, {size, 0}};

  // Submit normal frame to trigger ENODEV on V4L2
  const auto kick = frame(std::byte{0x99});
  sink.submit(sink_frame(kick));
  SYNC_REQUIRE(operations.wait_for_first());
  wall_clock_enabled.store(true, std::memory_order_relaxed);
  operations.release();

  // Wait briefly for V4L2 health state to drop
  for (int i = 0; i < 50 && sink.healthy(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  // Now V4L2 is unhealthy, but SHM is still active: submit_written writes directly to SHM
  ctx.pattern = std::byte{0x88};
  reader.mark_demand(camera::camera_clock_us());
  SYNC_REQUIRE(sink.submit_written(direct_writer, &ctx, 300) ==
               camera::CameraSinkWrite::Accepted);
  SYNC_REQUIRE(reader.read_latest(read_buf, stride));
  SYNC_REQUIRE(read_buf[0] == std::byte{0x88});
}

SYNC_TEST(linux_camera_concurrent_multi_reader_shm_and_v4l2_seqlock_consistency) {
  const auto shm_file = test_temp_path();
  DeviceOps operations;
  wall_clock_enabled.store(true, std::memory_order_relaxed);

  camera::LinuxCameraSink sink({
      .shm_path = shm_file,
      .enable_shm = true,
      .device_operations = &operations,
  });

  SYNC_REQUIRE(sink.available());

  PosixShmReaderMapping shm1(shm_file);
  PosixShmReaderMapping shm2(shm_file);
  PosixShmReaderMapping shm3(shm_file);
  SYNC_REQUIRE(shm1.valid());
  SYNC_REQUIRE(shm2.valid());
  SYNC_REQUIRE(shm3.valid());

  std::atomic<bool> producer_done{false};
  std::atomic<std::size_t> r1_successes{0};
  std::atomic<std::size_t> r2_successes{0};
  std::atomic<std::size_t> r3_successes{0};
  std::atomic<bool> consistency_violation{false};

  const std::size_t stride = static_cast<std::size_t>(camera::kCanvas.width) * 4U;

  auto reader_worker = [&](PosixShmReaderMapping& mapping,
                           std::atomic<std::size_t>& counter) {
    camera::FrameRingReader reader(mapping.mapping());
    std::vector<std::byte> buffer(stride * camera::kCanvas.height);

    while (!producer_done.load(std::memory_order_relaxed)) {
      reader.mark_demand(camera::camera_clock_us());
      if (reader.read_latest(buffer, stride)) {
        counter.fetch_add(1, std::memory_order_relaxed);
        // Verify buffer consistency across entire frame (no torn read)
        const std::byte sample = buffer[0];
        const std::byte mid = buffer[buffer.size() / 2];
        const std::byte end = buffer[buffer.size() - 4];
        if (sample != mid || sample != end) {
          consistency_violation.store(true, std::memory_order_release);
        }
      }
      std::this_thread::yield();
    }
  };

  std::thread t1([&] { reader_worker(shm1, r1_successes); });
  std::thread t2([&] { reader_worker(shm2, r2_successes); });
  std::thread t3([&] { reader_worker(shm3, r3_successes); });

  for (unsigned i = 1; i <= 60; ++i) {
    const std::byte b = static_cast<std::byte>(i & 0x7f);
    const auto f = frame(b);
    sink.submit(sink_frame(f));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  producer_done.store(true, std::memory_order_release);
  t1.join();
  t2.join();
  t3.join();

  SYNC_REQUIRE(!consistency_violation.load());
  SYNC_REQUIRE(r1_successes.load() > 0);
  SYNC_REQUIRE(r2_successes.load() > 0);
  SYNC_REQUIRE(r3_successes.load() > 0);
}
