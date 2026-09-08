#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <limits>

#include <sync/platform/camera_identity.hpp>
#include <sync/platform/cmio_camera_sink.hpp>
#include "../../src/platform/macos/cmio_frame_submission.hpp"
#import <Foundation/Foundation.h>

// A nonexistent UID keeps discovery tests isolated even on a machine with the
// real camera installed. Discovery must fail quietly, the reason must be the
// one that tells a user to approve the extension, and submit() must refuse
// frames rather than touch a queue that was never obtained.

namespace {

using noisefactor::sync::camera::CameraSinkFrame;
using noisefactor::sync::camera::CameraSinkSubmit;
using noisefactor::sync::camera::CameraSinkUnavailableReason;
using noisefactor::sync::camera::CmioCameraSink;
using noisefactor::sync::camera::kCanvas;

}  // namespace

SYNC_TEST(cmio_camera_sink_without_the_extension_is_unavailable_and_refuses_frames) {
  CmioCameraSink sink({.device_uid = "io.noisefactor.sync.camera.does-not-exist"});
  SYNC_REQUIRE(!sink.available());
  SYNC_REQUIRE(sink.unavailable_reason() == CameraSinkUnavailableReason::DeviceNotFound);
  std::vector<std::byte> bytes(static_cast<std::size_t>(kCanvas.width) * kCanvas.height * 4,
                               std::byte{0});
  const CameraSinkFrame frame{
      .width = kCanvas.width,
      .height = kCanvas.height,
      .row_stride = static_cast<std::size_t>(kCanvas.width) * 4,
      .bgra = bytes,
      .presentation_time_us = 1,
  };
  SYNC_REQUIRE(sink.submit(frame) == CameraSinkSubmit::Failed);
}

SYNC_TEST(cmio_camera_sink_rejects_frames_that_are_not_the_advertised_canvas) {
  CmioCameraSink sink({.device_uid = "io.noisefactor.sync.camera.does-not-exist"});
  std::array<std::byte, 16> bytes{};
  const CameraSinkFrame frame{
      .width = 2, .height = 2, .row_stride = 8, .bgra = bytes, .presentation_time_us = 1};
  SYNC_REQUIRE(sink.submit(frame) == CameraSinkSubmit::Failed);
}

namespace {
using noisefactor::sync::camera::detail::WritableCameraFrame;
using noisefactor::sync::camera::detail::submit_camera_frame;

enum class Fault { None, Capacity, Allocation, AllocationNull, AllocationWithRef, Lock, NullBase,
                   ShortStride, OverflowStride, ShortExtent, WrongWidth, WrongHeight,
                   Writer, Unlock, Format, Sample, SampleNull, SampleWithRef, Enqueue };
struct SubmissionOperations {
  using Pixels = int*;
  using Sample = long*;
  Fault fault;
  int pixel_token = 0;
  long sample_token = 0;
  int allocations = 0, locks = 0, unlocks = 0, pixel_releases = 0;
  int formats = 0, samples = 0, sample_releases = 0, enqueues = 0, writers = 0;
  bool held_lock = false, writer_saw_lock = false, sample_saw_unlocked = false;
  std::vector<std::byte> bytes = std::vector<std::byte>(7744 * 1080, std::byte{0xA7});
  explicit SubmissionOperations(Fault f) : fault(f) {}
  bool has_capacity() const noexcept { return fault != Fault::Capacity; }
  bool allocate(Pixels& out) noexcept {
    ++allocations;
    if (fault == Fault::Allocation) return false;
    if (fault == Fault::AllocationNull) return true;
    out = &pixel_token;
    return fault != Fault::AllocationWithRef;
  }
  bool lock(Pixels) noexcept { ++locks; held_lock = fault != Fault::Lock; return held_lock; }
  bool unlock(Pixels) noexcept { ++unlocks; held_lock = false; return fault != Fault::Unlock; }
  void release_pixels(Pixels) noexcept { ++pixel_releases; }
  WritableCameraFrame destination(Pixels) noexcept {
    if (fault == Fault::NullBase) return {};
    return {.bytes = fault == Fault::ShortExtent ? std::span<std::byte>(bytes).first(8) : bytes,
            .stride = fault == Fault::ShortStride ? 1U : fault == Fault::OverflowStride
                ? std::numeric_limits<std::size_t>::max() : 7744U,
            .width = fault == Fault::WrongWidth ? 2U : 1920U,
            .height = fault == Fault::WrongHeight ? 1U : 1080U};
  }
  bool prepare_format(Pixels) noexcept { ++formats; return fault != Fault::Format; }
  bool make_sample(Pixels, Sample& out) noexcept {
    ++samples;
    sample_saw_unlocked = !held_lock;
    if (fault == Fault::Sample) return false;
    if (fault == Fault::SampleNull) return true;
    out = &sample_token;
    return fault != Fault::SampleWithRef;
  }
  bool enqueue(Sample) noexcept { ++enqueues; return fault != Fault::Enqueue; }
  void release_sample(Sample) noexcept { ++sample_releases; }
  static bool write(void* context, std::span<std::byte> bytes, std::size_t stride) noexcept {
    auto& self = *static_cast<SubmissionOperations*>(context);
    ++self.writers; self.writer_saw_lock = self.held_lock;
    bytes[0] = std::byte{17}; bytes[(1080 - 1) * stride + 7679] = std::byte{29};
    return self.fault != Fault::Writer;
  }
};
}  // namespace

