#include <sync/platform/camera_frame_fitter.hpp>

#include <algorithm>
#include <cstring>

namespace noisefactor::sync::camera {

namespace {

constexpr std::uint16_t kPixelFormatRgba8 = 1;
constexpr std::uint16_t kAlphaStraight = 2;

[[nodiscard]] auto frame_is_fittable(const protocol::FrameView& frame) noexcept -> bool {
  if (frame.pixel_format != kPixelFormatRgba8 || !frame.top_down) return false;
  if (frame.width == 0 || frame.height == 0) return false;
  const std::uint64_t packed = static_cast<std::uint64_t>(frame.width) * kBytesPerPixel;
  if (frame.row_stride < packed) return false;
  const std::uint64_t needed = static_cast<std::uint64_t>(frame.row_stride) * frame.height;
  return frame.payload.size() >= needed;
}

// 255-scaled premultiply that rounds the way vImage does, so a frame fitted on
// Windows matches the same frame fitted on macOS byte for byte.
[[nodiscard]] constexpr auto premultiply(std::uint8_t value, std::uint8_t alpha) noexcept
    -> std::uint8_t {
  const std::uint32_t scaled = static_cast<std::uint32_t>(value) * alpha + 127U;
  return static_cast<std::uint8_t>((scaled + (scaled >> 8)) >> 8);
}

void fill_black(std::span<std::byte> canvas_bytes, std::size_t canvas_stride, std::uint32_t x,
                std::uint32_t y, std::uint32_t width, std::uint32_t height) noexcept {
  if (width == 0 || height == 0) return;
  for (std::uint32_t row = 0; row < height; ++row) {
    std::byte* out = canvas_bytes.data() + static_cast<std::size_t>(y + row) * canvas_stride +
                     static_cast<std::size_t>(x) * kBytesPerPixel;
    for (std::uint32_t column = 0; column < width; ++column) {
      out[0] = std::byte{0};
      out[1] = std::byte{0};
      out[2] = std::byte{0};
      out[3] = std::byte{255};
      out += kBytesPerPixel;
    }
  }
}

// Nearest-neighbour scale from the source into the placement, permuting RGBA
// to BGRA, premultiplying straight alpha over black, and forcing opaque. One
// pass, no intermediate buffer: the destination is written exactly once.
void scale_permute_into(const protocol::FrameView& frame, std::span<std::byte> canvas_bytes,
                        std::size_t canvas_stride, const CameraPlacement& placement) noexcept {
  const bool straight = frame.alpha_mode == kAlphaStraight;
  for (std::uint32_t row = 0; row < placement.height; ++row) {
    // Sample the centre of the destination texel, not its corner: corner
    // sampling biases the whole image up and left by half a source pixel.
    const std::uint64_t source_row =
        ((static_cast<std::uint64_t>(row) * 2 + 1) * frame.height) /
        (static_cast<std::uint64_t>(placement.height) * 2);
    const std::byte* in_row =
        frame.payload.data() +
        std::min<std::uint64_t>(source_row, frame.height - 1) * frame.row_stride;
    std::byte* out = canvas_bytes.data() +
                     static_cast<std::size_t>(placement.y + row) * canvas_stride +
                     static_cast<std::size_t>(placement.x) * kBytesPerPixel;
    for (std::uint32_t column = 0; column < placement.width; ++column) {
      const std::uint64_t source_column =
          ((static_cast<std::uint64_t>(column) * 2 + 1) * frame.width) /
          (static_cast<std::uint64_t>(placement.width) * 2);
      const std::byte* in =
          in_row + std::min<std::uint64_t>(source_column, frame.width - 1) * kBytesPerPixel;
      const auto r = static_cast<std::uint8_t>(in[0]);
      const auto g = static_cast<std::uint8_t>(in[1]);
      const auto b = static_cast<std::uint8_t>(in[2]);
      const auto a = static_cast<std::uint8_t>(in[3]);
      out[0] = static_cast<std::byte>(straight ? premultiply(b, a) : b);
      out[1] = static_cast<std::byte>(straight ? premultiply(g, a) : g);
      out[2] = static_cast<std::byte>(straight ? premultiply(r, a) : r);
      out[3] = std::byte{255};
      out += kBytesPerPixel;
    }
  }
}

}  // namespace

auto compute_camera_placement(std::uint32_t source_width, std::uint32_t source_height,
                              CameraCanvas canvas) noexcept -> std::optional<CameraPlacement> {
  if (source_width == 0 || source_height == 0 || canvas.width == 0 || canvas.height == 0) {
    return std::nullopt;
  }
  // The source is at least as wide as the canvas (relative to height) when
  // source_w * canvas_h >= canvas_w * source_h. Integer math only.
  const std::uint64_t lhs = static_cast<std::uint64_t>(source_width) * canvas.height;
  const std::uint64_t rhs = static_cast<std::uint64_t>(canvas.width) * source_height;
  CameraPlacement placement{};
  if (lhs >= rhs) {
    placement.width = canvas.width;
    placement.height = static_cast<std::uint32_t>(std::max<std::uint64_t>(
        1, (static_cast<std::uint64_t>(canvas.width) * source_height) / source_width));
  } else {
    placement.height = canvas.height;
    placement.width = static_cast<std::uint32_t>(std::max<std::uint64_t>(
        1, (static_cast<std::uint64_t>(canvas.height) * source_width) / source_height));
  }
  placement.x = (canvas.width - placement.width) / 2;
  placement.y = (canvas.height - placement.height) / 2;
  return placement;
}

auto fit_camera_frame(const protocol::FrameView& frame, std::span<std::byte> canvas_bytes,
                      std::size_t canvas_stride, CameraCanvas canvas,
                      CameraFitScratch& scratch) noexcept -> bool {
  (void)scratch;  // The one-pass path needs no working memory.
  if (!frame_is_fittable(frame)) return false;
  if (canvas_stride < static_cast<std::size_t>(canvas.width) * kBytesPerPixel) return false;
  if (canvas_bytes.size() < canvas_stride * canvas.height) return false;
  const auto placement = compute_camera_placement(frame.width, frame.height, canvas);
  if (!placement.has_value()) return false;
  // Only the bars are painted; the placement itself is fully overwritten by
  // the frame, so touching it here would be a second pass over the largest
  // part of the canvas on every frame.
  const std::uint32_t bottom = placement->y + placement->height;
  const std::uint32_t right = placement->x + placement->width;
  fill_black(canvas_bytes, canvas_stride, 0, 0, canvas.width, placement->y);
  fill_black(canvas_bytes, canvas_stride, 0, bottom, canvas.width, canvas.height - bottom);
  fill_black(canvas_bytes, canvas_stride, 0, placement->y, placement->x, placement->height);
  fill_black(canvas_bytes, canvas_stride, right, placement->y, canvas.width - right,
             placement->height);
  scale_permute_into(frame, canvas_bytes, canvas_stride, *placement);
  return true;
}

auto fit_camera_frame(const protocol::FrameView& frame, std::span<std::byte> canvas_bytes,
                      std::size_t canvas_stride, CameraCanvas canvas) noexcept -> bool {
  CameraFitScratch scratch;
  return fit_camera_frame(frame, canvas_bytes, canvas_stride, canvas, scratch);
}

}  // namespace noisefactor::sync::camera
