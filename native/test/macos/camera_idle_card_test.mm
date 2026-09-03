#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include <sync/platform/camera_idle_card.hpp>
#include <sync/platform/camera_identity.hpp>

namespace {

using noisefactor::sync::camera::CameraCanvas;
using noisefactor::sync::camera::draw_camera_idle_card;
using noisefactor::sync::camera::kCanvas;

[[nodiscard]] auto pixel(const std::vector<std::byte>& bytes, std::size_t stride, std::uint32_t x,
                         std::uint32_t y) -> std::array<std::uint8_t, 4> {
  const std::size_t offset = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4U;
  return {static_cast<std::uint8_t>(bytes[offset]), static_cast<std::uint8_t>(bytes[offset + 1]),
          static_cast<std::uint8_t>(bytes[offset + 2]), static_cast<std::uint8_t>(bytes[offset + 3])};
}

}  // namespace

// The idle frame is what a consumer sees before any sender exists. It must
// be opaque, visibly not black, and carry something in the middle (the card
// and its text) that the corners do not, so "the camera is dark" and "Sync
// is waiting" cannot be confused.
SYNC_TEST(camera_idle_card_is_opaque_visible_and_not_uniform) {
  const std::size_t stride = static_cast<std::size_t>(kCanvas.width) * 4U + 64U;  // padded
  std::vector<std::byte> bytes(stride * kCanvas.height, std::byte{0x7f});
  SYNC_REQUIRE(draw_camera_idle_card(bytes, stride, kCanvas));

  const auto corner = pixel(bytes, stride, 0, 0);
  const auto other_corner = pixel(bytes, stride, kCanvas.width - 1, kCanvas.height - 1);
  SYNC_REQUIRE(corner == other_corner);
  SYNC_REQUIRE(corner[3] == 255);
  SYNC_REQUIRE(corner[0] + corner[1] + corner[2] > 0);
  SYNC_REQUIRE(corner[0] + corner[1] + corner[2] < 3 * 64);  // dark background

  // Every pixel opaque, and the canvas is not one flat colour.
  std::set<std::uint32_t> colours;
  for (std::uint32_t y = 0; y < kCanvas.height; y += 7) {
    for (std::uint32_t x = 0; x < kCanvas.width; x += 7) {
      const auto p = pixel(bytes, stride, x, y);
      SYNC_REQUIRE(p[3] == 255);
      colours.insert((static_cast<std::uint32_t>(p[0]) << 16) | (p[1] << 8) | p[2]);
    }
  }
  SYNC_REQUIRE(colours.size() >= 3);

  // Bright text pixels exist near the centre band and nowhere near the top edge.
  std::size_t bright_centre = 0;
  std::size_t bright_top = 0;
  for (std::uint32_t x = 0; x < kCanvas.width; ++x) {
    for (std::uint32_t y = kCanvas.height / 2 - 40; y < kCanvas.height / 2 + 40; ++y) {
      const auto p = pixel(bytes, stride, x, y);
      if (p[0] > 180 && p[1] > 180 && p[2] > 180) ++bright_centre;
    }
    const auto top = pixel(bytes, stride, x, 4);
    if (top[0] > 180) ++bright_top;
  }
  SYNC_REQUIRE(bright_centre > 500);
  SYNC_REQUIRE(bright_top == 0);
}

SYNC_TEST(camera_idle_card_rejects_a_buffer_that_cannot_hold_the_canvas) {
  std::vector<std::byte> small(16);
  SYNC_REQUIRE(!draw_camera_idle_card(small, kCanvas.width * 4U, kCanvas));
  std::vector<std::byte> narrow(static_cast<std::size_t>(kCanvas.width) * 4U * kCanvas.height);
  SYNC_REQUIRE(!draw_camera_idle_card(narrow, kCanvas.width * 2U, kCanvas));
  SYNC_REQUIRE(!draw_camera_idle_card(narrow, kCanvas.width * 4U, CameraCanvas{.width = 0, .height = 1}));
}
