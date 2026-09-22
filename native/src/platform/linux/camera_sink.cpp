#include <sync/platform/linux_camera_sink.hpp>

#include <sync/camera/frame_ring.hpp>
#include <sync/camera/nv12.hpp>
#include <sync/platform/camera_idle_card.hpp>
#include <sync/platform/camera_identity.hpp>
#include <sync/platform/camera_relay_policy.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace noisefactor::sync::camera {
namespace {

std::uint64_t monotonic_ms() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct PublicFailure {
  CameraSinkUnavailableReason reason = CameraSinkUnavailableReason::DeviceWriteFailed;
  std::int32_t status = 0;
};

PublicFailure describe_open_failure(const LinuxCameraOpenResult& result) noexcept {
  switch (result.error) {
    case LinuxCameraDeviceError::None:
      return {.reason = CameraSinkUnavailableReason::None};
    case LinuxCameraDeviceError::NotFound:
      return {.reason = CameraSinkUnavailableReason::DeviceNotFound,
              .status = result.native_error};
    case LinuxCameraDeviceError::OpenDenied:
      return {.reason = CameraSinkUnavailableReason::DevicePermissionDenied,
              .status = result.native_error};
    case LinuxCameraDeviceError::FormatRejected:
    case LinuxCameraDeviceError::InvalidFormatBounds:
      return {.reason = CameraSinkUnavailableReason::FormatRejected,
              .status = result.native_error};
    case LinuxCameraDeviceError::InvalidPath:
    case LinuxCameraDeviceError::NotCharacterDevice:
    case LinuxCameraDeviceError::WrongCard:
    case LinuxCameraDeviceError::WrongDriver:
    case LinuxCameraDeviceError::MissingOutputCapability:
    case LinuxCameraDeviceError::Ambiguous:
      return {.reason = CameraSinkUnavailableReason::WrongDevice,
              .status = result.native_error};
    case LinuxCameraDeviceError::Io:
      return {.reason = CameraSinkUnavailableReason::DeviceWriteFailed,
              .status = result.native_error};
  }
  return {};
}

}  // namespace

struct LinuxCameraSink::Impl {
  enum class SlotState : std::uint8_t { Free, Filling, Queued, Writing };
  struct Slot {
    std::vector<std::byte> bytes;
    SlotState state = SlotState::Free;
  };

