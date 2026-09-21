#include <sync/platform/cmio_camera_sink.hpp>
#include <sync/camera/frame_ring.hpp>
#include "cmio_frame_submission.hpp"

#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreMediaIO/CMIOHardware.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <fcntl.h>
#include <mach/mach_time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

namespace noisefactor::sync::camera {

namespace {

// kCMIOStreamPropertyDirection: 0 is an output stream (client -> device),
// which is the sink from this process's point of view; 1 is input.
constexpr UInt32 kOutputStreamDirection = 0;

template <typename T>
[[nodiscard]] auto property_vector(CMIOObjectID object, CMIOObjectPropertyAddress address,
                                   std::vector<T>& out) noexcept -> bool {
  UInt32 size = 0;
  if (CMIOObjectGetPropertyDataSize(object, &address, 0, nullptr, &size) != kCMIOHardwareNoError) {
    return false;
  }
  try {
    out.assign(size / sizeof(T), T{});
  } catch (...) {
    return false;
  }
  if (out.empty()) return true;
  UInt32 used = 0;
  return CMIOObjectGetPropertyData(object, &address, 0, nullptr, size, &used, out.data()) ==
         kCMIOHardwareNoError;
}

[[nodiscard]] auto device_uid(CMIODeviceID device) noexcept -> std::string {
  CMIOObjectPropertyAddress address{kCMIODevicePropertyDeviceUID, kCMIOObjectPropertyScopeGlobal,
                                    kCMIOObjectPropertyElementMain};
  CFStringRef uid = nullptr;
  UInt32 used = 0;
  if (CMIOObjectGetPropertyData(device, &address, 0, nullptr, sizeof(uid), &used, &uid) !=
          kCMIOHardwareNoError ||
      uid == nullptr) {
    return {};
  }
  char buffer[256] = {0};
  const bool ok = CFStringGetCString(uid, buffer, sizeof(buffer), kCFStringEncodingUTF8);
  CFRelease(uid);
  return ok ? std::string(buffer) : std::string{};
}

[[nodiscard]] auto stream_direction(CMIOStreamID stream, UInt32& direction) noexcept -> bool {
  CMIOObjectPropertyAddress address{kCMIOStreamPropertyDirection, kCMIOObjectPropertyScopeGlobal,
                                    kCMIOObjectPropertyElementMain};
  UInt32 used = 0;
  return CMIOObjectGetPropertyData(stream, &address, 0, nullptr, sizeof(direction), &used,
                                   &direction) == kCMIOHardwareNoError;
}

[[nodiscard]] auto host_time_ns() noexcept -> std::uint64_t {
  static const mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    return info;
  }();
  return mach_absolute_time() * timebase.numer / timebase.denom;
}

}  // namespace

namespace {
void queue_altered(CMIOStreamID, void*, void*) noexcept {}
}  // namespace

struct CmioCameraSink::Impl {
  int ring_fd = -1;
  void* ring_view = nullptr;
  std::size_t ring_bytes = 0;
  std::string ring_path;
  std::unique_ptr<FrameRingWriter> ring_writer;

  explicit Impl(Options options) : depth(options.queue_depth == 0 ? 1 : options.queue_depth) {
    init_ring(options.shm_path, options.enable_shm);
    discover(std::string(options.device_uid));
  }

  ~Impl() {
    close_ring();
    if (started) CMIODeviceStopStream(device, stream);
    if (queue != nullptr) {
      // The queue does not own its elements. Each was enqueued at +1 for the
      // extension to release after consuming; whatever it never consumed is
      // ours to release, or its pixel buffer stays pinned.
      while (const void* element = CMSimpleQueueDequeue(queue)) {
        CFRelease(element);
      }
      CFRelease(queue);
    }
    if (format != nullptr) CFRelease(format);
    if (pool != nullptr) CVPixelBufferPoolRelease(pool);
  }

