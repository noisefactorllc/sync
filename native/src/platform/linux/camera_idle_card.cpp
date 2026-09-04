#include <sync/platform/camera_idle_card.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace noisefactor::sync::camera {
namespace {

constexpr std::string_view kMessage = "Sync: waiting for Noisedeck";
constexpr std::uint32_t kGlyphWidth = 5;
constexpr std::uint32_t kGlyphHeight = 7;
constexpr std::uint32_t kGlyphAdvance = 6;
constexpr std::uint32_t kScale = 8;

constexpr auto glyph(char value) noexcept -> std::array<std::uint8_t, 7> {
  switch (value) {
    case 'S': return {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
    case 'N': return {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11};
    case 'a': return {0x00, 0x00, 0x0e, 0x01, 0x0f, 0x11, 0x0f};
    case 'c': return {0x00, 0x00, 0x0f, 0x10, 0x10, 0x10, 0x0f};
    case 'd': return {0x01, 0x01, 0x0f, 0x11, 0x11, 0x11, 0x0f};
    case 'e': return {0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0f};
    case 'f': return {0x06, 0x08, 0x08, 0x1e, 0x08, 0x08, 0x08};
    case 'g': return {0x00, 0x00, 0x0f, 0x11, 0x0f, 0x01, 0x1e};
    case 'i': return {0x04, 0x00, 0x0c, 0x04, 0x04, 0x04, 0x0e};
    case 'k': return {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12};
    case 'n': return {0x00, 0x00, 0x1e, 0x11, 0x11, 0x11, 0x11};
    case 'o': return {0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e};
    case 'r': return {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10};
    case 's': return {0x00, 0x00, 0x0f, 0x10, 0x0e, 0x01, 0x1e};
    case 't': return {0x08, 0x08, 0x1e, 0x08, 0x08, 0x09, 0x06};
    case 'w': return {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0a};
    case 'y': return {0x00, 0x00, 0x11, 0x11, 0x0f, 0x01, 0x1e};
    case ':': return {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00};
    default: return {};
  }
}

void paint(std::byte* pixel, std::uint8_t blue, std::uint8_t green,
           std::uint8_t red) noexcept {
  pixel[0] = static_cast<std::byte>(blue);
  pixel[1] = static_cast<std::byte>(green);
  pixel[2] = static_cast<std::byte>(red);
  pixel[3] = std::byte{0xff};
}

}  // namespace

auto draw_camera_idle_card(std::span<std::byte> bgra,
                           std::size_t canvas_stride,
                           CameraCanvas canvas) noexcept -> bool {
  const std::size_t row_bytes =
      static_cast<std::size_t>(canvas.width) * kBytesPerPixel;
  if (canvas.width == 0 || canvas.height == 0 || canvas_stride < row_bytes ||
      canvas_stride > std::numeric_limits<std::size_t>::max() / canvas.height ||
      bgra.size() < canvas_stride * canvas.height) {
    return false;
  }
  constexpr std::uint32_t text_width =
      (static_cast<std::uint32_t>(kMessage.size()) * kGlyphAdvance - 1U) *
      kScale;
  constexpr std::uint32_t text_height = kGlyphHeight * kScale;
  if (canvas.width < text_width || canvas.height < text_height) return false;

  for (std::uint32_t row = 0; row < canvas.height; ++row) {
    std::byte* pixel =
        bgra.data() + static_cast<std::size_t>(row) * canvas_stride;
    for (std::uint32_t column = 0; column < canvas.width; ++column) {
      paint(pixel, 0x14, 0x14, 0x14);
      pixel += kBytesPerPixel;
    }
  }

  const std::uint32_t origin_x = (canvas.width - text_width) / 2U;
  const std::uint32_t origin_y = (canvas.height - text_height) / 2U;
  for (std::size_t character = 0; character < kMessage.size(); ++character) {
    const auto rows = glyph(kMessage[character]);
    for (std::uint32_t glyph_y = 0; glyph_y < kGlyphHeight; ++glyph_y) {
      for (std::uint32_t glyph_x = 0; glyph_x < kGlyphWidth; ++glyph_x) {
        if ((rows[glyph_y] & (1U << (kGlyphWidth - glyph_x - 1U))) == 0) {
          continue;
        }
        const std::uint32_t left = origin_x +
            static_cast<std::uint32_t>(character) * kGlyphAdvance * kScale +
            glyph_x * kScale;
        const std::uint32_t top = origin_y + glyph_y * kScale;
        for (std::uint32_t dy = 0; dy < kScale; ++dy) {
          std::byte* pixel =
              bgra.data() + static_cast<std::size_t>(top + dy) * canvas_stride +
              static_cast<std::size_t>(left) * kBytesPerPixel;
          for (std::uint32_t dx = 0; dx < kScale; ++dx) {
            paint(pixel, 0xcc, 0xc9, 0xc7);
            pixel += kBytesPerPixel;
          }
        }
      }
    }
  }
  return true;
}

}  // namespace noisefactor::sync::camera
