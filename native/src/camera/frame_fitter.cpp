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

// 255-scaled premultiply that rounds the way vImage does, so a frame that
// already matches the canvas comes out byte for byte identical to the same
// frame fitted on macOS. A frame that has to be scaled will not match exactly
// -- macOS resamples through vImageScale_ARGB8888 and this resamples
// bilinearly -- but both are properly filtered, so neither is the aliased one.
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

// One source pixel, already permuted to BGRA and premultiplied if needed.
struct SourcePixel {
  std::uint32_t b = 0;
  std::uint32_t g = 0;
  std::uint32_t r = 0;
};

[[nodiscard]] auto sample(const protocol::FrameView& frame, bool straight, std::uint32_t x,
                          std::uint32_t y) noexcept -> SourcePixel {
  const std::byte* in = frame.payload.data() + static_cast<std::size_t>(y) * frame.row_stride +
                        static_cast<std::size_t>(x) * kBytesPerPixel;
  const auto r = static_cast<std::uint8_t>(in[0]);
  const auto g = static_cast<std::uint8_t>(in[1]);
  const auto b = static_cast<std::uint8_t>(in[2]);
  const auto a = static_cast<std::uint8_t>(in[3]);
  return {
      .b = straight ? premultiply(b, a) : b,
      .g = straight ? premultiply(g, a) : g,
      .r = straight ? premultiply(r, a) : r,
  };
}

// Scales the source into the placement, permuting RGBA to BGRA,
// premultiplying straight alpha over black, and forcing opaque. One pass, no
// intermediate buffer: the destination is written exactly once.
//
// Filtered, not nearest-neighbour. macOS fits through vImageScale_ARGB8888,
// and point sampling would put visibly aliased edges and stair-stepped text
// on any output whose size differs from the 1920x1080 canvas -- which is most
// of them -- while the same source looked clean through the macOS camera.
//
// Shrinking averages every source pixel the destination pixel covers, because
// bilinear does not help there: at an exact 2x reduction its samples land on
// source pixel centres and it degenerates to point sampling, alias and all.
// Growing interpolates bilinearly, where averaging one pixel would do nothing.
void scale_permute_into(const protocol::FrameView& frame, std::span<std::byte> canvas_bytes,
                        std::size_t canvas_stride, const CameraPlacement& placement) noexcept {
  const bool straight = frame.alpha_mode == kAlphaStraight;
  // 16.16 fixed point throughout.
  const std::uint64_t x_step =
      (static_cast<std::uint64_t>(frame.width) << 16U) / placement.width;
  const std::uint64_t y_step =
      (static_cast<std::uint64_t>(frame.height) << 16U) / placement.height;
  const std::uint32_t last_column = frame.width - 1;
  const std::uint32_t last_row = frame.height - 1;

  for (std::uint32_t row = 0; row < placement.height; ++row) {
    const std::uint64_t top_edge = static_cast<std::uint64_t>(row) * y_step;
    const std::uint64_t bottom_edge = top_edge + y_step;
    const std::uint32_t first_row =
        std::min<std::uint32_t>(static_cast<std::uint32_t>(top_edge >> 16U), last_row);
    const std::uint32_t final_row = std::min<std::uint32_t>(
        static_cast<std::uint32_t>((bottom_edge - 1) >> 16U), last_row);

    std::byte* out = canvas_bytes.data() +
                     static_cast<std::size_t>(placement.y + row) * canvas_stride +
                     static_cast<std::size_t>(placement.x) * kBytesPerPixel;
    for (std::uint32_t column = 0; column < placement.width; ++column) {
      const std::uint64_t left_edge = static_cast<std::uint64_t>(column) * x_step;
      const std::uint64_t right_edge = left_edge + x_step;
      const std::uint32_t first_column =
          std::min<std::uint32_t>(static_cast<std::uint32_t>(left_edge >> 16U), last_column);
      const std::uint32_t final_column = std::min<std::uint32_t>(
          static_cast<std::uint32_t>((right_edge - 1) >> 16U), last_column);

      std::uint32_t b = 0;
      std::uint32_t g = 0;
      std::uint32_t r = 0;
      if (final_column > first_column || final_row > first_row) {
        std::uint32_t count = 0;
        for (std::uint32_t y = first_row; y <= final_row; ++y) {
          for (std::uint32_t x = first_column; x <= final_column; ++x) {
            const SourcePixel pixel = sample(frame, straight, x, y);
            b += pixel.b;
            g += pixel.g;
            r += pixel.r;
            ++count;
          }
        }
        b /= count;
        g /= count;
        r /= count;
      } else {
        // Growing: sample the centre of the destination pixel and interpolate.
        // Without the half-step the whole image shifts up and left by half a
        // source pixel.
        const std::uint64_t source_x = left_edge + x_step / 2;
        const std::uint64_t source_y = top_edge + y_step / 2;
        const std::uint32_t x0 =
            std::min<std::uint32_t>(static_cast<std::uint32_t>(source_x >> 16U), last_column);
        const std::uint32_t y0 =
            std::min<std::uint32_t>(static_cast<std::uint32_t>(source_y >> 16U), last_row);
        const std::uint32_t x1 = std::min<std::uint32_t>(x0 + 1, last_column);
        const std::uint32_t y1 = std::min<std::uint32_t>(y0 + 1, last_row);
        const std::uint32_t x_fraction = static_cast<std::uint32_t>(source_x & 0xFFFFU) >> 8U;
        const std::uint32_t y_fraction = static_cast<std::uint32_t>(source_y & 0xFFFFU) >> 8U;

        const SourcePixel top_left = sample(frame, straight, x0, y0);
        const SourcePixel top_right = sample(frame, straight, x1, y0);
        const SourcePixel bottom_left = sample(frame, straight, x0, y1);
        const SourcePixel bottom_right = sample(frame, straight, x1, y1);

        const auto blend = [&](std::uint32_t tl, std::uint32_t tr, std::uint32_t bl,
                               std::uint32_t br) noexcept -> std::uint32_t {
          const std::uint32_t top = tl * (256 - x_fraction) + tr * x_fraction;
          const std::uint32_t bottom = bl * (256 - x_fraction) + br * x_fraction;
          return (top * (256 - y_fraction) + bottom * y_fraction + 32768) >> 16U;
        };
        b = blend(top_left.b, top_right.b, bottom_left.b, bottom_right.b);
        g = blend(top_left.g, top_right.g, bottom_left.g, bottom_right.g);
        r = blend(top_left.r, top_right.r, bottom_left.r, bottom_right.r);
      }

      out[0] = static_cast<std::byte>(b > 255 ? 255 : b);
      out[1] = static_cast<std::byte>(g > 255 ? 255 : g);
      out[2] = static_cast<std::byte>(r > 255 ? 255 : r);
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
