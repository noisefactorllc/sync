#include <sync/camera/nv12.hpp>

#include <algorithm>

namespace noisefactor::sync::camera {

namespace {

constexpr std::size_t kBgraBytesPerPixel = 4;

// BT.709 studio range, fixed point in 16.16. Y in [16,235], chroma in [16,240]
// centred on 128.
//
// BT.709 and not BT.601: the canvas is 1920x1080, and a consumer handed HD
// NV12 decodes it as 709. Encoding 601 and letting the other side assume 709
// is a systematic colour error, not a rounding one -- rgb(96,192,64) came
// back from Chromium as rgb(90,176,65), and predicting that exact pair of
// values is what identified the mismatch. Grey hides it completely, which is
// why the round trip looked clean until a saturated colour went through.
//
// The media type declares MF_MT_YUV_MATRIX and MF_MT_VIDEO_NOMINAL_RANGE to
// match, so a consumer that reads the attributes and one that guesses from
// the frame size both arrive at the same answer.
constexpr std::int32_t kYr = 11966, kYg = 40254, kYb = 4064, kYOffset = 16 << 16;
constexpr std::int32_t kUr = -6595, kUg = -22187, kUb = 28782, kChromaOffset = 128 << 16;
constexpr std::int32_t kVr = 28782, kVg = -26146, kVb = -2639;

[[nodiscard]] constexpr auto clamp_byte(std::int32_t fixed) noexcept -> std::uint8_t {
  const std::int32_t value = (fixed + (1 << 15)) >> 16;
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

}  // namespace

auto nv12_size_bytes(std::uint32_t width, std::uint32_t height, std::size_t y_stride) noexcept
    -> std::size_t {
  (void)width;
  const std::size_t chroma_rows = (static_cast<std::size_t>(height) + 1) / 2;
  return y_stride * height + y_stride * chroma_rows;
}

auto bgra_to_nv12(std::span<const std::byte> bgra, std::size_t bgra_stride, std::uint32_t width,
                  std::uint32_t height, std::span<std::byte> nv12, std::size_t y_stride) noexcept
    -> bool {
  if (width == 0 || height == 0) return false;
  if ((width % 2) != 0 || (height % 2) != 0) return false;
  if (bgra_stride < static_cast<std::size_t>(width) * kBgraBytesPerPixel) return false;
  if (y_stride < width) return false;
  if (bgra.size() < bgra_stride * height) return false;
  if (nv12.size() < nv12_size_bytes(width, height, y_stride)) return false;

  std::byte* const luma = nv12.data();
  std::byte* const chroma = nv12.data() + y_stride * height;

  for (std::uint32_t row = 0; row < height; ++row) {
    const std::byte* in = bgra.data() + static_cast<std::size_t>(row) * bgra_stride;
    std::byte* out = luma + static_cast<std::size_t>(row) * y_stride;
    for (std::uint32_t column = 0; column < width; ++column) {
      const auto b = static_cast<std::int32_t>(static_cast<std::uint8_t>(in[0]));
      const auto g = static_cast<std::int32_t>(static_cast<std::uint8_t>(in[1]));
      const auto r = static_cast<std::int32_t>(static_cast<std::uint8_t>(in[2]));
      out[column] = static_cast<std::byte>(clamp_byte(kYr * r + kYg * g + kYb * b + kYOffset));
      in += kBgraBytesPerPixel;
    }
  }

  for (std::uint32_t row = 0; row < height; row += 2) {
    std::byte* out = chroma + static_cast<std::size_t>(row / 2) * y_stride;
    for (std::uint32_t column = 0; column < width; column += 2) {
      // Box-average the 2x2 block. Both dimensions are even, so every block is
      // complete and no sample is clamped.
      std::int32_t sum_r = 0, sum_g = 0, sum_b = 0;
      for (std::uint32_t dy = 0; dy < 2; ++dy) {
        const std::byte* in_row =
            bgra.data() + static_cast<std::size_t>(row + dy) * bgra_stride;
        for (std::uint32_t dx = 0; dx < 2; ++dx) {
          const std::byte* in =
              in_row + static_cast<std::size_t>(column + dx) * kBgraBytesPerPixel;
          sum_b += static_cast<std::int32_t>(static_cast<std::uint8_t>(in[0]));
          sum_g += static_cast<std::int32_t>(static_cast<std::uint8_t>(in[1]));
          sum_r += static_cast<std::int32_t>(static_cast<std::uint8_t>(in[2]));
        }
      }
      const std::int32_t r = sum_r / 4, g = sum_g / 4, b = sum_b / 4;
      out[column + 0] =
          static_cast<std::byte>(clamp_byte(kUr * r + kUg * g + kUb * b + kChromaOffset));
      out[column + 1] =
          static_cast<std::byte>(clamp_byte(kVr * r + kVg * g + kVb * b + kChromaOffset));
    }
  }
  return true;
}

}  // namespace noisefactor::sync::camera
