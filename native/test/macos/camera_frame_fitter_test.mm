#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <sync/platform/camera_frame_fitter.hpp>
#include <sync/platform/camera_identity.hpp>

namespace {

using noisefactor::sync::camera::CameraCanvas;
using noisefactor::sync::camera::compute_camera_placement;
using noisefactor::sync::camera::fit_camera_frame;
using noisefactor::sync::protocol::FrameView;

[[nodiscard]] auto frame_of(std::uint32_t width, std::uint32_t height,
                            std::span<const std::byte> payload, std::uint16_t alpha_mode,
                            std::uint32_t stride = 0) noexcept -> FrameView {
  return {
      .version = 1,
      .header_bytes = 64,
      .flags = 1,
      .pixel_format = 1,
      .color_space = 1,
      .alpha_mode = alpha_mode,
      .width = width,
      .height = height,
      .row_stride = stride == 0 ? width * 4U : stride,
      .payload_bytes = static_cast<std::uint32_t>(payload.size()),
      .sequence = 1,
      .presentation_time_us = 1'000,
      .top_down = true,
      .payload = payload,
  };
}

[[nodiscard]] auto pixel(std::span<const std::byte> canvas, std::size_t stride,
                         std::uint32_t x, std::uint32_t y) -> std::array<std::uint8_t, 4> {
  const std::size_t offset = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4U;
  return {static_cast<std::uint8_t>(canvas[offset]), static_cast<std::uint8_t>(canvas[offset + 1]),
          static_cast<std::uint8_t>(canvas[offset + 2]), static_cast<std::uint8_t>(canvas[offset + 3])};
}

}  // namespace

SYNC_TEST(camera_placement_letterboxes_wide_and_pillarboxes_tall_sources) {
  const CameraCanvas canvas{.width = 1920, .height = 1080};
  const auto wide = compute_camera_placement(3840, 1080, canvas);
  SYNC_REQUIRE(wide.has_value());
  SYNC_REQUIRE(wide->width == 1920 && wide->height == 540 && wide->x == 0 && wide->y == 270);
  const auto tall = compute_camera_placement(1080, 1920, canvas);
  SYNC_REQUIRE(tall.has_value());
  SYNC_REQUIRE(tall->height == 1080 && tall->width == 607 && tall->y == 0 && tall->x == 656);
  const auto exact = compute_camera_placement(1920, 1080, canvas);
  SYNC_REQUIRE(exact.has_value());
  SYNC_REQUIRE(exact->x == 0 && exact->y == 0 && exact->width == 1920 && exact->height == 1080);
  SYNC_REQUIRE(!compute_camera_placement(0, 1080, canvas).has_value());
  SYNC_REQUIRE(!compute_camera_placement(1920, 0, canvas).has_value());
}

