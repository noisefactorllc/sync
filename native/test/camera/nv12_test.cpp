#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <sync/camera/nv12.hpp>

namespace {

using noisefactor::sync::camera::bgra_to_nv12;
using noisefactor::sync::camera::nv12_size_bytes;

[[nodiscard]] auto solid_bgra(std::uint32_t width, std::uint32_t height, std::uint8_t b,
                              std::uint8_t g, std::uint8_t r) -> std::vector<std::byte> {
  std::vector<std::byte> pixels(static_cast<std::size_t>(width) * height * 4);
  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i + 0] = static_cast<std::byte>(b);
    pixels[i + 1] = static_cast<std::byte>(g);
    pixels[i + 2] = static_cast<std::byte>(r);
    pixels[i + 3] = static_cast<std::byte>(255);
  }
  return pixels;
}

SYNC_TEST(nv12_size_is_one_and_a_half_planes) {
  SYNC_REQUIRE(nv12_size_bytes(1920, 1080, 1920) == 1920u * 1080u * 3u / 2u);
  SYNC_REQUIRE(nv12_size_bytes(4, 4, 4) == 4u * 4u + 4u * 2u);
}

SYNC_TEST(black_converts_to_studio_range_floor) {
  const auto bgra = solid_bgra(4, 4, 0, 0, 0);
  std::vector<std::byte> nv12(nv12_size_bytes(4, 4, 4));
  SYNC_REQUIRE(bgra_to_nv12(bgra, 16, 4, 4, nv12, 4));
  SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[0]) == 16);
  SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[16]) == 128);
  SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[17]) == 128);
}

SYNC_TEST(white_converts_to_studio_range_ceiling) {
  const auto bgra = solid_bgra(4, 4, 255, 255, 255);
  std::vector<std::byte> nv12(nv12_size_bytes(4, 4, 4));
  SYNC_REQUIRE(bgra_to_nv12(bgra, 16, 4, 4, nv12, 4));
  SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[0]) == 235);
  SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[16]) == 128);
  SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[17]) == 128);
}

SYNC_TEST(pure_red_lands_on_known_chroma) {
  const auto bgra = solid_bgra(2, 2, 0, 0, 255);
  std::vector<std::byte> nv12(nv12_size_bytes(2, 2, 2));
  SYNC_REQUIRE(bgra_to_nv12(bgra, 8, 2, 2, nv12, 2));
  // BT.601 studio red: Y 81, U 90, V 240.
  const auto y = static_cast<std::uint8_t>(nv12[0]);
  const auto u = static_cast<std::uint8_t>(nv12[4]);
  const auto v = static_cast<std::uint8_t>(nv12[5]);
  SYNC_REQUIRE(y >= 80 && y <= 82);
  SYNC_REQUIRE(u >= 89 && u <= 91);
  SYNC_REQUIRE(v >= 239 && v <= 241);
}

SYNC_TEST(every_luma_row_is_written_at_the_stride) {
  // A stride wider than the width must leave the padding alone and still put
  // row N at exactly N * stride.
  const auto bgra = solid_bgra(4, 4, 255, 255, 255);
  std::vector<std::byte> nv12(nv12_size_bytes(4, 4, 8), static_cast<std::byte>(0x7E));
  SYNC_REQUIRE(bgra_to_nv12(bgra, 16, 4, 4, nv12, 8));
  for (std::uint32_t row = 0; row < 4; ++row) {
    SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[row * 8]) == 235);
    // Padding past the width is untouched.
    SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[row * 8 + 4]) == 0x7E);
  }
}

SYNC_TEST(odd_dimensions_are_rejected_rather_than_overflowing_a_chroma_row) {
  const auto bgra = solid_bgra(4, 4, 0, 0, 255);
  std::vector<std::byte> nv12(nv12_size_bytes(4, 4, 4));
  SYNC_REQUIRE(!bgra_to_nv12(bgra, 16, 3, 4, nv12, 4));
  SYNC_REQUIRE(!bgra_to_nv12(bgra, 16, 4, 3, nv12, 4));
}

SYNC_TEST(rejects_undersized_buffers_and_strides) {
  const auto bgra = solid_bgra(4, 4, 0, 0, 0);
  std::vector<std::byte> nv12(nv12_size_bytes(4, 4, 4));
  SYNC_REQUIRE(!bgra_to_nv12(bgra, 8, 4, 4, nv12, 4));
  SYNC_REQUIRE(!bgra_to_nv12(bgra, 16, 4, 4, nv12, 2));
  SYNC_REQUIRE(!bgra_to_nv12(bgra, 16, 0, 4, nv12, 4));
  std::vector<std::byte> tiny(4);
  SYNC_REQUIRE(!bgra_to_nv12(bgra, 16, 4, 4, tiny, 4));
}

}  // namespace