SYNC_TEST(cmio_submission_scopes_writing_then_transfers_exactly_one_sample_reference) {
  SubmissionOperations ops(Fault::None);
  SYNC_REQUIRE(submit_camera_frame(ops, &SubmissionOperations::write, &ops) == CameraSinkSubmit::Accepted);
  SYNC_REQUIRE(ops.allocations == 1 && ops.locks == 1 && ops.unlocks == 1);
  SYNC_REQUIRE(ops.writers == 1 && ops.writer_saw_lock && ops.sample_saw_unlocked);
  SYNC_REQUIRE(ops.formats == 1 && ops.samples == 1 && ops.enqueues == 1);
  SYNC_REQUIRE(ops.pixel_releases == 1 && ops.sample_releases == 0);
  SYNC_REQUIRE(ops.bytes[0] == std::byte{17} && ops.bytes[1079 * 7744 + 7679] == std::byte{29});
  for (std::size_t row = 0; row < 1080; ++row) {
    SYNC_REQUIRE(std::all_of(ops.bytes.begin() + row * 7744 + 7680,
                            ops.bytes.begin() + (row + 1) * 7744,
                            [](auto byte) { return byte == std::byte{0xA7}; }));
  }
}

SYNC_TEST(cmio_submission_cleans_every_failed_resource_boundary_without_publication) {
  for (auto fault : {Fault::Capacity, Fault::Allocation, Fault::AllocationNull,
                     Fault::AllocationWithRef, Fault::Lock, Fault::NullBase, Fault::ShortStride,
                     Fault::OverflowStride, Fault::ShortExtent, Fault::WrongWidth, Fault::WrongHeight,
                     Fault::Writer, Fault::Unlock, Fault::Format, Fault::Sample, Fault::SampleNull,
                     Fault::SampleWithRef, Fault::Enqueue}) {
    SubmissionOperations ops(fault);
    const bool early = fault == Fault::Capacity || fault == Fault::Allocation ||
                       fault == Fault::AllocationNull || fault == Fault::AllocationWithRef;
    const auto expected = early || fault == Fault::Enqueue ? CameraSinkSubmit::Backpressured
                                                         : CameraSinkSubmit::Failed;
    SYNC_REQUIRE(submit_camera_frame(ops, &SubmissionOperations::write, &ops) == expected);
    const bool owns_pixels = fault != Fault::Capacity && fault != Fault::Allocation && fault != Fault::AllocationNull;
    const bool locked = owns_pixels && fault != Fault::AllocationWithRef && fault != Fault::Lock;
    SYNC_REQUIRE(ops.pixel_releases == (owns_pixels ? 1 : 0));
    SYNC_REQUIRE(ops.unlocks == (locked ? 1 : 0));
    SYNC_REQUIRE(!ops.held_lock);
    SYNC_REQUIRE(ops.enqueues == (fault == Fault::Enqueue ? 1 : 0));
    SYNC_REQUIRE(ops.sample_releases == (fault == Fault::SampleWithRef || fault == Fault::Enqueue ? 1 : 0));
    if (early || fault == Fault::Lock || fault == Fault::NullBase || fault == Fault::ShortStride ||
        fault == Fault::OverflowStride || fault == Fault::ShortExtent || fault == Fault::WrongWidth ||
        fault == Fault::WrongHeight) SYNC_REQUIRE(ops.writers == 0);
    else SYNC_REQUIRE(ops.writers == 1);
  }
}

SYNC_TEST(cmio_submission_rejects_a_missing_writer_before_allocating) {
  SubmissionOperations ops(Fault::None);
  SYNC_REQUIRE(submit_camera_frame(ops, nullptr, &ops) == CameraSinkSubmit::Failed);
  SYNC_REQUIRE(ops.allocations == 0 && ops.writers == 0 && ops.enqueues == 0);
}