SYNC_TEST(camera_fitter_swaps_rgba_to_bgra_and_copies_an_exact_size_frame) {
  const CameraCanvas canvas{.width = 2, .height = 2};
  // One red, one green, one blue, one white pixel, opaque.
  const std::array<std::byte, 16> payload{
      std::byte{255}, std::byte{0},   std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{255}, std::byte{0},   std::byte{255},
      std::byte{0},   std::byte{0},   std::byte{255}, std::byte{255},
      std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
  std::vector<std::byte> out(16);
  SYNC_REQUIRE(fit_camera_frame(frame_of(2, 2, payload, 1), out, 8, canvas));
  SYNC_REQUIRE((pixel(out, 8, 0, 0) == std::array<std::uint8_t, 4>{0, 0, 255, 255}));
  SYNC_REQUIRE((pixel(out, 8, 1, 0) == std::array<std::uint8_t, 4>{0, 255, 0, 255}));
  SYNC_REQUIRE((pixel(out, 8, 0, 1) == std::array<std::uint8_t, 4>{255, 0, 0, 255}));
  SYNC_REQUIRE((pixel(out, 8, 1, 1) == std::array<std::uint8_t, 4>{255, 255, 255, 255}));
}

SYNC_TEST(camera_fitter_premultiplies_straight_alpha_over_black) {
  const CameraCanvas canvas{.width = 1, .height = 1};
  const std::array<std::byte, 4> payload{std::byte{200}, std::byte{100}, std::byte{50}, std::byte{128}};
  std::vector<std::byte> out(4);
  SYNC_REQUIRE(fit_camera_frame(frame_of(1, 1, payload, 2), out, 4, canvas));
  const auto p = pixel(out, 4, 0, 0);
  // Straight alpha 128/255 halves every channel (within rounding) and the
  // output alpha becomes opaque: a camera has no alpha channel to offer.
  SYNC_REQUIRE(p[0] >= 24 && p[0] <= 26);
  SYNC_REQUIRE(p[1] >= 49 && p[1] <= 51);
  SYNC_REQUIRE(p[2] >= 99 && p[2] <= 101);
  SYNC_REQUIRE(p[3] == 255);
}

SYNC_TEST(camera_fitter_letterboxes_with_black_and_scales_into_the_canvas) {
  const CameraCanvas canvas{.width = 4, .height = 2};
  // A 2x2 all-white opaque frame into a 4x2 canvas: 2x2 at x=1, black bars.
  std::vector<std::byte> payload(16, std::byte{255});
  std::vector<std::byte> out(32, std::byte{7});
  SYNC_REQUIRE(fit_camera_frame(frame_of(2, 2, payload, 1), out, 16, canvas));
  SYNC_REQUIRE((pixel(out, 16, 0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 255}));
  SYNC_REQUIRE((pixel(out, 16, 3, 1) == std::array<std::uint8_t, 4>{0, 0, 0, 255}));
  SYNC_REQUIRE((pixel(out, 16, 1, 0) == std::array<std::uint8_t, 4>{255, 255, 255, 255}));
  SYNC_REQUIRE((pixel(out, 16, 2, 1) == std::array<std::uint8_t, 4>{255, 255, 255, 255}));
}

SYNC_TEST(camera_fitter_scales_a_large_frame_down_without_leaving_bars) {
  const CameraCanvas canvas{.width = 2, .height = 2};
  // 4x4 white source into a 2x2 canvas: same aspect, so every pixel is white.
  std::vector<std::byte> payload(64, std::byte{255});
  std::vector<std::byte> out(16, std::byte{0});
  SYNC_REQUIRE(fit_camera_frame(frame_of(4, 4, payload, 1), out, 8, canvas));
  for (std::uint32_t y = 0; y < 2; ++y) {
    for (std::uint32_t x = 0; x < 2; ++x) {
      SYNC_REQUIRE((pixel(out, 8, x, y) == std::array<std::uint8_t, 4>{255, 255, 255, 255}));
    }
  }
}

SYNC_TEST(camera_fitter_rejects_malformed_frames_and_small_canvases) {
  const CameraCanvas canvas{.width = 2, .height = 2};
  std::vector<std::byte> payload(16, std::byte{255});
  std::vector<std::byte> out(16);
  FrameView bad_format = frame_of(2, 2, payload, 1);
  bad_format.pixel_format = 2;
  SYNC_REQUIRE(!fit_camera_frame(bad_format, out, 8, canvas));
  FrameView short_payload = frame_of(2, 2, std::span<const std::byte>(payload).first(8), 1);
  SYNC_REQUIRE(!fit_camera_frame(short_payload, out, 8, canvas));
  FrameView narrow_stride = frame_of(2, 2, payload, 1, 4);
  SYNC_REQUIRE(!fit_camera_frame(narrow_stride, out, 8, canvas));
  std::vector<std::byte> too_small(8);
  SYNC_REQUIRE(!fit_camera_frame(frame_of(2, 2, payload, 1), too_small, 8, canvas));
  FrameView bottom_up = frame_of(2, 2, payload, 1);
  bottom_up.top_down = false;
  SYNC_REQUIRE(!fit_camera_frame(bottom_up, out, 8, canvas));
}

// The daemon fits one frame per received frame, sixty times a second, for the
// length of a show. The conversion must not allocate per frame: a caller-owned
// scratch grows once to the working set and is reused from then on.
SYNC_TEST(camera_fitter_reuses_caller_scratch_across_frames) {
  using noisefactor::sync::camera::CameraFitScratch;
  const CameraCanvas canvas{.width = 4, .height = 2};
  std::vector<std::byte> payload(64, std::byte{255});  // 4x4 white, needs scaling
  std::vector<std::byte> out(32, std::byte{7});
  CameraFitScratch scratch;
  SYNC_REQUIRE(fit_camera_frame(frame_of(4, 4, payload, 1), out, 16, canvas, scratch));
  const std::size_t capacity_after_first = scratch.swapped.capacity();
  const std::byte* const data_after_first = scratch.swapped.data();
  SYNC_REQUIRE(capacity_after_first >= 64);
  for (int repeat = 0; repeat < 8; ++repeat) {
    SYNC_REQUIRE(fit_camera_frame(frame_of(4, 4, payload, 1), out, 16, canvas, scratch));
  }
  SYNC_REQUIRE(scratch.swapped.capacity() == capacity_after_first);
  SYNC_REQUIRE(scratch.swapped.data() == data_after_first);
  SYNC_REQUIRE((pixel(out, 16, 0, 0) == std::array<std::uint8_t, 4>{0, 0, 0, 255}));
  SYNC_REQUIRE((pixel(out, 16, 2, 1) == std::array<std::uint8_t, 4>{255, 255, 255, 255}));
}

// An exact-size frame covers the whole canvas, so there are no bars to paint
// and no intermediate to stage; every canvas byte is still overwritten.
SYNC_TEST(camera_fitter_overwrites_the_whole_canvas_for_an_exact_size_frame) {
  using noisefactor::sync::camera::CameraFitScratch;
  const CameraCanvas canvas{.width = 3, .height = 2};
  std::vector<std::byte> payload(24);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::byte>(i * 10 + 1);
  }
  std::vector<std::byte> out(24, std::byte{7});
  CameraFitScratch scratch;
  SYNC_REQUIRE(fit_camera_frame(frame_of(3, 2, payload, 1), out, 12, canvas, scratch));
  SYNC_REQUIRE(scratch.swapped.capacity() == 0);
  for (std::uint32_t y = 0; y < 2; ++y) {
    for (std::uint32_t x = 0; x < 3; ++x) {
      const std::size_t source = (static_cast<std::size_t>(y) * 3 + x) * 4;
      const auto expected = std::array<std::uint8_t, 4>{
          static_cast<std::uint8_t>(payload[source + 2]),
          static_cast<std::uint8_t>(payload[source + 1]),
          static_cast<std::uint8_t>(payload[source + 0]), 255};
      SYNC_REQUIRE(pixel(out, 12, x, y) == expected);
    }
  }
}

