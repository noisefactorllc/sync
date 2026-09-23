#include <sync/h264_decoder.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#endif

namespace noisefactor::sync {
namespace {

#if defined(__APPLE__)
struct NalView {
  std::span<const std::byte> bytes;
  std::uint8_t type = 0;
};

auto start_code_length(std::span<const std::byte> data, std::size_t offset) noexcept
    -> std::size_t {
  if (offset + 4 <= data.size() && data[offset] == std::byte{0} &&
      data[offset + 1] == std::byte{0} && data[offset + 2] == std::byte{0} &&
      data[offset + 3] == std::byte{1}) return 4;
  if (offset + 3 <= data.size() && data[offset] == std::byte{0} &&
      data[offset + 1] == std::byte{0} && data[offset + 2] == std::byte{1}) return 3;
  return 0;
}

auto parse_annexb(std::span<const std::byte> data,
                  std::array<NalView, 128>& nals,
                  std::size_t& count) noexcept -> bool {
  count = 0;
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t prefix = start_code_length(data, offset);
    if (prefix == 0 || count == nals.size()) return false;
    const std::size_t begin = offset + prefix;
    if (begin >= data.size()) return false;
    std::size_t end = begin;
    while (end < data.size() && start_code_length(data, end) == 0) ++end;
    // Annex B allows trailing_zero_8bits after a NAL.
    while (end > begin + 1 && data[end - 1] == std::byte{0}) --end;
    if (end <= begin) return false;
    const auto header = std::to_integer<std::uint8_t>(data[begin]);
    const auto type = static_cast<std::uint8_t>(header & 0x1fU);
    if ((header & 0x80U) != 0 || type == 0 || type > 23) return false;
    nals[count++] = NalView{data.subspan(begin, end - begin), type};
    // Resume at the start code; do not skip its zero bytes when trimming.
    offset = begin + nals[count - 1].bytes.size();
    while (offset < data.size() && start_code_length(data, offset) == 0) ++offset;
  }
  return count > 0;
}

struct DecodeOutput {
  std::span<std::byte> rgba;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool produced = false;
};

void decoded_frame(void*, void* context, OSStatus status, VTDecodeInfoFlags info,
                   CVImageBufferRef image, CMTime, CMTime) noexcept {
  auto* output = static_cast<DecodeOutput*>(context);
  if (!output || status != noErr || (info & kVTDecodeInfo_FrameDropped) != 0 || !image) return;
  auto pixel = static_cast<CVPixelBufferRef>(image);
  if (CVPixelBufferGetWidth(pixel) != output->width ||
      CVPixelBufferGetHeight(pixel) != output->height ||
      CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
  const vImage_Buffer destination{output->rgba.data(), output->height,
                                  output->width, output->width * 4U};
  if (CVPixelBufferGetPixelFormatType(pixel) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange &&
      CVPixelBufferGetPlaneCount(pixel) == 2) {
    struct Conversion {
      vImage_YpCbCrToARGB info{};
      bool valid = false;
    };
    static const Conversion conversion = []() noexcept {
      Conversion value;
      const vImage_YpCbCrPixelRange range{16, 128, 235, 240, 255, 0, 255, 0};
      value.valid = vImageConvert_YpCbCrToARGB_GenerateConversion(
          kvImage_YpCbCrToARGBMatrix_ITU_R_709_2, &range, &value.info,
          kvImage420Yp8_CbCr8, kvImageARGB8888, kvImageNoFlags) == kvImageNoError;
      return value;
    }();
    if (conversion.valid) {
      const vImage_Buffer y{CVPixelBufferGetBaseAddressOfPlane(pixel, 0), output->height,
                            output->width, CVPixelBufferGetBytesPerRowOfPlane(pixel, 0)};
      const vImage_Buffer uv{CVPixelBufferGetBaseAddressOfPlane(pixel, 1), output->height / 2U,
                             output->width / 2U, CVPixelBufferGetBytesPerRowOfPlane(pixel, 1)};
      constexpr std::uint8_t permutation[4] = {1, 2, 3, 0};
      output->produced = vImageConvert_420Yp8_CbCr8ToARGB8888(
          &y, &uv, &destination, &conversion.info, permutation, 255,
          kvImageNoFlags) == kvImageNoError;
    }
  } else if (CVPixelBufferGetPixelFormatType(pixel) == kCVPixelFormatType_32BGRA) {
    const vImage_Buffer source{CVPixelBufferGetBaseAddress(pixel), output->height,
                               output->width, CVPixelBufferGetBytesPerRow(pixel)};
    constexpr std::uint8_t permutation[4] = {2, 1, 0, 3};
    output->produced = vImagePermuteChannels_ARGB8888(
        &source, &destination, permutation, kvImageNoFlags) == kvImageNoError;
  }
  CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
}
#endif

}  // namespace

struct H264Decoder::Impl {
#if defined(__APPLE__)
  VTDecompressionSessionRef session = nullptr;
  CMVideoFormatDescriptionRef description = nullptr;
#endif
  std::vector<std::byte> sps;
  std::vector<std::byte> pps;
  std::vector<std::byte> packet;
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  ~Impl() { reset(); }