  void init_ring(std::string_view path, bool enable) noexcept {
    if (!enable || path.empty()) return;
    ring_bytes = frame_ring_bytes();
    ring_path = std::string(path);

    // Mode 0600 (S_IRUSR | S_IWUSR) creates an owner-private file.
    // O_NOFOLLOW prevents following symlinks. O_CLOEXEC prevents fd leaks.
    ring_fd = ::open(ring_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (ring_fd < 0) {
      ring_path.clear();
      return;
    }

    // Existing-object validation: verify file is a regular file owned by the current process user.
    struct stat st{};
    if (::fstat(ring_fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid()) {
      ::close(ring_fd);
      ring_fd = -1;
      ring_path.clear();
      return;
    }

    // Ensure permissions are strictly owner-private 0600 even if the pre-existing file had looser mode.
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

    ring_writer = std::make_unique<FrameRingWriter>(
        std::span<std::byte>(static_cast<std::byte*>(ring_view), ring_bytes));
    if (!ring_writer->valid()) {
      ring_writer.reset();
      ::munmap(ring_view, ring_bytes);
      ring_view = nullptr;
      ::close(ring_fd);
      ring_fd = -1;
      ring_path.clear();
    }
  }

  void close_ring() noexcept {
    if (ring_writer) ring_writer.reset();
    if (ring_view != nullptr && ring_view != MAP_FAILED) {
      // Scrub retained pixels before unmapping so sensitive camera frames
      // do not linger in shared memory or disk cache.
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

  CameraSinkUnavailableReason reason = CameraSinkUnavailableReason::DeviceNotFound;
  OSStatus status = 0;
  CMIODeviceID device = kCMIOObjectUnknown;
  CMIOStreamID stream = kCMIOObjectUnknown;
  CMSimpleQueueRef queue = nullptr;
  CVPixelBufferPoolRef pool = nullptr;
  CMVideoFormatDescriptionRef format = nullptr;
  std::size_t depth = 3;
  bool started = false;

  void discover(const std::string& wanted_uid) noexcept {
    std::vector<CMIODeviceID> devices;
    if (!property_vector(kCMIOObjectSystemObject,
                         {kCMIOHardwarePropertyDevices, kCMIOObjectPropertyScopeGlobal,
                          kCMIOObjectPropertyElementMain},
                         devices)) {
      return;
    }
    for (CMIODeviceID candidate : devices) {
      if (device_uid(candidate) == wanted_uid) {
        device = candidate;
        break;
      }
    }
    if (device == kCMIOObjectUnknown) return;

    reason = CameraSinkUnavailableReason::SinkStreamMissing;
    std::vector<CMIOStreamID> streams;
    if (!property_vector(device,
                         {kCMIODevicePropertyStreams, kCMIOObjectPropertyScopeGlobal,
                          kCMIOObjectPropertyElementMain},
                         streams)) {
      return;
    }
    for (CMIOStreamID candidate : streams) {
      UInt32 direction = 1;
      if (stream_direction(candidate, direction) && direction == kOutputStreamDirection) {
        stream = candidate;
        break;
      }
    }
    if (stream == kCMIOObjectUnknown) return;

    // CoreMediaIO registers the queue-altered proc as part of copying the
    // queue; every working sink client (Apple's sample, OBS) passes a real
    // routine, so pass a no-op rather than NULL. Nothing waits on it: submit()
    // polls the queue count instead.
    reason = CameraSinkUnavailableReason::QueueNotProvided;
    status = CMIOStreamCopyBufferQueue(stream, &queue_altered, nullptr, &queue);
    if (status != kCMIOHardwareNoError || queue == nullptr) {
      queue = nullptr;
      return;
    }
    reason = CameraSinkUnavailableReason::StreamNotStarted;
    status = CMIODeviceStartStream(device, stream);
    if (status != kCMIOHardwareNoError) return;
    started = true;
    status = 0;

    NSDictionary* attributes = @{
      (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
      (id)kCVPixelBufferWidthKey : @(kCanvas.width),
      (id)kCVPixelBufferHeightKey : @(kCanvas.height),
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attributes,
                                &pool) != kCVReturnSuccess) {
      pool = nullptr;
      return;
    }
    reason = CameraSinkUnavailableReason::None;
  }
};

CmioCameraSink::CmioCameraSink() : CmioCameraSink(Options{}) {}

CmioCameraSink::CmioCameraSink(Options options) : impl_(std::make_unique<Impl>(options)) {}

CmioCameraSink::~CmioCameraSink() = default;

auto detail::submit_cmio_frame(CVPixelBufferPoolRef pool, CMVideoFormatDescriptionRef& format,
                               CMSimpleQueueRef queue, std::size_t depth,
                               CameraFrameWriter writer, void* context) noexcept -> CameraSinkSubmit {
  if (pool == nullptr || queue == nullptr) return CameraSinkSubmit::Failed;
  struct Operations {
    using Pixels = CVPixelBufferRef;
    using Sample = CMSampleBufferRef;
    CVPixelBufferPoolRef pool;
    CMVideoFormatDescriptionRef& format;
    CMSimpleQueueRef queue;
    std::size_t depth;

    auto has_capacity() const noexcept -> bool {
      return static_cast<std::size_t>(CMSimpleQueueGetCount(queue)) < depth;
    }
    auto allocate(Pixels& pixels) noexcept -> bool {
      return CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixels) == kCVReturnSuccess;
    }
    auto lock(Pixels pixels) noexcept -> bool {
      return CVPixelBufferLockBaseAddress(pixels, 0) == kCVReturnSuccess;
    }
    auto unlock(Pixels pixels) noexcept -> bool {
      return CVPixelBufferUnlockBaseAddress(pixels, 0) == kCVReturnSuccess;
    }
    void release_pixels(Pixels pixels) noexcept { CVPixelBufferRelease(pixels); }
    auto destination(Pixels pixels) const noexcept -> WritableCameraFrame {
      auto* base = static_cast<std::byte*>(CVPixelBufferGetBaseAddress(pixels));
      if (base == nullptr || CVPixelBufferIsPlanar(pixels)) return {};
      return {.bytes = {base, CVPixelBufferGetDataSize(pixels)},
              .stride = CVPixelBufferGetBytesPerRow(pixels),
              .width = CVPixelBufferGetWidth(pixels), .height = CVPixelBufferGetHeight(pixels)};
    }
    auto prepare_format(Pixels pixels) noexcept -> bool {
      CVBufferSetAttachment(pixels, kCVImageBufferColorPrimariesKey,
                            kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
      CVBufferSetAttachment(pixels, kCVImageBufferTransferFunctionKey,
                            kCVImageBufferTransferFunction_sRGB, kCVAttachmentMode_ShouldPropagate);
      if (format != nullptr) return true;
      CMVideoFormatDescriptionRef created = nullptr;
      const OSStatus result = CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault,
                                                                          pixels, &created);
      if (result != noErr || created == nullptr) {
        if (created != nullptr) CFRelease(created);
        return false;
      }
      format = created;
      return true;
    }
    auto make_sample(Pixels pixels, Sample& sample) const noexcept -> bool {
      const CMTime pts = CMTimeMake(static_cast<int64_t>(host_time_ns()), 1'000'000'000);
      CMSampleTimingInfo timing{
          .duration = CMTimeMake(1, static_cast<int32_t>(kMaximumFramesPerSecond)),
          .presentationTimeStamp = pts,
          .decodeTimeStamp = kCMTimeInvalid,
      };
      return CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, pixels, true, nullptr, nullptr,
                                                format, &timing, &sample) == noErr;
    }
    auto enqueue(Sample sample) noexcept -> bool { return CMSimpleQueueEnqueue(queue, sample) == noErr; }
    void release_sample(Sample sample) noexcept { CFRelease(sample); }
  } operations{pool, format, queue, depth};
  return submit_camera_frame(operations, writer, context);
}

auto CmioCameraSink::available() const noexcept -> bool {
  return impl_->reason == CameraSinkUnavailableReason::None;
}

auto CmioCameraSink::unavailable_reason() const noexcept -> CameraSinkUnavailableReason {
  return impl_->reason;
}

auto CmioCameraSink::unavailable_status() const noexcept -> std::int32_t {
  return impl_->status;
}

auto CmioCameraSink::has_capacity() const noexcept -> bool {
  const bool cmio_cap = available() &&
         static_cast<std::size_t>(CMSimpleQueueGetCount(impl_->queue)) < impl_->depth;
  const bool shm_demand = (impl_->ring_writer != nullptr && impl_->ring_writer->valid() &&
                           impl_->ring_writer->has_demand(camera_clock_us()));
  return cmio_cap || shm_demand;
}

auto CmioCameraSink::submit(const CameraSinkFrame& frame) noexcept -> CameraSinkSubmit {
  if (frame.width != kCanvas.width || frame.height != kCanvas.height ||
      frame.row_stride < static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel ||
      frame.row_stride > frame.bgra.size() / kCanvas.height) {
    return CameraSinkSubmit::Failed;
  }
  if (!available()) return CameraSinkSubmit::Failed;

  if (impl_->ring_writer != nullptr && impl_->ring_writer->valid() &&
      impl_->ring_writer->has_demand(camera_clock_us())) {
    (void)impl_->ring_writer->write(frame.bgra, frame.row_stride, frame.presentation_time_us);
  }

  // Copy the small borrowed view, not its pixels; the callback remains const
  // with respect to the caller's frame without casting away constness.
  CameraSinkFrame source = frame;
  const auto copy = [](void* context, std::span<std::byte> destination,
                        std::size_t destination_stride) noexcept -> bool {
    const auto& source = *static_cast<const CameraSinkFrame*>(context);
    const std::size_t row_bytes = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;
    for (std::uint32_t row = 0; row < kCanvas.height; ++row) {
      std::memcpy(destination.data() + static_cast<std::size_t>(row) * destination_stride,
                  source.bgra.data() + static_cast<std::size_t>(row) * source.row_stride, row_bytes);
    }
    return true;
  };
  return detail::submit_cmio_frame(impl_->pool, impl_->format, impl_->queue, impl_->depth,
                                   copy, &source);
}

auto CmioCameraSink::submit_written(CameraFrameWriter writer, void* context,
                                    std::uint64_t presentation_time_us) noexcept -> CameraSinkWrite {
  if (!available()) return CameraSinkWrite::Failed;

  CameraSinkWrite cmio_result = CameraSinkWrite::Failed;
  if (static_cast<std::size_t>(CMSimpleQueueGetCount(impl_->queue)) < impl_->depth) {
    struct DualContext {
      CameraFrameWriter writer;
      void* context;
      FrameRingWriter* ring_writer;
      std::uint64_t presentation_time_us;
    } dual{writer, context, impl_->ring_writer.get(), presentation_time_us};

    const auto dual_writer = [](void* opaque, std::span<std::byte> dest, std::size_t stride) noexcept -> bool {
      auto& d = *static_cast<DualContext*>(opaque);
      if (!d.writer(d.context, dest, stride)) return false;
      if (d.ring_writer != nullptr && d.ring_writer->valid() &&
          d.ring_writer->has_demand(camera_clock_us())) {
        (void)d.ring_writer->write(dest, stride, d.presentation_time_us);
      }
      return true;
    };

    switch (detail::submit_cmio_frame(impl_->pool, impl_->format, impl_->queue, impl_->depth,
                                      dual_writer, &dual)) {
      case CameraSinkSubmit::Accepted:
        cmio_result = CameraSinkWrite::Accepted;
        break;
      case CameraSinkSubmit::Backpressured:
        cmio_result = CameraSinkWrite::Backpressured;
        break;
      case CameraSinkSubmit::Failed:
        cmio_result = CameraSinkWrite::Failed;
        break;
    }
  }

  if (cmio_result == CameraSinkWrite::Accepted) {
    return CameraSinkWrite::Accepted;
  }

  // If CMIO queue is full or backpressured, but SHM consumer has demand, write directly to ring slot
  if (impl_->ring_writer != nullptr && impl_->ring_writer->valid() &&
      impl_->ring_writer->has_demand(camera_clock_us())) {
    if (impl_->ring_writer->write_with(writer, context, presentation_time_us)) {
      return CameraSinkWrite::Accepted;
    }
  }

  return cmio_result;
}

}  // namespace noisefactor::sync::camera
