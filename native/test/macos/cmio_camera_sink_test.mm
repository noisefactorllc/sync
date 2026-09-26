#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <limits>

#include <sync/platform/camera_identity.hpp>
#include <sync/platform/cmio_camera_sink.hpp>
#include <sync/camera/frame_ring.hpp>
#include "../../src/platform/macos/cmio_frame_submission.hpp"
#import <Foundation/Foundation.h>
#include <sys/stat.h>

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
  CmioCameraSink sink({.device_uid = "io.noisefactor.sync.camera.does-not-exist",
                       .enable_shm = false});
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
  CmioCameraSink sink({.device_uid = "io.noisefactor.sync.camera.does-not-exist",
                       .enable_shm = false});
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

SYNC_TEST(cmio_camera_sink_initializes_shm_ring_file_with_owner_private_permissions) {
  const std::string test_path = "/tmp/SyncCamera.test." + std::to_string(::getpid()) + ".frames";
  ::unlink(test_path.c_str());
  {
    CmioCameraSink sink({.device_uid = "io.noisefactor.sync.camera.does-not-exist",
                         .shm_path = test_path,
                         .enable_shm = true});
    struct stat st{};
    SYNC_REQUIRE(::stat(test_path.c_str(), &st) == 0);
    SYNC_REQUIRE(S_ISREG(st.st_mode));
    SYNC_REQUIRE((st.st_mode & 0777) == 0600);
    SYNC_REQUIRE(st.st_uid == ::geteuid());
    SYNC_REQUIRE(static_cast<std::size_t>(st.st_size) == noisefactor::sync::camera::frame_ring_bytes());
  }
  struct stat st_after{};
  SYNC_REQUIRE(::stat(test_path.c_str(), &st_after) != 0);
}

// The sink must refuse to follow a symlinked shared-frame path: it opens
// with O_NOFOLLOW, so the target is neither truncated nor scrubbed through
// the link, and the sink leaves the link and target alone.
SYNC_TEST(cmio_camera_sink_rejects_symlink_shm_path) {
  const std::string target_path = "/tmp/SyncCamera.target." + std::to_string(::getpid()) + ".frames";
  const std::string link_path = "/tmp/SyncCamera.link." + std::to_string(::getpid()) + ".frames";
  ::unlink(link_path.c_str());
  ::unlink(target_path.c_str());

  int fd = ::open(target_path.c_str(), O_RDWR | O_CREAT, 0600);
  SYNC_REQUIRE(fd >= 0);
  const char canary[] = "CANARY_DATA_DO_NOT_OVERWRITE";
  SYNC_REQUIRE(::write(fd, canary, sizeof(canary)) == sizeof(canary));
  ::close(fd);

  SYNC_REQUIRE(::symlink(target_path.c_str(), link_path.c_str()) == 0);

  {
    CmioCameraSink sink({.device_uid = "io.noisefactor.sync.camera.does-not-exist",
                         .shm_path = link_path,
                         .enable_shm = true});
  }

  // Target must NOT have been truncated or modified through the symlink
  struct stat st{};
  SYNC_REQUIRE(::stat(target_path.c_str(), &st) == 0);
  SYNC_REQUIRE(st.st_size == sizeof(canary));

  ::unlink(link_path.c_str());
  ::unlink(target_path.c_str());
}

