#include <sync/protocol.hpp>

#include <limits>

namespace noisefactor::sync::protocol {
namespace {

constexpr std::uint32_t kMagic = 0x434e5953U;
constexpr std::uint16_t kVersion = 1;
constexpr std::uint16_t kHeaderBytes = 64;
constexpr std::uint32_t kTopDownFlag = 1U;

DecodeResult failure(DecodeError error) noexcept {
  return {.error = error, .frame = std::nullopt};
}

std::uint16_t read_u16_le(std::span<const std::byte> input, std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset])) |
         static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset + 1])) << 8U;
}

std::uint32_t read_u32_le(std::span<const std::byte> input, std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + index]))
             << (index * 8U);
  }
  return value;
}

std::uint64_t read_u64_le(std::span<const std::byte> input, std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[offset + index]))
             << (index * 8U);
  }
  return value;
}

bool multiplication_overflows_u32(std::uint32_t left, std::uint32_t right) noexcept {
  return right != 0 && left > std::numeric_limits<std::uint32_t>::max() / right;
}

bool addition_overflows_size(std::size_t left, std::size_t right) noexcept {
  return left > std::numeric_limits<std::size_t>::max() - right;
}

}  // namespace

DecodeResult decode_frame(std::span<const std::byte> input, Limits limits) noexcept {
  if (input.size() < kHeaderBytes) {
    return failure(DecodeError::InputTooShort);
  }
  if (read_u32_le(input, 0) != kMagic) {
    return failure(DecodeError::BadMagic);
  }
  if (read_u16_le(input, 4) != kVersion) {
    return failure(DecodeError::UnsupportedVersion);
  }
  if (read_u16_le(input, 6) != kHeaderBytes) {
    return failure(DecodeError::BadHeaderSize);
  }

  const std::uint32_t flags = read_u32_le(input, 8);
  if ((flags & kTopDownFlag) == 0) {
    return failure(DecodeError::MissingTopDownFlag);
  }
  if ((flags & ~kTopDownFlag) != 0) {
    return failure(DecodeError::UnsupportedFlags);
  }

  const std::uint16_t pixel_format = read_u16_le(input, 12);
  if (pixel_format != 1) {
    return failure(DecodeError::UnsupportedPixelFormat);
  }
  const std::uint16_t color_space = read_u16_le(input, 14);
  if (color_space != 1 && color_space != 2) {
    return failure(DecodeError::UnsupportedColorSpace);
  }
  const std::uint16_t alpha_mode = read_u16_le(input, 16);
  if (alpha_mode != 1 && alpha_mode != 2 && alpha_mode != 3) {
    return failure(DecodeError::UnsupportedAlphaMode);
  }
  if (read_u16_le(input, 18) != 0) {
    return failure(DecodeError::NonZeroReserved);
  }
  for (std::size_t offset = 52; offset < kHeaderBytes; ++offset) {
    if (input[offset] != std::byte{0}) {
      return failure(DecodeError::NonZeroReserved);
    }
  }

  const std::uint32_t width = read_u32_le(input, 20);
  const std::uint32_t height = read_u32_le(input, 24);
  const std::uint32_t row_stride = read_u32_le(input, 28);
  const std::uint32_t payload_bytes = read_u32_le(input, 32);
  if (width == 0 || height == 0) {
    return failure(DecodeError::ZeroDimensions);
  }
  if (width > limits.max_width) {
    return failure(DecodeError::WidthLimitExceeded);
  }
  if (height > limits.max_height) {
    return failure(DecodeError::HeightLimitExceeded);
  }
  if (multiplication_overflows_u32(width, 4)) {
    return failure(DecodeError::ArithmeticOverflow);
  }
  const std::uint32_t minimum_stride = width * 4U;
  if (row_stride < minimum_stride) {
    return failure(DecodeError::StrideTooSmall);
  }
  if (multiplication_overflows_u32(row_stride, height)) {
    return failure(DecodeError::ArithmeticOverflow);
  }
  const std::uint32_t expected_payload_bytes = row_stride * height;
  if (payload_bytes != expected_payload_bytes) {
    return failure(DecodeError::PayloadSizeMismatch);
  }
  if (payload_bytes > limits.max_payload_bytes) {
    return failure(DecodeError::PayloadLimitExceeded);
  }
  if (addition_overflows_size(kHeaderBytes, static_cast<std::size_t>(payload_bytes))) {
    return failure(DecodeError::ArithmeticOverflow);
  }
  const std::size_t expected_frame_bytes = kHeaderBytes + static_cast<std::size_t>(payload_bytes);
  if (input.size() != expected_frame_bytes) {
    return failure(DecodeError::FrameSizeMismatch);
  }

  return {
      .error = DecodeError::None,
      .frame = FrameView{
          .version = kVersion,
          .header_bytes = kHeaderBytes,
          .flags = flags,
          .pixel_format = pixel_format,
          .color_space = color_space,
          .alpha_mode = alpha_mode,
          .width = width,
          .height = height,
          .row_stride = row_stride,
          .payload_bytes = payload_bytes,
          .sequence = read_u64_le(input, 36),
          .presentation_time_us = read_u64_le(input, 44),
          .top_down = true,
          .payload = input.subspan(kHeaderBytes, payload_bytes),
      },
  };
}

}  // namespace noisefactor::sync::protocol
