#include <sync/frame_receiver.hpp>

#include <algorithm>
#include <cstdint>
#include <new>

#if defined(__APPLE__)
#include <Accelerate/Accelerate.h>
#endif

namespace noisefactor::sync {
namespace {

ReceiveResult result(ReceiveStatus status,
                     protocol::DecodeError decode_error = protocol::DecodeError::None) noexcept {
  return {.status = status, .decode_error = decode_error};
}

bool convert_nv12_to_rgba(const protocol::FrameView& frame,
                          std::span<std::byte> output) noexcept {
  const std::size_t width = frame.width;
  const std::size_t height = frame.height;
  if (frame.pixel_format != 2 || output.size() != width * height * 4U) return false;
#if defined(__APPLE__)
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
  if (!conversion.valid) return false;
  const std::size_t y_bytes = static_cast<std::size_t>(frame.row_stride) * height;
  vImage_Buffer y_plane{
      const_cast<std::byte*>(frame.payload.data()), height, width, frame.row_stride};
  vImage_Buffer uv_plane{
      const_cast<std::byte*>(frame.payload.data() + y_bytes), height / 2U,
      width / 2U, frame.row_stride};
  vImage_Buffer rgba{output.data(), height, width, width * 4U};
  constexpr std::uint8_t permutation[4] = {1, 2, 3, 0};
  return vImageConvert_420Yp8_CbCr8ToARGB8888(
             &y_plane, &uv_plane, &rgba, &conversion.info, permutation, 255,
             kvImageNoFlags) == kvImageNoError;
#else
  const auto clamp = [](int value) -> std::byte {
    return static_cast<std::byte>(std::clamp(value, 0, 255));
  };
  for (std::size_t row = 0; row < height; ++row) {
    const std::size_t y_offset = row * frame.row_stride;
    const std::size_t uv_offset = height * frame.row_stride +
                                  (row / 2U) * frame.row_stride;
    for (std::size_t column = 0; column < width; ++column) {
      const int y = std::max(0, static_cast<int>(std::to_integer<std::uint8_t>(
          frame.payload[y_offset + column])) - 16);
      const int u = static_cast<int>(std::to_integer<std::uint8_t>(
          frame.payload[uv_offset + (column & ~std::size_t{1})])) - 128;
      const int v = static_cast<int>(std::to_integer<std::uint8_t>(
          frame.payload[uv_offset + (column & ~std::size_t{1}) + 1])) - 128;
      const std::size_t pixel = (row * width + column) * 4U;
      output[pixel] = clamp((298 * y + 459 * v + 128) >> 8);
      output[pixel + 1] = clamp((298 * y - 55 * u - 136 * v + 128) >> 8);
      output[pixel + 2] = clamp((298 * y + 541 * u + 128) >> 8);
      output[pixel + 3] = std::byte{255};
    }
  }
  return true;
#endif
}

}  // namespace

FrameReceiver::FrameReceiver(FramePublisher& publisher, protocol::Limits limits)
    : publisher_(publisher), limits_(limits) {}

auto FrameReceiver::find_sender(std::string_view sender_id) noexcept -> SenderEntry* {
  for (SenderEntry& entry : sender_entries_) {
    if (entry.occupied && entry.sender_id_length == sender_id.size() &&
        std::string_view(entry.sender_id.data(), entry.sender_id_length) == sender_id) {
      return &entry;
    }
  }
  return nullptr;
}

auto FrameReceiver::find_sender(std::string_view sender_id) const noexcept -> const SenderEntry* {
  for (const SenderEntry& entry : sender_entries_) {
    if (entry.occupied && entry.sender_id_length == sender_id.size() &&
        std::string_view(entry.sender_id.data(), entry.sender_id_length) == sender_id) {
      return &entry;
    }
  }
  return nullptr;
}

auto FrameReceiver::find_or_create_sender(std::string_view sender_id) noexcept -> SenderEntry* {
  if (SenderEntry* existing = find_sender(sender_id)) {
    return existing;
  }
  for (SenderEntry& entry : sender_entries_) {
    if (!entry.occupied) {
      for (std::size_t index = 0; index < sender_id.size(); ++index) {
        entry.sender_id[index] = sender_id[index];
      }
      entry.sender_id_length = sender_id.size();
      entry.occupied = true;
      return &entry;
    }
  }
  return nullptr;
}

auto FrameReceiver::receive(std::string_view sender_id,
                            std::span<const std::byte> bytes) noexcept -> ReceiveResult {
  if (sender_id.empty() || sender_id.size() > kMaximumSenderIdBytes) {
    return result(ReceiveStatus::RejectedSender);
  }

  SenderEntry* entry = find_or_create_sender(sender_id);
  if (entry == nullptr) {
    return result(ReceiveStatus::RejectedSender);
  }

  const auto decoded = protocol::decode_frame(bytes, limits_);
  SenderStats& stats = entry->stats;
  if (!decoded.ok()) {
    ++stats.rejected;
    return result(ReceiveStatus::RejectedMalformed, decoded.error);
  }

  const protocol::FrameView& frame = *decoded.frame;
  if (stats.has_last_sequence && frame.sequence <= stats.last_sequence) {
    ++stats.dropped;
    return result(ReceiveStatus::DroppedStale);
  }

  stats.has_last_sequence = true;
  stats.last_sequence = frame.sequence;
  stats.last_presentation_time_us = frame.presentation_time_us;

  protocol::FrameView publish_frame = frame;
  if (frame.pixel_format == 2 || frame.pixel_format == 3) {
    const std::size_t rgba_bytes =
        static_cast<std::size_t>(frame.width) * frame.height * 4U;
    if (rgba_bytes > rgba_conversion_capacity_) {
      std::unique_ptr<std::byte[]> replacement(new (std::nothrow) std::byte[rgba_bytes]);
      if (!replacement) {
        ++stats.failed;
        return result(ReceiveStatus::PublishFailed);
      }
      rgba_conversion_buffer_ = std::move(replacement);
      rgba_conversion_capacity_ = rgba_bytes;
    }
    auto rgba = std::span(rgba_conversion_buffer_.get(), rgba_bytes);
    if (frame.pixel_format == 3 && !entry->h264_decoder) {
      entry->h264_decoder.reset(new (std::nothrow) H264Decoder);
      if (!entry->h264_decoder) {
        ++stats.failed;
        return result(ReceiveStatus::PublishFailed);
      }
    }
    const bool converted = frame.pixel_format == 2
        ? convert_nv12_to_rgba(frame, rgba)
        : entry->h264_decoder->decode(frame, rgba);
    if (!converted) {
      ++stats.failed;
      return result(ReceiveStatus::PublishFailed);
    }
    publish_frame.pixel_format = 1;
    publish_frame.alpha_mode = 1;
    publish_frame.row_stride = frame.width * 4U;
    publish_frame.payload_bytes = static_cast<std::uint32_t>(rgba_bytes);
    publish_frame.payload = rgba;
  }

  switch (publisher_.publish(sender_id, publish_frame)) {
    case PublishResult::Accepted:
      ++stats.accepted;
      return result(ReceiveStatus::Accepted);
    case PublishResult::Backpressured:
      ++stats.dropped;
      return result(ReceiveStatus::DroppedBackpressure);
    case PublishResult::Failed:
      ++stats.failed;
      return result(ReceiveStatus::PublishFailed);
  }

  ++stats.failed;
  return result(ReceiveStatus::PublishFailed);
}

auto FrameReceiver::stats(std::string_view sender_id) const noexcept -> const SenderStats* {
  const SenderEntry* entry = find_sender(sender_id);
  return entry == nullptr ? nullptr : &entry->stats;
}

auto FrameReceiver::remove_sender(std::string_view sender_id) noexcept -> bool {
  if (sender_id.empty() || sender_id.size() > kMaximumSenderIdBytes) {
    return false;
  }
  SenderEntry* entry = find_sender(sender_id);
  if (entry == nullptr) {
    return false;
  }
  *entry = SenderEntry{};
  return true;
}

}  // namespace noisefactor::sync