  explicit Impl(Options value)
      : options(value),
        operations(value.device_operations != nullptr
                       ? *value.device_operations
                       : default_linux_camera_device_ops()),
        clock(value.clock_ms != nullptr ? value.clock_ms : monotonic_ms),
        explicit_path(value.device_path),
        relay(1'000'000'000ULL / 30ULL, 250'000'000ULL) {}

  Options options;
  LinuxCameraDeviceOps& operations;
  std::uint64_t (*clock)() noexcept;
  std::string explicit_path;
  std::string selected_path;
  LinuxCameraFormat format{};
  int descriptor = -1;
  bool initially_available = false;
  CameraSinkUnavailableReason initial_reason =
      CameraSinkUnavailableReason::DeviceWriteFailed;
  std::int32_t initial_status = 0;
  std::atomic<bool> healthy_state{false};
  std::atomic<CameraSinkUnavailableReason> current_reason_state{
      CameraSinkUnavailableReason::DeviceWriteFailed};
  std::atomic<std::int32_t> current_status_state{0};
  std::array<Slot, 3> slots{};
  std::vector<std::byte> idle_bgra;
  std::vector<std::byte> idle_nv12;
  mutable std::mutex mutex;
  std::condition_variable condition;
  bool stopping = false;
  std::thread writer;
  CameraRelayPolicy relay;
  std::uint64_t started_ms = 0;
  std::uint64_t next_live_ms = 0;
  unsigned consecutive_failures = 0;
  std::uint64_t reopen_delay_ms = 100;

  std::string ring_path;
  int ring_fd = -1;
  void* ring_view = nullptr;
  std::size_t ring_bytes = 0;
  std::unique_ptr<FrameRingWriter> shm_writer;

  void init_ring(std::string_view path, bool enable) noexcept {
    if (!enable) return;
    std::string candidate_path = path.empty() ? posix_shm_path() : std::string(path);
    ring_bytes = frame_ring_bytes();
    ring_path = std::move(candidate_path);

    // Mode 0600 (S_IRUSR | S_IWUSR) creates an owner-private file.
    // O_NOFOLLOW prevents following symlinks. O_CLOEXEC prevents fd leaks.
    ring_fd = ::open(ring_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (ring_fd < 0) {
      ring_path.clear();
      return;
    }

    struct stat st{};
    if (::fstat(ring_fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid()) {
      ::close(ring_fd);
      ring_fd = -1;
      ring_path.clear();
      return;
    }

    if ((st.st_mode & 0777) != 0600) {
      if (::fchmod(ring_fd, 0600) != 0) {
        ::close(ring_fd);
        ring_fd = -1;
        ring_path.clear();
        return;
      }
    }

    if (::ftruncate(ring_fd, static_cast<off_t>(ring_bytes)) != 0) {
      ::close(ring_fd);
      ring_fd = -1;
      ring_path.clear();
      return;
    }

    ring_view = ::mmap(nullptr, ring_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, ring_fd, 0);
    if (ring_view == MAP_FAILED) {
      ring_view = nullptr;
      ::close(ring_fd);
      ring_fd = -1;
      ring_path.clear();
      return;
    }

    shm_writer = std::make_unique<FrameRingWriter>(
        std::span<std::byte>(static_cast<std::byte*>(ring_view), ring_bytes));
    if (!shm_writer->valid()) {
      shm_writer.reset();
      ::munmap(ring_view, ring_bytes);
      ring_view = nullptr;
      ::close(ring_fd);
      ring_fd = -1;
      ring_path.clear();
    }
  }

  void close_ring() noexcept {
    if (shm_writer) shm_writer.reset();
    if (ring_view != nullptr && ring_view != MAP_FAILED) {
      std::memset(ring_view, 0, ring_bytes);
      ::msync(ring_view, ring_bytes, MS_SYNC);
      ::munmap(ring_view, ring_bytes);
      ring_view = nullptr;
    }
    if (ring_fd >= 0) {
      ::close(ring_fd);
      ring_fd = -1;
    }
    if (!ring_path.empty()) {
      ::unlink(ring_path.c_str());
      ring_path.clear();
    }
  }

  void publish_health(bool value, CameraSinkUnavailableReason reason,
                      std::int32_t status) noexcept {
    const bool previous = healthy_state.exchange(value);
    current_reason_state.store(reason);
    current_status_state.store(status);
    if ((previous != value || !value) && options.health_changed != nullptr) {
      options.health_changed(options.health_context, value, reason, status);
    }
  }

  bool initialize() {
    init_ring(options.shm_path, options.enable_shm);

    const LinuxCameraOpenResult opened =
        open_linux_camera(explicit_path, operations);
    if (opened.error != LinuxCameraDeviceError::None) {
      const PublicFailure failure = describe_open_failure(opened);
      initial_reason = failure.reason;
      initial_status = failure.status;
      publish_health(false, failure.reason, failure.status);
      return (shm_writer != nullptr && shm_writer->valid());
    }
    descriptor = opened.descriptor;
    format = opened.format;
    selected_path.assign(opened.path.data());
    try {
      for (Slot& slot : slots) slot.bytes.resize(format.size_image);
      const std::size_t bgra_stride =
          static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;
      idle_bgra.resize(bgra_stride * kCanvas.height);
      idle_nv12.resize(format.size_image);
    } catch (...) {
      operations.close_descriptor(descriptor);
      descriptor = -1;
      initial_reason = CameraSinkUnavailableReason::DeviceWriteFailed;
      initial_status = ENOMEM;
      publish_health(false, initial_reason, initial_status);
      return (shm_writer != nullptr && shm_writer->valid());
    }
    const std::size_t bgra_stride =
        static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;
    std::fill(idle_nv12.begin(), idle_nv12.end(), std::byte{0});
    if (!draw_camera_idle_card(idle_bgra, bgra_stride, kCanvas) ||
        !bgra_to_nv12(idle_bgra, bgra_stride, kCanvas.width, kCanvas.height,
                      idle_nv12, format.y_stride)) {
      operations.close_descriptor(descriptor);
      descriptor = -1;
      initial_reason = CameraSinkUnavailableReason::FormatRejected;
      publish_health(false, initial_reason, 0);
      return (shm_writer != nullptr && shm_writer->valid());
    }
    initially_available = true;
    initial_reason = CameraSinkUnavailableReason::None;
    initial_status = 0;
    publish_health(true, CameraSinkUnavailableReason::None, 0);
    relay.source_started();
    started_ms = clock();
    writer = std::thread([this] { writer_loop(); });
    return true;
  }

  std::pair<std::ptrdiff_t, std::int32_t> write(
      std::span<const std::byte> bytes) noexcept {
    std::pair<std::ptrdiff_t, std::int32_t> result{-1, EIO};
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
      result = operations.write_frame(descriptor, bytes);
      if (!(result.first < 0 && result.second == EINTR)) break;
    }
    return result;
  }

  void transition_to_recovery(std::int32_t status) noexcept {
    if (descriptor >= 0) {
      operations.close_descriptor(descriptor);
      descriptor = -1;
    }
    publish_health(false, CameraSinkUnavailableReason::DeviceWriteFailed,
                   status);
    reopen_delay_ms = 100;
  }

  void handle_write_result(std::pair<std::ptrdiff_t, std::int32_t> result,
                           std::size_t expected, bool idle,
                           std::uint64_t now) noexcept {
    if (result.first == static_cast<std::ptrdiff_t>(expected)) {
      consecutive_failures = 0;
      reopen_delay_ms = 100;
      if (options.metrics != nullptr) options.metrics->note_camera_write(now, idle);
      if (!idle) relay.client_frame_arrived(now * 1'000'000ULL);
      return;
    }
    if (result.first < 0 &&
        (result.second == EAGAIN || result.second == EWOULDBLOCK)) {
      if (options.metrics != nullptr) options.metrics->note_camera_backpressure();
      return;
    }
    if (options.metrics != nullptr) options.metrics->note_camera_write_failure();
    ++consecutive_failures;
    if ((result.first < 0 &&
         (result.second == ENODEV || result.second == EPIPE)) ||
        consecutive_failures >= 3) {
      transition_to_recovery(result.second);
    }
  }

  void reopen(std::unique_lock<std::mutex>& lock) noexcept {
    const std::uint64_t delay = reopen_delay_ms;
    if (condition.wait_for(lock, std::chrono::milliseconds(delay),
                           [&] { return stopping; })) {
      return;
    }
    lock.unlock();
    if (options.metrics != nullptr) options.metrics->note_camera_reopen_attempt();
    const LinuxCameraOpenResult opened =
        open_linux_camera(explicit_path, operations);
    lock.lock();
    if (stopping) {
      if (opened.descriptor >= 0) operations.close_descriptor(opened.descriptor);
      return;
    }
    if (opened.error == LinuxCameraDeviceError::None &&
        opened.format.y_stride == format.y_stride &&
        opened.format.size_image == format.size_image) {
      descriptor = opened.descriptor;
      publish_health(true, CameraSinkUnavailableReason::None, 0);
      consecutive_failures = 0;
      reopen_delay_ms = 100;
      return;
    }
    if (opened.descriptor >= 0) operations.close_descriptor(opened.descriptor);
    const PublicFailure failure = describe_open_failure(opened);
    publish_health(false, failure.reason, failure.status);
    reopen_delay_ms = std::min<std::uint64_t>(reopen_delay_ms * 2, 5000);
  }

  void writer_loop() noexcept {
    std::unique_lock lock(mutex);
    while (!stopping) {
      if (!healthy_state.load()) {
        reopen(lock);
        continue;
      }
      std::size_t queued = slots.size();
      for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index].state == SlotState::Queued) {
          queued = index;
          break;
        }
      }
      const std::uint64_t now = clock();
      if (queued != slots.size()) {
        if (now < next_live_ms) {
          condition.wait_for(lock,
                             std::chrono::milliseconds(next_live_ms - now),
                             [&] { return stopping || !healthy_state.load(); });
          continue;
        }
        slots[queued].state = SlotState::Writing;
        const int active_descriptor = descriptor;
        (void)active_descriptor;
        lock.unlock();
        const auto result = write(slots[queued].bytes);
        const std::uint64_t completed = clock();
        lock.lock();
        slots[queued].state = SlotState::Free;
        next_live_ms = completed + 17;
        handle_write_result(result, format.size_image, false, completed);
        condition.notify_all();
        continue;
      }
      if (now - started_ms >= 250 &&
          relay.tick(now * 1'000'000ULL) == CameraRelayPolicy::Action::EmitBlack) {
        lock.unlock();
        const auto result = write(idle_nv12);
        const std::uint64_t completed = clock();
        lock.lock();
        handle_write_result(result, format.size_image, true, completed);
        continue;
      }
      condition.wait_for(lock, std::chrono::milliseconds(5),
                         [&] { return stopping || !healthy_state.load() ||
                                      std::ranges::any_of(slots, [](const Slot& slot) {
                                        return slot.state == SlotState::Queued;
                                      }); });
    }
  }

  void shutdown() noexcept {
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    condition.notify_all();
    if (writer.joinable()) writer.join();
    if (descriptor >= 0) {
      operations.close_descriptor(descriptor);
      descriptor = -1;
    }
    close_ring();
  }
};

