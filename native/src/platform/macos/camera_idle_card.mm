#include <sync/platform/camera_idle_card.hpp>

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

#include <cstring>

namespace noisefactor::sync::camera {

namespace {

constexpr CGFloat kTextPoints = 56;

void fill_black_opaque(std::span<std::byte> bgra, std::size_t stride, CameraCanvas canvas) noexcept {
  const std::size_t row_bytes = static_cast<std::size_t>(canvas.width) * kBytesPerPixel;
  std::byte* first = bgra.data();
  for (std::uint32_t x = 0; x < canvas.width; ++x) {
    first[static_cast<std::size_t>(x) * 4 + 0] = std::byte{0};
    first[static_cast<std::size_t>(x) * 4 + 1] = std::byte{0};
    first[static_cast<std::size_t>(x) * 4 + 2] = std::byte{0};
    first[static_cast<std::size_t>(x) * 4 + 3] = std::byte{255};
  }
  for (std::uint32_t row = 1; row < canvas.height; ++row) {
    std::memcpy(first + static_cast<std::size_t>(row) * stride, first, row_bytes);
  }
}

// Draws with CoreGraphics into the caller's bytes. BGRA in memory is
// CoreGraphics' 32-bit little-endian ARGB layout with premultiplied alpha.
[[nodiscard]] auto draw(std::span<std::byte> bgra, std::size_t stride,
                        CameraCanvas canvas) noexcept -> bool {
  @autoreleasepool {
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    if (space == nullptr) return false;
    CGContextRef context = CGBitmapContextCreate(
        bgra.data(), canvas.width, canvas.height, 8, stride, space,
        static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) |
            static_cast<uint32_t>(kCGBitmapByteOrder32Little));
    CGColorSpaceRelease(space);
    if (context == nullptr) return false;

    const CGFloat width = canvas.width;
    const CGFloat height = canvas.height;
    CGContextSetRGBFillColor(context, 0.055, 0.055, 0.07, 1.0);
    CGContextFillRect(context, CGRectMake(0, 0, width, height));

    // The card: a rounded panel half the canvas wide, centered.
    const CGRect card = CGRectMake(width * 0.25, height * 0.35, width * 0.5, height * 0.3);
    const CGFloat radius = height * 0.03;
    CGPathRef card_path = CGPathCreateWithRoundedRect(card, radius, radius, nullptr);
    CGContextSetRGBFillColor(context, 0.11, 0.11, 0.135, 1.0);
    CGContextAddPath(context, card_path);
    CGContextFillPath(context);
    CGPathRelease(card_path);

    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, kTextPoints, nullptr);
    if (font == nullptr) {
      CGContextRelease(context);
      return false;
    }
    CGColorRef ink = CGColorCreateGenericRGB(0.92, 0.92, 0.95, 1.0);
    if (ink == nullptr) {
      CFRelease(font);
      CGContextRelease(context);
      return false;
    }
    NSDictionary* attributes = @{
      (__bridge id)kCTFontAttributeName : (__bridge id)font,
      (__bridge id)kCTForegroundColorAttributeName : (__bridge id)ink,
    };
    NSAttributedString* text =
        [[NSAttributedString alloc] initWithString:@"Sync: waiting for Noisedeck"
                                        attributes:attributes];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)text);
    CGColorRelease(ink);
    CFRelease(font);
    if (line == nullptr) {
      CGContextRelease(context);
      return false;
    }
    const CGRect bounds = CTLineGetBoundsWithOptions(line, kCTLineBoundsUseGlyphPathBounds);
    CGContextSetTextPosition(context, (width - bounds.size.width) / 2 - bounds.origin.x,
                             (height - bounds.size.height) / 2 - bounds.origin.y);
    CTLineDraw(line, context);
    CFRelease(line);
    CGContextFlush(context);
    CGContextRelease(context);
    return true;
  }
}

}  // namespace

auto draw_camera_idle_card(std::span<std::byte> bgra, std::size_t canvas_stride,
                           CameraCanvas canvas) noexcept -> bool {
  if (canvas.width == 0 || canvas.height == 0 ||
      canvas_stride < static_cast<std::size_t>(canvas.width) * kBytesPerPixel ||
      bgra.size() < canvas_stride * canvas.height) {
    return false;
  }
  if (draw(bgra, canvas_stride, canvas)) return true;
  fill_black_opaque(bgra, canvas_stride, canvas);
  return false;
}

}  // namespace noisefactor::sync::camera