namespace {
struct LocalFrameResources {
  CVPixelBufferPoolRef pool = nullptr;
  CMVideoFormatDescriptionRef format = nullptr;
  CMSimpleQueueRef queue = nullptr;
  ~LocalFrameResources() {
    if (queue) {
      while (const void* sample = CMSimpleQueueDequeue(queue)) CFRelease(sample);
      CFRelease(queue);
    }
    if (format) CFRelease(format);
    if (pool) CVPixelBufferPoolRelease(pool);
  }
  void initialize() {
    NSDictionary* attributes = @{
      (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
      (id)kCVPixelBufferWidthKey: @1920, (id)kCVPixelBufferHeightKey: @1080,
      (id)kCVPixelBufferBytesPerRowAlignmentKey: @4096,
      (id)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
    SYNC_REQUIRE(CVPixelBufferPoolCreate(nullptr, nullptr, (__bridge CFDictionaryRef)attributes,
                                        &pool) == kCVReturnSuccess);
    SYNC_REQUIRE(CMSimpleQueueCreate(nullptr, 1, &queue) == noErr);
  }
};
struct LocalWriter {
  bool succeed = true;
  int calls = 0;
  std::size_t actual_stride = 0;
  static bool write(void* context, std::span<std::byte> output, std::size_t stride) noexcept {
    auto& self = *static_cast<LocalWriter*>(context);
    ++self.calls;
    self.actual_stride = stride;
    std::fill(output.begin(), output.end(), std::byte{0xA7});
    for (std::size_t y = 0; y < 1080; ++y) for (std::size_t x = 0; x < 1920; ++x) {
      const auto offset = y * stride + x * 4;
      output[offset] = std::byte{17}; output[offset + 1] = std::byte{31};
      output[offset + 2] = std::byte{47}; output[offset + 3] = std::byte{255};
    }
    return self.succeed;
  }
};
}  // namespace

SYNC_TEST(cmio_submission_uses_real_pooled_surfaces_and_preserves_sample_metadata_without_a_device) {
  LocalFrameResources resources;
  resources.initialize();
  LocalWriter writer;
  using noisefactor::sync::camera::detail::submit_cmio_frame;
  SYNC_REQUIRE(submit_cmio_frame(resources.pool, resources.format, resources.queue, 1,
                                 &LocalWriter::write, &writer) == CameraSinkSubmit::Accepted);
  SYNC_REQUIRE(writer.calls == 1 && writer.actual_stride >= 7680);
  SYNC_REQUIRE(CMSimpleQueueGetCount(resources.queue) == 1);
  SYNC_REQUIRE(submit_cmio_frame(resources.pool, resources.format, resources.queue, 1,
                                 &LocalWriter::write, &writer) == CameraSinkSubmit::Backpressured);
  SYNC_REQUIRE(writer.calls == 1);
  auto sample = static_cast<CMSampleBufferRef>(const_cast<void*>(CMSimpleQueueGetHead(resources.queue)));
  SYNC_REQUIRE(sample != nullptr);
  auto pixels = CMSampleBufferGetImageBuffer(sample);
  SYNC_REQUIRE(pixels != nullptr && CVPixelBufferGetIOSurface(pixels) != nullptr);
  SYNC_REQUIRE(CMTimeCompare(CMSampleBufferGetDuration(sample), CMTimeMake(1, 60)) == 0);
  SYNC_REQUIRE(CMTIME_IS_NUMERIC(CMSampleBufferGetPresentationTimeStamp(sample)));
  const auto primaries = CVBufferCopyAttachment(pixels, kCVImageBufferColorPrimariesKey, nullptr);
  const auto transfer = CVBufferCopyAttachment(pixels, kCVImageBufferTransferFunctionKey, nullptr);
  const bool attachments_match = primaries != nullptr && transfer != nullptr &&
      CFEqual(primaries, kCVImageBufferColorPrimaries_ITU_R_709_2) &&
      CFEqual(transfer, kCVImageBufferTransferFunction_sRGB);
  if (primaries) CFRelease(primaries);
  if (transfer) CFRelease(transfer);
  SYNC_REQUIRE(attachments_match);
  SYNC_REQUIRE(CMFormatDescriptionGetMediaSubType(resources.format) == kCVPixelFormatType_32BGRA);
  SYNC_REQUIRE(CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess);
  const auto* bytes = static_cast<const std::byte*>(CVPixelBufferGetBaseAddress(pixels));
  bool equal = true;
  for (std::size_t y = 0; y < 1080; ++y) for (std::size_t x = 0; x < 1920; ++x) {
    const auto offset = y * writer.actual_stride + x * 4;
    equal = equal && bytes[offset] == std::byte{17} && bytes[offset + 1] == std::byte{31} &&
            bytes[offset + 2] == std::byte{47} && bytes[offset + 3] == std::byte{255};
  }
  SYNC_REQUIRE(CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) == kCVReturnSuccess);
  SYNC_REQUIRE(equal);
}

SYNC_TEST(cmio_submission_cancelled_writer_returns_the_real_pixel_buffer_to_its_pool) {
  LocalFrameResources resources;
  resources.initialize();
  LocalWriter writer;
  writer.succeed = false;
  using noisefactor::sync::camera::detail::submit_cmio_frame;
  SYNC_REQUIRE(submit_cmio_frame(resources.pool, resources.format, resources.queue, 1,
                                 &LocalWriter::write, &writer) == CameraSinkSubmit::Failed);
  SYNC_REQUIRE(writer.calls == 1 && CMSimpleQueueGetCount(resources.queue) == 0);
  NSDictionary* threshold = @{(id)kCVPixelBufferPoolAllocationThresholdKey: @1};
  CVPixelBufferRef recycled = nullptr;
  // One allocated buffer already exists. A leaked +1 reference would prevent
  // reuse and cause this threshold-one acquisition to fail.
  const auto status = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(
      nullptr, resources.pool, (__bridge CFDictionaryRef)threshold, &recycled);
  if (recycled) CVPixelBufferRelease(recycled);
  SYNC_REQUIRE(status == kCVReturnSuccess);
}
