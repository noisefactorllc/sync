#include <sync/platform/camera_frame_fitter.hpp>

#import <Accelerate/Accelerate.h>

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#include <dispatch/dispatch.h>
#endif

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

#if defined(__aarch64__)
// Match vImage's rounded color * alpha / 255. The largest intermediate is
// 65407, so each lane remains within 16 bits.
[[nodiscard]] inline auto premultiply_channels(uint8x16_t color, uint8x16_t alpha) noexcept
    -> uint8x16_t {
  const auto low = vaddq_u16(vmull_u8(vget_low_u8(color), vget_low_u8(alpha)),
                            vdupq_n_u16(128));
  const auto high = vaddq_u16(vmull_u8(vget_high_u8(color), vget_high_u8(alpha)),
                             vdupq_n_u16(128));
  return vcombine_u8(vshrn_n_u16(vsraq_n_u16(low, low, 8), 8),
                      vshrn_n_u16(vsraq_n_u16(high, high, 8), 8));
}

// Each invocation writes only its row range and uses no scratch storage.
template <bool Straight>
void convert_rows(const protocol::FrameView& frame,
                   const vImage_Buffer& destination,
                   std::uint32_t first, std::uint32_t end) noexcept {
  for (std::uint32_t y = first; y < end; ++y) {
    const auto* in = reinterpret_cast<const std::uint8_t*>(frame.payload.data()) +
                     static_cast<std::size_t>(y) * frame.row_stride;
    auto* out = static_cast<std::uint8_t*>(destination.data) +
                static_cast<std::size_t>(y) * destination.rowBytes;
    std::uint32_t x = 0;
    for (; frame.width - x >= 16; x += 16, in += 64, out += 64) {
      const uint8x16x4_t rgba = vld4q_u8(in);
      uint8x16x4_t bgra = {{rgba.val[2], rgba.val[1], rgba.val[0], vdupq_n_u8(255)}};
      if constexpr (Straight) {
        bgra.val[0] = premultiply_channels(bgra.val[0], rgba.val[3]);
        bgra.val[1] = premultiply_channels(bgra.val[1], rgba.val[3]);
        bgra.val[2] = premultiply_channels(bgra.val[2], rgba.val[3]);
      }
      vst4q_u8(out, bgra);
    }
    for (; x < frame.width; ++x, in += 4, out += 4) {
      const auto r = in[0], g = in[1], b = in[2];
      if constexpr (Straight) {
        const auto multiply = [alpha = in[3]](std::uint8_t value) {
          const std::uint32_t scaled = std::uint32_t(value) * alpha + 128U;
          return static_cast<std::uint8_t>((scaled + (scaled >> 8)) >> 8);
        };
        out[0] = multiply(b);
        out[1] = multiply(g);
        out[2] = multiply(r);
      } else {
        out[0] = b;
        out[1] = g;
        out[2] = r;
      }
      out[3] = 255;
    }
  }
}

template <bool Straight>
void convert_arm64_into(const protocol::FrameView& frame,
                        const vImage_Buffer& destination) noexcept {
  constexpr std::uint64_t parallel_pixels = 512U * 512U;
  if (frame.height < 2 || std::uint64_t(frame.width) * frame.height < parallel_pixels) {
    convert_rows<Straight>(frame, destination, 0, frame.height);
    return;
  }
  struct Work {
    const protocol::FrameView& frame;
    const vImage_Buffer& destination;
  } work{frame, destination};
  // Exactly two ranges, synchronously joined before the borrowed frame or
  // destination can be reused. Dispatch owns the reusable worker pool.
  dispatch_apply_f(2, DISPATCH_APPLY_AUTO, &work, [](void* context, std::size_t index) {
    const auto& job = *static_cast<Work*>(context);
    const auto middle = job.frame.height / 2;
    convert_rows<Straight>(job.frame, job.destination,
                           index == 0 ? 0 : middle,
                           index == 0 ? middle : job.frame.height);
  });
}
#endif

// ARM64 combines channel permutation, optional premultiplication, and opaque
// alpha in one pass. Other targets use vImage. The source is never written.
[[nodiscard]] auto convert_into(const protocol::FrameView& frame,
                                vImage_Buffer& destination) noexcept -> bool {
#if defined(__aarch64__)
  if (frame.alpha_mode == kAlphaStraight) {
    convert_arm64_into<true>(frame, destination);
  } else {
    convert_arm64_into<false>(frame, destination);
  }
  return true;
#else
  // vImage's channel mask for the last of four channels: alpha in BGRA.
  constexpr uint8_t kAlphaChannelMask = 0x1;
  vImage_Buffer source{
      .data = const_cast<std::byte*>(frame.payload.data()),
      .height = frame.height,
      .width = frame.width,
      .rowBytes = frame.row_stride,
  };
  const uint8_t permute[4] = {2, 1, 0, 3};
  if (frame.alpha_mode == kAlphaStraight) {
    if (vImagePremultiplyData_RGBA8888(&source, &destination, kvImageNoFlags) !=
        kvImageNoError) {
      return false;
    }
    return vImagePermuteChannelsWithMaskedInsert_ARGB8888(
               &destination, &destination, permute, kAlphaChannelMask, kBlackOpaqueBgra,
               kvImageNoFlags) == kvImageNoError;
  }
  // Preserve the established vImage conversion on other architectures.
  if (vImagePermuteChannels_ARGB8888(&source, &destination, permute, kvImageNoFlags) !=
      kvImageNoError) {
    return false;
  }
  return vImageOverwriteChannelsWithScalar_ARGB8888(255, &destination, &destination,
                                                    kAlphaChannelMask, kvImageNoFlags) ==
         kvImageNoError;
#endif
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
