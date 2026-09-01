#include <sync/platform/camera_frame_fitter.hpp>

#import <Accelerate/Accelerate.h>

#include <algorithm>
#include <cstring>
#include <vector>

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

void fill_black_opaque(std::span<std::byte> canvas_bytes, std::size_t canvas_stride,
                       CameraCanvas canvas) noexcept {
  for (std::uint32_t row = 0; row < canvas.height; ++row) {
    std::byte* line = canvas_bytes.data() + static_cast<std::size_t>(row) * canvas_stride;
    for (std::uint32_t x = 0; x < canvas.width; ++x) {
      line[static_cast<std::size_t>(x) * 4U + 0] = std::byte{0};
      line[static_cast<std::size_t>(x) * 4U + 1] = std::byte{0};
      line[static_cast<std::size_t>(x) * 4U + 2] = std::byte{0};
      line[static_cast<std::size_t>(x) * 4U + 3] = std::byte{255};
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
                      std::size_t canvas_stride, CameraCanvas canvas) noexcept -> bool {
  if (!frame_is_fittable(frame)) return false;
  if (canvas_stride < static_cast<std::size_t>(canvas.width) * kBytesPerPixel) return false;
  if (canvas_bytes.size() < canvas_stride * canvas.height) return false;
  const auto placement = compute_camera_placement(frame.width, frame.height, canvas);
  if (!placement.has_value()) return false;

  fill_black_opaque(canvas_bytes, canvas_stride, canvas);

  // RGBA -> BGRA is a channel permutation. The alpha handling that follows
  // works on the permuted copy, so the source is never written.
  std::vector<std::byte> swapped;
  try {
    swapped.resize(static_cast<std::size_t>(frame.width) * frame.height * kBytesPerPixel);
  } catch (...) {
    return false;
  }
  vImage_Buffer source{
      .data = const_cast<std::byte*>(frame.payload.data()),
      .height = frame.height,
      .width = frame.width,
      .rowBytes = frame.row_stride,
  };
  vImage_Buffer swapped_buffer{
      .data = swapped.data(),
      .height = frame.height,
      .width = frame.width,
      .rowBytes = static_cast<std::size_t>(frame.width) * kBytesPerPixel,
  };
  const uint8_t permute[4] = {2, 1, 0, 3};
  if (vImagePermuteChannels_ARGB8888(&source, &swapped_buffer, permute, kvImageNoFlags) !=
      kvImageNoError) {
    return false;
  }
  if (frame.alpha_mode == kAlphaStraight) {
    // The RGBA8888 premultiply treats the last channel as alpha regardless
    // of the order of the first three, so it is correct for BGRA too.
    if (vImagePremultiplyData_RGBA8888(&swapped_buffer, &swapped_buffer, kvImageNoFlags) !=
        kvImageNoError) {
      return false;
    }
  }
  // A camera has no alpha to offer: force opaque after any premultiply.
  for (std::uint32_t row = 0; row < frame.height; ++row) {
    std::byte* line = swapped.data() + static_cast<std::size_t>(row) * swapped_buffer.rowBytes;
    for (std::uint32_t x = 0; x < frame.width; ++x) {
      line[static_cast<std::size_t>(x) * 4U + 3] = std::byte{255};
    }
  }

  std::byte* destination_origin = canvas_bytes.data() +
                                  static_cast<std::size_t>(placement->y) * canvas_stride +
                                  static_cast<std::size_t>(placement->x) * kBytesPerPixel;
  if (placement->width == frame.width && placement->height == frame.height) {
    const std::size_t row_bytes = static_cast<std::size_t>(frame.width) * kBytesPerPixel;
    for (std::uint32_t row = 0; row < frame.height; ++row) {
      std::memcpy(destination_origin + static_cast<std::size_t>(row) * canvas_stride,
                  swapped.data() + static_cast<std::size_t>(row) * swapped_buffer.rowBytes,
                  row_bytes);
    }
    return true;
  }
  vImage_Buffer destination{
      .data = destination_origin,
      .height = placement->height,
      .width = placement->width,
      .rowBytes = canvas_stride,
  };
  return vImageScale_ARGB8888(&swapped_buffer, &destination, nullptr, kvImageNoFlags) ==
         kvImageNoError;
}

}  // namespace noisefactor::sync::camera