LinuxCameraSink::LinuxCameraSink() : LinuxCameraSink(Options{}) {}

LinuxCameraSink::LinuxCameraSink(Options options)
    : impl_(std::make_unique<Impl>(options)) {
  (void)impl_->initialize();
}

LinuxCameraSink::~LinuxCameraSink() noexcept {
  if (impl_ != nullptr) impl_->shutdown();
}

auto LinuxCameraSink::available() const noexcept -> bool {
  return impl_ != nullptr && (impl_->initially_available ||
                              (impl_->shm_writer != nullptr && impl_->shm_writer->valid()));
}

auto LinuxCameraSink::unavailable_reason() const noexcept
    -> CameraSinkUnavailableReason {
  if (impl_ == nullptr) return CameraSinkUnavailableReason::DeviceWriteFailed;
  if (impl_->shm_writer != nullptr && impl_->shm_writer->valid()) {
    return CameraSinkUnavailableReason::None;
  }
  return impl_->initial_reason;
}

auto LinuxCameraSink::unavailable_status() const noexcept -> std::int32_t {
  if (impl_ == nullptr) return 0;
  if (impl_->shm_writer != nullptr && impl_->shm_writer->valid()) {
    return 0;
  }
  return impl_->initial_status;
}

auto LinuxCameraSink::has_capacity() const noexcept -> bool {
  if (impl_ == nullptr || impl_->stopping) return false;
  const std::uint64_t now_us = camera_clock_us();
  const bool shm_demand = (impl_->shm_writer != nullptr && impl_->shm_writer->valid() &&
                           impl_->shm_writer->has_demand(now_us));
  bool v4l2_capacity = false;
  if (impl_->initially_available && impl_->healthy_state.load()) {
    std::lock_guard lock(impl_->mutex);
    v4l2_capacity = !impl_->stopping &&
                    std::ranges::any_of(impl_->slots, [](const Impl::Slot& slot) {
                      return slot.state == Impl::SlotState::Free ||
                             slot.state == Impl::SlotState::Queued;
                    });
  }
  return shm_demand || v4l2_capacity;
}

