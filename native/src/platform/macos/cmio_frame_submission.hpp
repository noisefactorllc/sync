#pragma once

#include <sync/platform/camera_identity.hpp>
#include <sync/platform/camera_sink.hpp>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

namespace noisefactor::sync::camera::detail {

struct WritableCameraFrame {
  std::span<std::byte> bytes;
  std::size_t stride = 0;
  std::size_t width = 0;
  std::size_t height = 0;
};

// Shared synchronous ownership flow; platform operations are supplied by the
// CMIO adapter. Unit tests inject failures without starting a camera stream.
template <typename Operations>
auto submit_camera_frame(Operations& operations, CameraFrameWriter writer, void* context) noexcept
    -> CameraSinkSubmit {
  if (writer == nullptr) return CameraSinkSubmit::Failed;
  if (!operations.has_capacity()) return CameraSinkSubmit::Backpressured;

  struct PixelOwner {
    Operations& operations;
    typename Operations::Pixels value = nullptr;
    bool locked = false;
    ~PixelOwner() {
      if (locked) (void)operations.unlock(value);
      if (value != nullptr) operations.release_pixels(value);
    }
    auto unlock() noexcept -> bool {
      // A failed unlock is terminal; do not issue a second unlock in cleanup.
      locked = false;
      return operations.unlock(value);
    }
  } pixels{operations};
  if (!operations.allocate(pixels.value) || pixels.value == nullptr) {
    return CameraSinkSubmit::Backpressured;
  }
  if (!operations.lock(pixels.value)) return CameraSinkSubmit::Failed;
  pixels.locked = true;
  const WritableCameraFrame destination = operations.destination(pixels.value);
  if (destination.bytes.data() == nullptr || destination.width != kCanvas.width ||
      destination.height != kCanvas.height ||
      destination.stride < static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel ||
      destination.stride > destination.bytes.size() / kCanvas.height) {
    return CameraSinkSubmit::Failed;
  }
  if (!writer(context, destination.bytes, destination.stride)) return CameraSinkSubmit::Failed;
  if (!pixels.unlock()) return CameraSinkSubmit::Failed;
  if (!operations.prepare_format(pixels.value)) return CameraSinkSubmit::Failed;

  struct SampleOwner {
    Operations& operations;
    typename Operations::Sample value = nullptr;
    ~SampleOwner() {
      if (value != nullptr) operations.release_sample(value);
    }
  } sample{operations};
  if (!operations.make_sample(pixels.value, sample.value) || sample.value == nullptr) {
    return CameraSinkSubmit::Failed;
  }
  if (!operations.enqueue(sample.value)) return CameraSinkSubmit::Backpressured;
  // Only a successful enqueue transfers the sample's retained reference.
  sample.value = nullptr;
  return CameraSinkSubmit::Accepted;
}

auto submit_cmio_frame(CVPixelBufferPoolRef pool, CMVideoFormatDescriptionRef& format,
                       CMSimpleQueueRef queue, std::size_t depth,
                       CameraFrameWriter writer, void* context) noexcept -> CameraSinkSubmit;

}  // namespace noisefactor::sync::camera::detail
