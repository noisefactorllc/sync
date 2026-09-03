#include <sync/platform/cmio_camera_sink.hpp>

#import <CoreFoundation/CoreFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreMediaIO/CMIOHardware.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <mach/mach_time.h>

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
  explicit Impl(Options options) : depth(options.queue_depth == 0 ? 1 : options.queue_depth) {
    discover(std::string(options.device_uid));
  }

  ~Impl() {
    if (started) CMIODeviceStopStream(device, stream);
    if (queue != nullptr) CFRelease(queue);
    if (format != nullptr) CFRelease(format);
    if (pool != nullptr) CVPixelBufferPoolRelease(pool);
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

auto CmioCameraSink::available() const noexcept -> bool {
  return impl_->reason == CameraSinkUnavailableReason::None;
}

auto CmioCameraSink::unavailable_reason() const noexcept -> CameraSinkUnavailableReason {
  return impl_->reason;
}

auto CmioCameraSink::unavailable_status() const noexcept -> std::int32_t {
  return impl_->status;
}

auto CmioCameraSink::submit(const CameraSinkFrame& frame) noexcept -> CameraSinkSubmit {
  if (frame.width != kCanvas.width || frame.height != kCanvas.height ||
      frame.row_stride < static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel ||
      frame.bgra.size() < frame.row_stride * kCanvas.height) {
    return CameraSinkSubmit::Failed;
  }
  if (!available()) return CameraSinkSubmit::Failed;
  if (static_cast<std::size_t>(CMSimpleQueueGetCount(impl_->queue)) >= impl_->depth) {
    return CameraSinkSubmit::Backpressured;
  }

  CVPixelBufferRef pixels = nullptr;
  if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, impl_->pool, &pixels) !=
          kCVReturnSuccess ||
      pixels == nullptr) {
    return CameraSinkSubmit::Backpressured;
  }
  CVPixelBufferLockBaseAddress(pixels, 0);
  auto* destination = static_cast<std::byte*>(CVPixelBufferGetBaseAddress(pixels));
  const std::size_t destination_stride = CVPixelBufferGetBytesPerRow(pixels);
  const std::size_t row_bytes = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;
  for (std::uint32_t row = 0; row < kCanvas.height; ++row) {
    std::memcpy(destination + static_cast<std::size_t>(row) * destination_stride,
                frame.bgra.data() + static_cast<std::size_t>(row) * frame.row_stride, row_bytes);
  }
  CVPixelBufferUnlockBaseAddress(pixels, 0);
  CVBufferSetAttachment(pixels, kCVImageBufferColorPrimariesKey,
                        kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
  CVBufferSetAttachment(pixels, kCVImageBufferTransferFunctionKey,
                        kCVImageBufferTransferFunction_sRGB, kCVAttachmentMode_ShouldPropagate);

  if (impl_->format == nullptr &&
      CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixels, &impl_->format) !=
          noErr) {
    impl_->format = nullptr;
    CVPixelBufferRelease(pixels);
    return CameraSinkSubmit::Failed;
  }
  const CMTime pts = CMTimeMake(static_cast<int64_t>(host_time_ns()), 1'000'000'000);
  CMSampleTimingInfo timing{
      .duration = CMTimeMake(1, static_cast<int32_t>(kMaximumFramesPerSecond)),
      .presentationTimeStamp = pts,
      .decodeTimeStamp = kCMTimeInvalid,
  };
  CMSampleBufferRef sample = nullptr;
  const OSStatus status = CMSampleBufferCreateForImageBuffer(
      kCFAllocatorDefault, pixels, true, nullptr, nullptr, impl_->format, &timing, &sample);
  CVPixelBufferRelease(pixels);
  if (status != noErr || sample == nullptr) return CameraSinkSubmit::Failed;
  // The queue takes over this reference: the extension releases the buffer
  // after consuming it, so there is deliberately no CFRelease on success.
  if (CMSimpleQueueEnqueue(impl_->queue, sample) != noErr) {
    CFRelease(sample);
    return CameraSinkSubmit::Backpressured;
  }
  return CameraSinkSubmit::Accepted;
}

}  // namespace noisefactor::sync::camera