auto LinuxCameraSink::submit(const CameraSinkFrame& frame) noexcept
    -> CameraSinkSubmit {
  if (impl_ == nullptr || !available() ||
      frame.width != kCanvas.width || frame.height != kCanvas.height ||
      frame.row_stride < static_cast<std::size_t>(kCanvas.width) *
                             kBytesPerPixel ||
      frame.row_stride > std::numeric_limits<std::size_t>::max() /
                             frame.height ||
      frame.bgra.size() < frame.row_stride * frame.height) {
    return CameraSinkSubmit::Failed;
  }

  const std::uint64_t now_us = camera_clock_us();
  bool wrote_any = false;

  // 1. Deliver to POSIX shared memory ring buffer if demanded
  if (impl_->shm_writer != nullptr && impl_->shm_writer->valid() &&
      impl_->shm_writer->has_demand(now_us)) {
    if (impl_->shm_writer->write(frame.bgra, frame.row_stride,
                                 frame.presentation_time_us)) {
      wrote_any = true;
    }
  }

  // 2. Deliver to V4L2 loopback device if available and healthy
  if (impl_->initially_available) {
    std::size_t slot_index = impl_->slots.size();
    bool replacement = false;
    {
      std::lock_guard lock(impl_->mutex);
      if (!impl_->stopping && impl_->healthy_state.load()) {
        for (std::size_t index = 0; index < impl_->slots.size(); ++index) {
          if (impl_->slots[index].state == Impl::SlotState::Queued) {
            slot_index = index;
            replacement = true;
            break;
          }
        }
        if (slot_index == impl_->slots.size()) {
          for (std::size_t index = 0; index < impl_->slots.size(); ++index) {
            if (impl_->slots[index].state == Impl::SlotState::Free) {
              slot_index = index;
              break;
            }
          }
        }
        if (slot_index < impl_->slots.size()) {
          impl_->slots[slot_index].state = Impl::SlotState::Filling;
        }
      }
    }

    if (slot_index < impl_->slots.size()) {
      auto& bytes = impl_->slots[slot_index].bytes;
      std::fill(bytes.begin(), bytes.end(), std::byte{0});
      if (bgra_to_nv12(frame.bgra, frame.row_stride, frame.width, frame.height,
                       bytes, impl_->format.y_stride)) {
        bool queued = false;
        {
          std::lock_guard lock(impl_->mutex);
          if (!impl_->stopping && impl_->healthy_state.load()) {
            impl_->slots[slot_index].state = Impl::SlotState::Queued;
            queued = true;
          } else {
            impl_->slots[slot_index].state = Impl::SlotState::Free;
          }
        }
        if (queued) {
          wrote_any = true;
          if (impl_->options.metrics != nullptr) {
            impl_->options.metrics->note_camera_driving_frame();
            if (replacement) impl_->options.metrics->note_camera_queue_replacement();
          }
          impl_->condition.notify_one();
        }
      } else {
        std::lock_guard lock(impl_->mutex);
        impl_->slots[slot_index].state = Impl::SlotState::Free;
      }
    }
  }

  if (wrote_any) return CameraSinkSubmit::Accepted;

  if (impl_->options.metrics != nullptr) {
    impl_->options.metrics->note_camera_backpressure();
  }
  return CameraSinkSubmit::Backpressured;
}