  void reset() noexcept {
#if defined(__APPLE__)
    if (session) {
      VTDecompressionSessionInvalidate(session);
      CFRelease(session);
      session = nullptr;
    }
    if (description) {
      CFRelease(description);
      description = nullptr;
    }
#endif
    width = 0;
    height = 0;
  }

#if defined(__APPLE__)
  auto configure(std::uint32_t next_width, std::uint32_t next_height) noexcept -> bool {
    reset();
    if (sps.empty() || pps.empty()) return false;
    const std::uint8_t* parameter_sets[] = {
        reinterpret_cast<const std::uint8_t*>(sps.data()),
        reinterpret_cast<const std::uint8_t*>(pps.data())};
    const std::size_t parameter_sizes[] = {sps.size(), pps.size()};
    if (CMVideoFormatDescriptionCreateFromH264ParameterSets(
            kCFAllocatorDefault, 2, parameter_sets, parameter_sizes, 4,
            &description) != noErr) return false;
    const auto dimensions = CMVideoFormatDescriptionGetDimensions(description);
    if (dimensions.width != static_cast<std::int32_t>(next_width) ||
        dimensions.height != static_cast<std::int32_t>(next_height)) return false;
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    const std::int32_t format = static_cast<std::int32_t>(
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
    CFNumberRef format_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &format);
    if (!attributes || !format_number) {
      if (format_number) CFRelease(format_number);
      if (attributes) CFRelease(attributes);
      return false;
    }
    CFDictionarySetValue(attributes, kCVPixelBufferPixelFormatTypeKey, format_number);
    CFRelease(format_number);
    const VTDecompressionOutputCallbackRecord callback{decoded_frame, nullptr};
    const OSStatus status = VTDecompressionSessionCreate(
        kCFAllocatorDefault, description, nullptr, attributes, &callback, &session);
    CFRelease(attributes);
    if (status != noErr || !session) return false;
    width = next_width;
    height = next_height;
    return true;
  }

  auto decode_packet(const protocol::FrameView& frame,
                     std::span<std::byte> rgba) noexcept -> bool {
    CMBlockBufferRef block = nullptr;
    if (CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault, packet.data(), packet.size(), kCFAllocatorNull,
            nullptr, 0, packet.size(), 0, &block) != noErr) return false;
    CMSampleBufferRef sample = nullptr;
    const std::size_t packet_size = packet.size();
    CMSampleTimingInfo timing{};
    timing.duration = kCMTimeInvalid;
    timing.presentationTimeStamp = CMTimeMake(
        static_cast<std::int64_t>(frame.presentation_time_us), 1000000);
    timing.decodeTimeStamp = kCMTimeInvalid;
    const OSStatus sample_status = CMSampleBufferCreateReady(
        kCFAllocatorDefault, block, description, 1, 1, &timing, 1,
        &packet_size, &sample);
    CFRelease(block);
    if (sample_status != noErr || !sample) return false;
    DecodeOutput output{rgba, frame.width, frame.height, false};
    VTDecodeInfoFlags info = 0;
    const OSStatus status = VTDecompressionSessionDecodeFrame(
        session, sample, 0, &output, &info);
    CFRelease(sample);
    return status == noErr && (info & kVTDecodeInfo_FrameDropped) == 0 &&
           output.produced;
  }
#endif
};

