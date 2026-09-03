#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <sync/platform/camera_frame_fitter.hpp>
#include <sync/platform/camera_identity.hpp>

namespace {

using noisefactor::sync::camera::CameraFitScratch;
using noisefactor::sync::camera::compute_camera_placement;
using noisefactor::sync::camera::fit_camera_frame;
using noisefactor::sync::camera::kBytesPerPixel;
using noisefactor::sync::camera::kCanvas;
using noisefactor::sync::protocol::FrameView;

constexpr std::uint16_t kPixelFormatRgba8 = 1;
constexpr std::uint16_t kAlphaOpaque = 1;
constexpr std::uint16_t kAlphaStraight = 2;
constexpr std::size_t kStride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;

[[nodiscard]] auto solid_rgba(std::uint32_t width, std::uint32_t height, std::uint8_t r,
                              std::uint8_t g, std::uint8_t b, std::uint8_t a)
    -> std::vector<std::byte> {
  std::vector<std::byte> pixels(static_cast<std::size_t>(width) * height * kBytesPerPixel);
  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i + 0] = static_cast<std::byte>(r);
    pixels[i + 1] = static_cast<std::byte>(g);
    pixels[i + 2] = static_cast<std::byte>(b);
    pixels[i + 3] = static_cast<std::byte>(a);
  }
  return pixels;
}

[[nodiscard]] auto frame_over(const std::vector<std::byte>& payload, std::uint32_t width,
                              std::uint32_t height, std::uint16_t alpha_mode) -> FrameView {
  return {
      .version = 1,
      .header_bytes = 64,
      .flags = 1,
      .pixel_format = kPixelFormatRgba8,
      .color_space = 1,
      .alpha_mode = alpha_mode,
      .width = width,
      .height = height,
      .row_stride = width * kBytesPerPixel,
      .payload_bytes = static_cast<std::uint32_t>(payload.size()),
      .sequence = 1,
      .presentation_time_us = 0,
      .top_down = true,
      .payload = payload,
  };
}

SYNC_TEST(placement_letterboxes_a_wide_source) {
  const auto placement = compute_camera_placement(3840, 1080, kCanvas);
  SYNC_REQUIRE(placement.has_value());
  SYNC_REQUIRE(placement->width == 1920);
  SYNC_REQUIRE(placement->height == 540);
  SYNC_REQUIRE(placement->x == 0);
  SYNC_REQUIRE(placement->y == 270);
}

SYNC_TEST(placement_pillarboxes_a_tall_source) {
  const auto placement = compute_camera_placement(1080, 1920, kCanvas);
  SYNC_REQUIRE(placement.has_value());
  SYNC_REQUIRE(placement->height == 1080);
  SYNC_REQUIRE(placement->width == 607);
  SYNC_REQUIRE(placement->y == 0);
  SYNC_REQUIRE(placement->x == 656);
}

SYNC_TEST(placement_rejects_a_zero_dimension) {
  SYNC_REQUIRE(!compute_camera_placement(0, 1080, kCanvas).has_value());
  SYNC_REQUIRE(!compute_camera_placement(1920, 0, kCanvas).has_value());
}

SYNC_TEST(fit_permutes_rgba_to_bgra_and_forces_opaque) {
  const auto payload = solid_rgba(1920, 1080, 255, 0, 0, 255);
  const auto frame = frame_over(payload, 1920, 1080, kAlphaOpaque);
  std::vector<std::byte> canvas(kStride * kCanvas.height);
  CameraFitScratch scratch;
  SYNC_REQUIRE(fit_camera_frame(frame, canvas, kStride, kCanvas, scratch));
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[0]) == 0);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[1]) == 0);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[2]) == 255);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[3]) == 255);
}

SYNC_TEST(fit_premultiplies_straight_alpha_over_black) {
  const auto payload = solid_rgba(1920, 1080, 255, 255, 255, 128);
  const auto frame = frame_over(payload, 1920, 1080, kAlphaStraight);
  std::vector<std::byte> canvas(kStride * kCanvas.height);
  CameraFitScratch scratch;
  SYNC_REQUIRE(fit_camera_frame(frame, canvas, kStride, kCanvas, scratch));
  const auto blue = static_cast<std::uint8_t>(canvas[0]);
  SYNC_REQUIRE(blue >= 127 && blue <= 129);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[3]) == 255);
}

SYNC_TEST(fit_paints_bars_black_around_a_letterboxed_source) {
  const auto payload = solid_rgba(3840, 1080, 255, 255, 255, 255);
  const auto frame = frame_over(payload, 3840, 1080, kAlphaOpaque);
  std::vector<std::byte> canvas(kStride * kCanvas.height, static_cast<std::byte>(0xAB));
  CameraFitScratch scratch;
  SYNC_REQUIRE(fit_camera_frame(frame, canvas, kStride, kCanvas, scratch));
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[0]) == 0);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[1]) == 0);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[2]) == 0);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[3]) == 255);
  const std::size_t centre = static_cast<std::size_t>(kCanvas.height / 2) * kStride +
                             static_cast<std::size_t>(kCanvas.width / 2) * kBytesPerPixel;
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[centre]) == 255);
}

SYNC_TEST(fit_rejects_a_bottom_up_frame) {
  const auto payload = solid_rgba(1920, 1080, 255, 0, 0, 255);
  auto frame = frame_over(payload, 1920, 1080, kAlphaOpaque);
  frame.top_down = false;
  std::vector<std::byte> canvas(kStride * kCanvas.height);
  CameraFitScratch scratch;
  SYNC_REQUIRE(!fit_camera_frame(frame, canvas, kStride, kCanvas, scratch));
}

SYNC_TEST(fit_rejects_a_canvas_buffer_that_is_too_small) {
  const auto payload = solid_rgba(1920, 1080, 255, 0, 0, 255);
  const auto frame = frame_over(payload, 1920, 1080, kAlphaOpaque);
  std::vector<std::byte> canvas(kStride * (kCanvas.height - 1));
  CameraFitScratch scratch;
  SYNC_REQUIRE(!fit_camera_frame(frame, canvas, kStride, kCanvas, scratch));
}

}  // namespace