// Straight alpha is premultiplied over black in the exact-size path too, and
// the result is opaque, matching the scaled path pixel for pixel.
SYNC_TEST(camera_fitter_exact_size_straight_alpha_matches_scaled_path) {
  using noisefactor::sync::camera::CameraFitScratch;
  const CameraCanvas canvas{.width = 2, .height = 2};
  const std::array<std::byte, 16> payload{
      std::byte{200}, std::byte{100}, std::byte{50}, std::byte{128},
      std::byte{200}, std::byte{100}, std::byte{50}, std::byte{128},
      std::byte{200}, std::byte{100}, std::byte{50}, std::byte{128},
      std::byte{200}, std::byte{100}, std::byte{50}, std::byte{128}};
  std::vector<std::byte> out(16, std::byte{7});
  CameraFitScratch scratch;
  SYNC_REQUIRE(fit_camera_frame(frame_of(2, 2, payload, 2), out, 8, canvas, scratch));
  for (std::uint32_t y = 0; y < 2; ++y) {
    for (std::uint32_t x = 0; x < 2; ++x) {
      const auto p = pixel(out, 8, x, y);
      SYNC_REQUIRE(p[0] >= 24 && p[0] <= 26);
      SYNC_REQUIRE(p[1] >= 49 && p[1] <= 51);
      SYNC_REQUIRE(p[2] >= 99 && p[2] <= 101);
      SYNC_REQUIRE(p[3] == 255);
    }
  }
}
