#pragma once

#if !defined(__APPLE__)
#error "metal_frame_consumer.hpp is available only on Apple platforms"
#endif

#import <Metal/Metal.h>

#include <cstdint>
#include <string_view>

namespace noisefactor::sync {

enum class MetalColorSpace : std::uint16_t {
  Srgb = 1,
  DisplayP3 = 2,
};

enum class MetalAlphaMode : std::uint16_t {
  Opaque = 1,
  Straight = 2,
  Premultiplied = 3,
};

struct MetalFrameMetadata {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  MetalColorSpace color_space = MetalColorSpace::Srgb;
  MetalAlphaMode alpha_mode = MetalAlphaMode::Opaque;
  std::uint64_t sequence = 0;
  std::uint64_t presentation_time_us = 0;
  bool top_down = true;
};

class MetalFrameConsumer {
 public:
  virtual ~MetalFrameConsumer() = default;

  virtual auto open_sender(std::string_view sender_id,
                           std::string_view name,
                           id<MTLDevice> device) noexcept -> bool = 0;
  virtual void close_sender(std::string_view sender_id) noexcept = 0;
  virtual auto encode_frame(std::string_view sender_id,
                            id<MTLTexture> texture,
                            id<MTLCommandBuffer> command_buffer,
                            const MetalFrameMetadata& metadata) noexcept -> bool = 0;
};

}  // namespace noisefactor::sync
