#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <sync/protocol.hpp>

namespace noisefactor::sync::ndi {

// The caller validates the FrameView and provides a separate destination with
// width * height * 4 bytes. NDI receives tightly packed, straight-alpha RGBA:
// https://docs.ndi.video/all/developing-with-ndi/sdk/frame-types
inline void copy_rgba_frame(const protocol::FrameView& frame, std::byte* destination) noexcept {
  const std::size_t packed_row_bytes = static_cast<std::size_t>(frame.width) * 4U;
  if (static_cast<std::size_t>(frame.row_stride) == packed_row_bytes) {
    std::memcpy(destination, frame.payload.data(), frame.payload.size());
  } else {
    for (std::uint32_t row = 0; row < frame.height; ++row) {
      std::memcpy(destination + static_cast<std::size_t>(row) * packed_row_bytes,
                  frame.payload.data() + static_cast<std::size_t>(row) * frame.row_stride,
                  packed_row_bytes);
    }
  }

  if (frame.alpha_mode == 2) {  // STRAIGHT already matches NDI.
    return;
  }
  const std::size_t packed_bytes = packed_row_bytes * frame.height;
  for (std::size_t offset = 0; offset < packed_bytes; offset += 4U) {
    if (frame.alpha_mode == 1) {  // OPAQUE ignores the source alpha byte.
      destination[offset + 3U] = std::byte{255};
      continue;
    }

    const auto alpha = std::to_integer<unsigned int>(destination[offset + 3U]);
    for (std::size_t channel = 0; channel < 3U; ++channel) {
      const auto value = std::to_integer<unsigned int>(destination[offset + channel]);
      // Round to the nearest byte, saturating inconsistent premultiplied
      // inputs. A zero-alpha pixel has no recoverable color and becomes black.
      const auto straight = alpha == 0 ? 0U : std::min(255U, (value * 255U + alpha / 2U) / alpha);
      destination[offset + channel] = static_cast<std::byte>(straight);
    }
  }
}

}  // namespace noisefactor::sync::ndi