SYNC_TEST(cmio_camera_sink_preserves_a_replacement_file_on_close) {
  const std::string test_path = "/tmp/SyncCamera.replaced." + std::to_string(::getpid()) + ".frames";
  const std::string original_path = test_path + ".original";
  const char canary[] = "CANARY_DATA_DO_NOT_OVERWRITE";
  {
    CmioCameraSink sink({.device_uid = "io.noisefactor.sync.camera.does-not-exist",
                         .shm_path = test_path,
                         .enable_shm = true});
    SYNC_REQUIRE(::rename(test_path.c_str(), original_path.c_str()) == 0);
    const int fd = ::open(test_path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    SYNC_REQUIRE(fd >= 0);
    SYNC_REQUIRE(::write(fd, canary, sizeof(canary)) == sizeof(canary));
    ::close(fd);
  }
  const int fd = ::open(test_path.c_str(), O_RDONLY | O_NOFOLLOW);
  char actual[sizeof(canary)]{};
  const auto count = fd < 0 ? -1 : ::read(fd, actual, sizeof(actual));
  if (fd >= 0) ::close(fd);
  ::unlink(test_path.c_str());
  ::unlink(original_path.c_str());
  SYNC_REQUIRE(count == sizeof(canary));
  SYNC_REQUIRE(std::memcmp(actual, canary, sizeof(canary)) == 0);
}

SYNC_TEST(cmio_camera_sink_second_writer_cannot_scrub_the_first_writer) {
  const std::string test_path = "/tmp/SyncCamera.owner." + std::to_string(::getpid()) + ".frames";
  CmioCameraSink first({.device_uid = "io.noisefactor.sync.camera.does-not-exist",
                        .shm_path = test_path, .enable_shm = true});
  const int fd = ::open(test_path.c_str(), O_RDONLY | O_NOFOLLOW);
  SYNC_REQUIRE(fd >= 0);
  std::uint32_t before = 0;
  SYNC_REQUIRE(::pread(fd, &before, sizeof(before), 0) == sizeof(before));
  {
    CmioCameraSink second({.device_uid = "io.noisefactor.sync.camera.does-not-exist",
                           .shm_path = test_path, .enable_shm = true});
  }
  std::uint32_t after = 0;
  const auto count = ::pread(fd, &after, sizeof(after), 0);
  ::close(fd);
  struct stat st{};
  SYNC_REQUIRE(::stat(test_path.c_str(), &st) == 0);
  SYNC_REQUIRE(count == sizeof(after));
  SYNC_REQUIRE(before == noisefactor::sync::camera::kFrameRingMagic);
  SYNC_REQUIRE(after == before);
}

SYNC_TEST(cmio_camera_sink_rejects_non_regular_file_shm_path) {
  const std::string dir_path = "/tmp/SyncCamera.dir." + std::to_string(::getpid()) + ".frames";
  ::rmdir(dir_path.c_str());
  SYNC_REQUIRE(::mkdir(dir_path.c_str(), 0700) == 0);

  {
    CmioCameraSink sink({.device_uid = "io.noisefactor.sync.camera.does-not-exist",
                         .shm_path = dir_path,
                         .enable_shm = true});
  }

  struct stat st{};
  SYNC_REQUIRE(::stat(dir_path.c_str(), &st) == 0);
  SYNC_REQUIRE(S_ISDIR(st.st_mode));

  ::rmdir(dir_path.c_str());
}

SYNC_TEST(cmio_camera_sink_tightens_preexisting_loose_permissions) {
  const std::string test_path = "/tmp/SyncCamera.loose." + std::to_string(::getpid()) + ".frames";
  ::unlink(test_path.c_str());
  int fd = ::open(test_path.c_str(), O_RDWR | O_CREAT, 0666);
  SYNC_REQUIRE(fd >= 0);
  SYNC_REQUIRE(::fchmod(fd, 0666) == 0);
  ::close(fd);

  struct stat st_before{};
  SYNC_REQUIRE(::stat(test_path.c_str(), &st_before) == 0);
  SYNC_REQUIRE((st_before.st_mode & 0777) == 0666);

  {
    CmioCameraSink sink({.device_uid = "io.noisefactor.sync.camera.does-not-exist",
                         .shm_path = test_path,
                         .enable_shm = true});
    struct stat st_during{};
    SYNC_REQUIRE(::stat(test_path.c_str(), &st_during) == 0);
    SYNC_REQUIRE((st_during.st_mode & 0777) == 0600);
  }

  struct stat st_after{};
  SYNC_REQUIRE(::stat(test_path.c_str(), &st_after) != 0);
}

SYNC_TEST(cmio_camera_sink_scrubs_pixels_and_cleans_up_on_close) {
  const std::string test_path = "/tmp/SyncCamera.scrub." + std::to_string(::getpid()) + ".frames";
  ::unlink(test_path.c_str());

  int fd_keep = -1;
  void* view_keep = nullptr;
  const std::size_t ring_bytes = noisefactor::sync::camera::frame_ring_bytes();

  {
    CmioCameraSink sink({.device_uid = "io.noisefactor.sync.camera.does-not-exist",
                         .shm_path = test_path,
                         .enable_shm = true});

    fd_keep = ::open(test_path.c_str(), O_RDWR | O_NOFOLLOW);
    SYNC_REQUIRE(fd_keep >= 0);
    view_keep = ::mmap(nullptr, ring_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_keep, 0);
    SYNC_REQUIRE(view_keep != MAP_FAILED);

    auto* header = reinterpret_cast<noisefactor::sync::camera::FrameRingHeader*>(view_keep);
    SYNC_REQUIRE(header->magic == noisefactor::sync::camera::kFrameRingMagic);
    auto* payload = static_cast<std::byte*>(view_keep) + sizeof(noisefactor::sync::camera::FrameRingHeader);
    std::memset(payload, 0xEE, 4096);
    SYNC_REQUIRE(static_cast<unsigned char>(payload[0]) == 0xEE);
  }

  struct stat st{};
  SYNC_REQUIRE(::stat(test_path.c_str(), &st) != 0);

  auto* payload = static_cast<const std::byte*>(view_keep) + sizeof(noisefactor::sync::camera::FrameRingHeader);
  bool all_zero = true;
  for (std::size_t i = 0; i < 4096; ++i) {
    if (payload[i] != std::byte{0}) {
      all_zero = false;
      break;
    }
  }
  SYNC_REQUIRE(all_zero);

  auto* header = reinterpret_cast<const noisefactor::sync::camera::FrameRingHeader*>(view_keep);
  SYNC_REQUIRE(header->magic == 0);

  ::munmap(view_keep, ring_bytes);
  ::close(fd_keep);
}

SYNC_TEST(frame_ring_writer_gates_publication_on_consumer_demand) {
  const std::size_t bytes = noisefactor::sync::camera::frame_ring_bytes();
  std::vector<std::byte> storage(bytes, std::byte{0});

  noisefactor::sync::camera::FrameRingWriter writer(storage);
  SYNC_REQUIRE(writer.valid());

  noisefactor::sync::camera::FrameRingReader reader(storage);
  SYNC_REQUIRE(reader.valid());

  const auto now_us = noisefactor::sync::camera::camera_clock_us();
  SYNC_REQUIRE(!writer.has_demand(now_us));

  std::vector<std::byte> frame_data(noisefactor::sync::camera::kFrameRingSlotBytes, std::byte{0x42});
  const std::size_t row_stride = static_cast<std::size_t>(noisefactor::sync::camera::kCanvas.width) * 4;

  if (writer.has_demand(now_us)) {
    (void)writer.write(frame_data, row_stride, now_us);
  }
  SYNC_REQUIRE(reader.newest_sequence() == 0);

  reader.record_demand(now_us);
  SYNC_REQUIRE(writer.has_demand(now_us));

  if (writer.has_demand(now_us)) {
    SYNC_REQUIRE(writer.write(frame_data, row_stride, now_us));
  }
  SYNC_REQUIRE(reader.newest_sequence() == 1);

  const auto future_us = now_us + noisefactor::sync::camera::kFrameRingDemandTimeoutUs + 500'000;
  SYNC_REQUIRE(!writer.has_demand(future_us));
}