auto LinuxCameraSink::submit_written(CameraFrameWriter writer, void* context,
                                     std::uint64_t presentation_time_us) noexcept
    -> CameraSinkWrite {
  if (impl_ == nullptr || !available()) return CameraSinkWrite::Failed;

  const std::uint64_t now_us = camera_clock_us();
  const bool shm_demand = (impl_->shm_writer != nullptr && impl_->shm_writer->valid() &&
                           impl_->shm_writer->has_demand(now_us));
  const bool v4l2_demand = (impl_->initially_available && impl_->healthy_state.load());

  if (!shm_demand && !v4l2_demand) {
    return CameraSinkWrite::Backpressured;
  }

  if (shm_demand && v4l2_demand) {
    struct DualContext {
      CameraFrameWriter writer;
      void* context;
      LinuxCameraSink::Impl* impl;
      std::uint64_t presentation_time_us;
    } dual{writer, context, impl_.get(), presentation_time_us};

    const auto copy_and_queue = [](void* ctx, std::span<std::byte> dest,
                                   std::size_t stride) noexcept -> bool {
      auto& d = *static_cast<DualContext*>(ctx);
      if (!d.writer(d.context, dest, stride)) return false;

      std::size_t slot_index = d.impl->slots.size();
      bool replacement = false;
      {
        std::lock_guard lock(d.impl->mutex);
        if (!d.impl->stopping && d.impl->healthy_state.load()) {
          for (std::size_t index = 0; index < d.impl->slots.size(); ++index) {
            if (d.impl->slots[index].state == Impl::SlotState::Queued) {
              slot_index = index;
              replacement = true;
              break;
            }
          }
          if (slot_index == d.impl->slots.size()) {
            for (std::size_t index = 0; index < d.impl->slots.size(); ++index) {
              if (d.impl->slots[index].state == Impl::SlotState::Free) {
                slot_index = index;
                break;
              }
            }
          }
          if (slot_index < d.impl->slots.size()) {
            d.impl->slots[slot_index].state = Impl::SlotState::Filling;
          }
        }
      }

      if (slot_index < d.impl->slots.size()) {
        auto& bytes = d.impl->slots[slot_index].bytes;
        std::fill(bytes.begin(), bytes.end(), std::byte{0});
        if (bgra_to_nv12(dest, stride, kCanvas.width, kCanvas.height,
                         bytes, d.impl->format.y_stride)) {
          bool queued = false;
          {
            std::lock_guard lock(d.impl->mutex);
            if (!d.impl->stopping && d.impl->healthy_state.load()) {
              d.impl->slots[slot_index].state = Impl::SlotState::Queued;
              queued = true;
            } else {
              d.impl->slots[slot_index].state = Impl::SlotState::Free;
            }
          }
          if (queued) {
            if (d.impl->options.metrics != nullptr) {
              d.impl->options.metrics->note_camera_driving_frame();
              if (replacement) d.impl->options.metrics->note_camera_queue_replacement();
            }
            d.impl->condition.notify_one();
          }
        } else {
          std::lock_guard lock(d.impl->mutex);
          d.impl->slots[slot_index].state = Impl::SlotState::Free;
        }
      }
      return true;
    };

    return impl_->shm_writer->write_with(copy_and_queue, &dual, presentation_time_us)
               ? CameraSinkWrite::Accepted
               : CameraSinkWrite::Failed;
  }

  if (shm_demand) {
    return impl_->shm_writer->write_with(writer, context, presentation_time_us)
               ? CameraSinkWrite::Accepted
               : CameraSinkWrite::Failed;
  }

  return CameraSinkWrite::Unsupported;
}

auto LinuxCameraSink::healthy() const noexcept -> bool {
  return impl_ != nullptr && impl_->healthy_state.load();
}

auto LinuxCameraSink::current_reason() const noexcept
    -> CameraSinkUnavailableReason {
  return impl_ == nullptr
             ? CameraSinkUnavailableReason::DeviceWriteFailed
             : impl_->current_reason_state.load();
}

auto LinuxCameraSink::current_status() const noexcept -> std::int32_t {
  return impl_ == nullptr ? 0 : impl_->current_status_state.load();
}

auto LinuxCameraSink::device_path() const noexcept -> std::string_view {
  return impl_ == nullptr ? std::string_view{} : impl_->selected_path;
}

}  // namespace noisefactor::sync::camera
