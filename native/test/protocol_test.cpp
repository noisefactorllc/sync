#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <vector>

#include <sync/protocol.hpp>

namespace {

using noisefactor::sync::protocol::DecodeError;
using noisefactor::sync::protocol::Limits;
using noisefactor::sync::protocol::decode_frame;

constexpr std::size_t kHeaderBytes = 64;

std::vector<std::byte> load_golden_frame() {
  const auto path = std::filesystem::path(SYNC_SOURCE_DIR) / "test/fixtures/frame-v1.bin";
  std::ifstream input(path, std::ios::binary);
  SYNC_REQUIRE(input.good());
  std::vector<std::byte> frame;
  char value = 0;
  while (input.get(value)) {
    frame.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
  }
  return frame;
}

void write_u16(std::vector<std::byte>& frame, std::size_t offset, std::uint16_t value) {
  frame[offset] = static_cast<std::byte>(value & 0xffU);
  frame[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void write_u32(std::vector<std::byte>& frame, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    frame[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

void require_error(const std::vector<std::byte>& frame, DecodeError error, Limits limits = {}) {
  const auto result = decode_frame(frame, limits);
  SYNC_REQUIRE(result.error == error);
  SYNC_REQUIRE(!result.frame.has_value());
}

}  // namespace

SYNC_TEST(decodes_the_browser_generated_golden_frame_without_copying_payload) {
  auto frame = load_golden_frame();
  const auto result = decode_frame(frame, {});

  SYNC_REQUIRE(result.error == DecodeError::None);
  SYNC_REQUIRE(result.frame.has_value());
  const auto& decoded = *result.frame;
  SYNC_REQUIRE(decoded.width == 2);
  SYNC_REQUIRE(decoded.height == 2);
  SYNC_REQUIRE(decoded.row_stride == 8);
  SYNC_REQUIRE(decoded.payload_bytes == 16);
  SYNC_REQUIRE(decoded.sequence == 4294967301ULL);
  SYNC_REQUIRE(decoded.presentation_time_us == 1723305600123456ULL);
  SYNC_REQUIRE(decoded.pixel_format == 1);
  SYNC_REQUIRE(decoded.color_space == 1);
  SYNC_REQUIRE(decoded.alpha_mode == 3);
  SYNC_REQUIRE(decoded.top_down);
  SYNC_REQUIRE(decoded.payload.data() == frame.data() + kHeaderBytes);
  const std::array<std::byte, 16> expected_payload = {
      std::byte{0xff}, std::byte{0x00}, std::byte{0x00}, std::byte{0xff},
      std::byte{0x00}, std::byte{0xff}, std::byte{0x00}, std::byte{0x80},
      std::byte{0x00}, std::byte{0x00}, std::byte{0xff}, std::byte{0x40},
      std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x00},
  };
  SYNC_REQUIRE(decoded.payload.size() == expected_payload.size());
  for (std::size_t index = 0; index < expected_payload.size(); ++index) {
    SYNC_REQUIRE(decoded.payload[index] == expected_payload[index]);
  }
}

SYNC_TEST(rejects_a_frame_shorter_than_the_header) {
  std::vector<std::byte> frame(kHeaderBytes - 1);
  require_error(frame, DecodeError::InputTooShort);
}

SYNC_TEST(rejects_bad_magic) {
  auto frame = load_golden_frame();
  write_u32(frame, 0, 0);
  require_error(frame, DecodeError::BadMagic);
}

SYNC_TEST(rejects_an_unknown_version) {
  auto frame = load_golden_frame();
  write_u16(frame, 4, 2);
  require_error(frame, DecodeError::UnsupportedVersion);
}

SYNC_TEST(rejects_a_wrong_header_size) {
  auto frame = load_golden_frame();
  write_u16(frame, 6, 63);
  require_error(frame, DecodeError::BadHeaderSize);
}

SYNC_TEST(rejects_absent_or_unknown_flags) {
  auto absent = load_golden_frame();
  write_u32(absent, 8, 0);
  require_error(absent, DecodeError::MissingTopDownFlag);

  auto unknown = load_golden_frame();
  write_u32(unknown, 8, 3);
  require_error(unknown, DecodeError::UnsupportedFlags);
}

SYNC_TEST(rejects_unknown_enums) {
  auto pixel_format = load_golden_frame();
  write_u16(pixel_format, 12, 2);
  require_error(pixel_format, DecodeError::UnsupportedPixelFormat);

  auto color_space = load_golden_frame();
  write_u16(color_space, 14, 3);
  require_error(color_space, DecodeError::UnsupportedColorSpace);

  auto alpha_mode = load_golden_frame();
  write_u16(alpha_mode, 16, 4);
  require_error(alpha_mode, DecodeError::UnsupportedAlphaMode);
}

SYNC_TEST(rejects_nonzero_reserved_bytes) {
  auto reserved_field = load_golden_frame();
  write_u16(reserved_field, 18, 1);
  require_error(reserved_field, DecodeError::NonZeroReserved);

  auto reserved_tail = load_golden_frame();
  reserved_tail[52] = std::byte{1};
  require_error(reserved_tail, DecodeError::NonZeroReserved);
}

SYNC_TEST(rejects_zero_dimensions) {
  auto zero_width = load_golden_frame();
  write_u32(zero_width, 20, 0);
  require_error(zero_width, DecodeError::ZeroDimensions);

  auto zero_height = load_golden_frame();
  write_u32(zero_height, 24, 0);
  require_error(zero_height, DecodeError::ZeroDimensions);
}

SYNC_TEST(rejects_a_stride_smaller_than_width_times_four) {
  auto frame = load_golden_frame();
  write_u32(frame, 28, 7);
  require_error(frame, DecodeError::StrideTooSmall);
}

SYNC_TEST(rejects_width_times_four_arithmetic_overflow) {
  auto frame = load_golden_frame();
  write_u32(frame, 20, std::numeric_limits<std::uint32_t>::max());
  write_u32(frame, 28, std::numeric_limits<std::uint32_t>::max());
  const Limits unbounded = {
      .max_width = std::numeric_limits<std::uint32_t>::max(),
      .max_height = std::numeric_limits<std::uint32_t>::max(),
      .max_payload_bytes = std::numeric_limits<std::uint32_t>::max(),
  };
  require_error(frame, DecodeError::ArithmeticOverflow, unbounded);
}

SYNC_TEST(rejects_row_stride_times_height_arithmetic_overflow) {
  auto frame = load_golden_frame();
  write_u32(frame, 20, 1);
  write_u32(frame, 24, 2);
  write_u32(frame, 28, std::numeric_limits<std::uint32_t>::max());
  write_u32(frame, 32, std::numeric_limits<std::uint32_t>::max());
  const Limits unbounded = {
      .max_width = std::numeric_limits<std::uint32_t>::max(),
      .max_height = std::numeric_limits<std::uint32_t>::max(),
      .max_payload_bytes = std::numeric_limits<std::uint32_t>::max(),
  };
  require_error(frame, DecodeError::ArithmeticOverflow, unbounded);
}

SYNC_TEST(rejects_a_payload_size_that_does_not_match_stride_times_height) {
  auto frame = load_golden_frame();
  write_u32(frame, 32, 15);
  require_error(frame, DecodeError::PayloadSizeMismatch);
}

SYNC_TEST(rejects_a_full_frame_length_that_does_not_match_the_header) {
  auto frame = load_golden_frame();
  frame.pop_back();
  require_error(frame, DecodeError::FrameSizeMismatch);
}

SYNC_TEST(enforces_configured_width_height_and_payload_limits) {
  auto frame = load_golden_frame();
  require_error(frame, DecodeError::WidthLimitExceeded,
                {.max_width = 1, .max_height = 2, .max_payload_bytes = 16});
  require_error(frame, DecodeError::HeightLimitExceeded,
                {.max_width = 2, .max_height = 1, .max_payload_bytes = 16});
  require_error(frame, DecodeError::PayloadLimitExceeded,
                {.max_width = 2, .max_height = 2, .max_payload_bytes = 15});
}
