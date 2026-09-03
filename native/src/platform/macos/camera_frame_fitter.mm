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

// Opaque black in the 32BGRA byte order the canvas uses.
constexpr Pixel_8888 kBlackOpaqueBgra = {0, 0, 0, 255};
// vImage's channel mask for the last of four channels: alpha in BGRA.
constexpr uint8_t kAlphaChannelMask = 0x1;

[[nodiscard]] auto region(std::span<std::byte> canvas_bytes, std::size_t canvas_stride,
                          std::uint32_t x, std::uint32_t y, std::uint32_t width,
                          std::uint32_t height) noexcept -> vImage_Buffer {
  return {
      .data = canvas_bytes.data() + static_cast<std::size_t>(y) * canvas_stride +
              static_cast<std::size_t>(x) * kBytesPerPixel,
      .height = height,
      .width = width,
      .rowBytes = canvas_stride,
  };
}

// Paints only the bars around the placement. The placement itself is written
// in full by the caller, so touching it here would be a second pass over the
// largest part of the canvas on every frame.
[[nodiscard]] auto fill_bars_black(std::span<std::byte> canvas_bytes, std::size_t canvas_stride,
                                   CameraCanvas canvas,
                                   const CameraPlacement& placement) noexcept -> bool {
  const auto fill = [&](std::uint32_t x, std::uint32_t y, std::uint32_t width,
                        std::uint32_t height) noexcept -> bool {
    if (width == 0 || height == 0) return true;
    vImage_Buffer bar = region(canvas_bytes, canvas_stride, x, y, width, height);
    return vImageBufferFill_ARGB8888(&bar, kBlackOpaqueBgra, kvImageNoFlags) == kvImageNoError;
  };
  const std::uint32_t bottom = placement.y + placement.height;
  const std::uint32_t right = placement.x + placement.width;
  return fill(0, 0, canvas.width, placement.y) &&
         fill(0, bottom, canvas.width, canvas.height - bottom) &&
         fill(0, placement.y, placement.x, placement.height) &&
         fill(right, placement.y, canvas.width - right, placement.height);
}

// RGBA -> BGRA permute into `destination`, then straight alpha premultiplied
// over black in place, then alpha forced opaque in place. The source is never
// written.
[[nodiscard]] auto convert_into(const protocol::FrameView& frame,
                                vImage_Buffer& destination) noexcept -> bool {
  vImage_Buffer source{
      .data = const_cast<std::byte*>(frame.payload.data()),
      .height = frame.height,
      .width = frame.width,
      .rowBytes = frame.row_stride,
  };
  const uint8_t permute[4] = {2, 1, 0, 3};
  if (vImagePermuteChannels_ARGB8888(&source, &destination, permute, kvImageNoFlags) !=
      kvImageNoError) {
    return false;
  }
  if (frame.alpha_mode == kAlphaStraight) {
    // The RGBA8888 premultiply treats the last channel as alpha regardless
    // of the order of the first three, so it is correct for BGRA too.
    if (vImagePremultiplyData_RGBA8888(&destination, &destination, kvImageNoFlags) !=
        kvImageNoError) {
      return false;
    }
  }
  // A camera has no alpha to offer: force opaque after any premultiply.
  return vImageOverwriteChannelsWithScalar_ARGB8888(255, &destination, &destination,
                                                    kAlphaChannelMask, kvImageNoFlags) ==
         kvImageNoError;
}

[[nodiscard]] auto ensure_capacity(std::vector<std::byte>& buffer, std::size_t bytes) noexcept
    -> bool {
  if (buffer.size() >= bytes) return true;
  try {
    buffer.resize(bytes);
  } catch (...) {
    return false;
  }
  return true;
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
  if (!frame_is_fittable(frame)) return false;
  if (canvas_stride < static_cast<std::size_t>(canvas.width) * kBytesPerPixel) return false;
  if (canvas_bytes.size() < canvas_stride * canvas.height) return false;
  const auto placement = compute_camera_placement(frame.width, frame.height, canvas);
  if (!placement.has_value()) return false;
  if (!fill_bars_black(canvas_bytes, canvas_stride, canvas, *placement)) return false;

  vImage_Buffer destination = region(canvas_bytes, canvas_stride, placement->x, placement->y,
                                     placement->width, placement->height);
  if (placement->width == frame.width && placement->height == frame.height) {
    // The common case, a frame already at canvas size: convert straight into
    // the canvas with no intermediate at all.
    return convert_into(frame, destination);
  }

  const std::size_t swapped_bytes =
      static_cast<std::size_t>(frame.width) * frame.height * kBytesPerPixel;
  if (!ensure_capacity(scratch.swapped, swapped_bytes)) return false;
  vImage_Buffer swapped{
      .data = scratch.swapped.data(),
      .height = frame.height,
      .width = frame.width,
      .rowBytes = static_cast<std::size_t>(frame.width) * kBytesPerPixel,
  };
  if (!convert_into(frame, swapped)) return false;

  // vImage sizes its own temporary for this exact source/destination pair;
  // handing it a caller-owned one keeps the scale from allocating per frame.
  const vImage_Error temp_bytes =
      vImageScale_ARGB8888(&swapped, &destination, nullptr, kvImageGetTempBufferSize);
  if (temp_bytes < 0) return false;
  if (!ensure_capacity(scratch.scale_temp, static_cast<std::size_t>(temp_bytes))) return false;
  return vImageScale_ARGB8888(&swapped, &destination, scratch.scale_temp.data(),
                              kvImageNoFlags) == kvImageNoError;
}

auto fit_camera_frame(const protocol::FrameView& frame, std::span<std::byte> canvas_bytes,
                      std::size_t canvas_stride, CameraCanvas canvas) noexcept -> bool {
  CameraFitScratch scratch;
  return fit_camera_frame(frame, canvas_bytes, canvas_stride, canvas, scratch);
}

}  // namespace noisefactor::sync::camera
