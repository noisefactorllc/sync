#include "test_harness.hpp"

#include <windows.h>

#include <objbase.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <sync/platform/camera_identity.hpp>
#include <sync/platform/camera_idle_card.hpp>

namespace {

using noisefactor::sync::camera::draw_camera_idle_card;
using noisefactor::sync::camera::kBytesPerPixel;
using noisefactor::sync::camera::kCanvas;

constexpr std::size_t kStride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;

// The card draws through WIC and Direct2D, both of which need COM on this
// thread. The media source's host initializes COM for it; the test does it
// here.
struct ComScope {
  ComScope() { ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
  ~ComScope() { ::CoUninitialize(); }
};
ComScope com_scope;

SYNC_TEST(the_card_fills_the_canvas_opaquely) {
  std::vector<std::byte> canvas(kStride * kCanvas.height, static_cast<std::byte>(0x11));
  SYNC_REQUIRE(draw_camera_idle_card(canvas, kStride, kCanvas));
  for (std::uint32_t row = 0; row < kCanvas.height; row += 97) {
    for (std::uint32_t column = 0; column < kCanvas.width; column += 61) {
      const std::size_t alpha =
          static_cast<std::size_t>(row) * kStride + static_cast<std::size_t>(column) * 4 + 3;
      SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[alpha]) == 255);
    }
  }
}

SYNC_TEST(the_card_is_dark_but_not_pure_black) {
  std::vector<std::byte> canvas(kStride * kCanvas.height);
  SYNC_REQUIRE(draw_camera_idle_card(canvas, kStride, kCanvas));
  // The corner is background: dark, and deliberately not zero so a viewer can
  // tell a drawn card from a dead signal.
  const auto blue = static_cast<std::uint8_t>(canvas[0]);
  SYNC_REQUIRE(blue > 0 && blue < 64);
}

SYNC_TEST(the_card_draws_something_lighter_than_its_background) {
  std::vector<std::byte> canvas(kStride * kCanvas.height);
  SYNC_REQUIRE(draw_camera_idle_card(canvas, kStride, kCanvas));
  const auto background = static_cast<std::uint8_t>(canvas[0]);
  std::uint8_t brightest = 0;
  for (std::size_t i = 0; i < canvas.size(); i += 4) {
    brightest = std::max(brightest, static_cast<std::uint8_t>(canvas[i]));
  }
  // If the font failed to load there would be no text and this would fail,
  // which is the point: a blank card is not the card.
  SYNC_REQUIRE(brightest > background + 64);
}

SYNC_TEST(the_card_refuses_a_buffer_that_cannot_hold_the_canvas) {
  std::vector<std::byte> canvas(kStride * (kCanvas.height - 1), static_cast<std::byte>(0x22));
  SYNC_REQUIRE(!draw_camera_idle_card(canvas, kStride, kCanvas));
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[0]) == 0x22);  // left untouched
}

SYNC_TEST(the_card_refuses_a_stride_narrower_than_the_canvas) {
  std::vector<std::byte> canvas(kStride * kCanvas.height, static_cast<std::byte>(0x33));
  SYNC_REQUIRE(!draw_camera_idle_card(canvas, kStride - 4, kCanvas));
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[0]) == 0x33);
}

}  // namespace
