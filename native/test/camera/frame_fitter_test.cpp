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

SYNC_TEST(a_canvas_sized_frame_is_copied_pixel_for_pixel) {
  // The common case: a 1920x1080 output into a 1920x1080 canvas. It must come
  // out untouched. Every other fit test uses a solid colour, which is
  // invariant under blur and cannot tell a copy from a resample -- a
  // resampler that averaged neighbours here passed all of them while
  // delivering a half-blurred, half-pixel-shifted frame.
  std::vector<std::byte> payload(static_cast<std::size_t>(kCanvas.width) * kCanvas.height *
                                 kBytesPerPixel);
  for (std::uint32_t y = 0; y < kCanvas.height; ++y) {
    for (std::uint32_t x = 0; x < kCanvas.width; ++x) {
      const std::size_t at =
          (static_cast<std::size_t>(y) * kCanvas.width + x) * kBytesPerPixel;
      // Deliberately high frequency: neighbouring pixels differ, so any
      // averaging shows up immediately.
      payload[at + 0] = static_cast<std::byte>(((x + y) % 2) == 0 ? 255 : 0);
      payload[at + 1] = static_cast<std::byte>(x % 256);
      payload[at + 2] = static_cast<std::byte>(y % 256);
      payload[at + 3] = static_cast<std::byte>(255);
    }
  }
  const auto frame = frame_over(payload, kCanvas.width, kCanvas.height, kAlphaOpaque);
  std::vector<std::byte> canvas(kStride * kCanvas.height);
  CameraFitScratch scratch;
  SYNC_REQUIRE(fit_camera_frame(frame, canvas, kStride, kCanvas, scratch));

  for (std::uint32_t y = 0; y < kCanvas.height; ++y) {
    for (std::uint32_t x = 0; x < kCanvas.width; ++x) {
      const std::size_t in = (static_cast<std::size_t>(y) * kCanvas.width + x) * kBytesPerPixel;
      const std::size_t out =
          static_cast<std::size_t>(y) * kStride + static_cast<std::size_t>(x) * kBytesPerPixel;
      // RGBA in, BGRA out, alpha forced opaque.
      SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[out + 0]) ==
                   static_cast<std::uint8_t>(payload[in + 2]));
      SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[out + 1]) ==
                   static_cast<std::uint8_t>(payload[in + 1]));
      SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[out + 2]) ==
                   static_cast<std::uint8_t>(payload[in + 0]));
      SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[out + 3]) == 255);
    }
  }
}

SYNC_TEST(an_upscale_is_not_biased_half_a_pixel) {
  // A 2x upscale of a hard edge. Centre-aligned bilinear keeps the first two
  // destination pixels at the source's first value; a mapping that forgets to
  // subtract half a source pixel shifts the whole ramp one pixel early.
  constexpr std::uint32_t kSourceWidth = 960;
  constexpr std::uint32_t kSourceHeight = 540;
  std::vector<std::byte> payload(static_cast<std::size_t>(kSourceWidth) * kSourceHeight *
                                 kBytesPerPixel);
  for (std::uint32_t y = 0; y < kSourceHeight; ++y) {
    for (std::uint32_t x = 0; x < kSourceWidth; ++x) {
      const auto level = static_cast<std::uint8_t>(x < kSourceWidth / 2 ? 0 : 255);
      const std::size_t at = (static_cast<std::size_t>(y) * kSourceWidth + x) * kBytesPerPixel;
      payload[at + 0] = static_cast<std::byte>(level);
      payload[at + 1] = static_cast<std::byte>(level);
      payload[at + 2] = static_cast<std::byte>(level);
      payload[at + 3] = static_cast<std::byte>(255);
    }
  }
  const auto frame = frame_over(payload, kSourceWidth, kSourceHeight, kAlphaOpaque);
  std::vector<std::byte> canvas(kStride * kCanvas.height);
  CameraFitScratch scratch;
  SYNC_REQUIRE(fit_camera_frame(frame, canvas, kStride, kCanvas, scratch));

  // Far from the edge the value is exact in both halves, with no bleed.
  const std::size_t row = static_cast<std::size_t>(kCanvas.height / 2) * kStride;
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[row + 0]) == 0);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[row + 4]) == 0);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[row + (kCanvas.width - 1) * kBytesPerPixel]) ==
               255);
}

SYNC_TEST(fit_resamples_rather_than_picking_nearest_pixels) {
  // A one-pixel-wide black-and-white checkerboard, downscaled. Nearest
  // neighbour keeps every sample at 0 or 255 and lands aliased; a filtered
  // resample averages neighbours and produces intermediate values. Solid
  // colours cannot tell the two apart, which is why the other tests do not.
  constexpr std::uint32_t kWidth = 3840;
  constexpr std::uint32_t kHeight = 2160;
  std::vector<std::byte> payload(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
  for (std::uint32_t y = 0; y < kHeight; ++y) {
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      const auto level = static_cast<std::uint8_t>(((x + y) % 2) == 0 ? 255 : 0);
      const std::size_t at = (static_cast<std::size_t>(y) * kWidth + x) * kBytesPerPixel;
      payload[at + 0] = static_cast<std::byte>(level);
      payload[at + 1] = static_cast<std::byte>(level);
      payload[at + 2] = static_cast<std::byte>(level);
      payload[at + 3] = static_cast<std::byte>(255);
    }
  }
  const auto frame = frame_over(payload, kWidth, kHeight, kAlphaOpaque);
  std::vector<std::byte> canvas(kStride * kCanvas.height);
  CameraFitScratch scratch;
  SYNC_REQUIRE(fit_camera_frame(frame, canvas, kStride, kCanvas, scratch));

  std::size_t intermediate = 0;
  for (std::uint32_t row = 0; row < kCanvas.height; ++row) {
    for (std::uint32_t column = 0; column < kCanvas.width; ++column) {
      const auto value = static_cast<std::uint8_t>(
          canvas[static_cast<std::size_t>(row) * kStride +
                 static_cast<std::size_t>(column) * kBytesPerPixel]);
      if (value > 16 && value < 239) ++intermediate;
    }
  }
  // Most of the canvas should be blended. Nearest neighbour would score zero.
  SYNC_REQUIRE(intermediate > (static_cast<std::size_t>(kCanvas.width) * kCanvas.height) / 2);
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
