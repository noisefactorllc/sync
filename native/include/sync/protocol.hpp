#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace noisefactor::sync::protocol {

enum class DecodeError {
  None,
  InputTooShort,
  BadMagic,
  UnsupportedVersion,
  BadHeaderSize,
  MissingTopDownFlag,
  UnsupportedFlags,
  UnsupportedPixelFormat,
  UnsupportedColorSpace,
  UnsupportedAlphaMode,
  NonZeroReserved,
  ZeroDimensions,
  StrideTooSmall,
  ArithmeticOverflow,
  PayloadSizeMismatch,
  FrameSizeMismatch,
  WidthLimitExceeded,
  HeightLimitExceeded,
  PayloadLimitExceeded,
};

struct Limits {
  std::uint32_t max_width = 4096;
  std::uint32_t max_height = 4096;
  std::uint32_t max_payload_bytes = 64U * 1024U * 1024U;
};

struct FrameView {
  std::uint16_t version = 0;
  std::uint16_t header_bytes = 0;
  std::uint32_t flags = 0;
  std::uint16_t pixel_format = 0;
  std::uint16_t color_space = 0;
  std::uint16_t alpha_mode = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_stride = 0;
  std::uint32_t payload_bytes = 0;
  std::uint64_t sequence = 0;
  std::uint64_t presentation_time_us = 0;
  bool top_down = false;
  std::span<const std::byte> payload;
};

struct DecodeResult {
  DecodeError error = DecodeError::None;
  std::optional<FrameView> frame;

  [[nodiscard]] bool ok() const noexcept { return error == DecodeError::None; }
};

[[nodiscard]] DecodeResult decode_frame(std::span<const std::byte> input,
                                         Limits limits = {}) noexcept;

}  // namespace noisefactor::sync::protocol