H264Decoder::H264Decoder() noexcept : impl_(new (std::nothrow) Impl) {}
H264Decoder::~H264Decoder() = default;

auto H264Decoder::decode(const protocol::FrameView& frame,
                         std::span<std::byte> output) noexcept -> bool {
#if !defined(__APPLE__)
  (void)frame;
  (void)output;
  return false;
#else
  if (!impl_ || frame.pixel_format != 3 || frame.payload.empty() ||
      output.size() != static_cast<std::size_t>(frame.width) * frame.height * 4U) return false;
  try {
    std::array<NalView, 128> nals{};
    std::size_t count = 0;
    if (!parse_annexb(frame.payload, nals, count)) return false;
    std::span<const std::byte> next_sps;
    std::span<const std::byte> next_pps;
    bool has_idr = false;
    bool has_picture = false;
    std::size_t packet_bytes = 0;
    for (std::size_t index = 0; index < count; ++index) {
      const NalView& nal = nals[index];
      if (nal.type == 7) next_sps = nal.bytes;
      if (nal.type == 8) next_pps = nal.bytes;
      if (nal.type == 5) has_idr = true;
      if (nal.type == 1 || nal.type == 5) has_picture = true;
      if (nal.type != 7 && nal.type != 8 && nal.type != 9) {
        packet_bytes += 4U + nal.bytes.size();
      }
    }
    if (!has_picture || packet_bytes == 0 || packet_bytes > 8U * 1024U * 1024U) return false;
    if ((next_sps.size() > 4096) || (next_pps.size() > 4096)) return false;
    const bool sets_changed = (!next_sps.empty() &&
        (next_sps.size() != impl_->sps.size() ||
         std::memcmp(next_sps.data(), impl_->sps.data(), next_sps.size()) != 0)) ||
        (!next_pps.empty() &&
         (next_pps.size() != impl_->pps.size() ||
          std::memcmp(next_pps.data(), impl_->pps.data(), next_pps.size()) != 0));
    if (sets_changed && !has_idr) return false;
    if (!next_sps.empty()) impl_->sps.assign(next_sps.begin(), next_sps.end());
    if (!next_pps.empty()) impl_->pps.assign(next_pps.begin(), next_pps.end());
    if (!impl_->session || sets_changed || impl_->width != frame.width ||
        impl_->height != frame.height) {
      if (!has_idr || !impl_->configure(frame.width, frame.height)) return false;
    }
    impl_->packet.resize(packet_bytes);
    std::size_t position = 0;
    for (std::size_t index = 0; index < count; ++index) {
      const NalView& nal = nals[index];
      if (nal.type == 7 || nal.type == 8 || nal.type == 9) continue;
      const auto length = static_cast<std::uint32_t>(nal.bytes.size());
      impl_->packet[position++] = static_cast<std::byte>(length >> 24U);
      impl_->packet[position++] = static_cast<std::byte>(length >> 16U);
      impl_->packet[position++] = static_cast<std::byte>(length >> 8U);
      impl_->packet[position++] = static_cast<std::byte>(length);
      std::memcpy(impl_->packet.data() + position, nal.bytes.data(), nal.bytes.size());
      position += nal.bytes.size();
    }
    if (!impl_->decode_packet(frame, output)) {
      impl_->reset();
      return false;
    }
    return true;
  } catch (...) {
    impl_->reset();
    return false;
  }
#endif
}

}  // namespace noisefactor::sync
