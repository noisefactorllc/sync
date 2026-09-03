#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace noisefactor::sync::camera {

// Bytes an NV12 image occupies: a full-size Y plane followed by a half-height
// interleaved UV plane, both at y_stride.
[[nodiscard]] auto nv12_size_bytes(std::uint32_t width, std::uint32_t height,
                                   std::size_t y_stride) noexcept -> std::size_t;

// Converts top-down opaque BGRA to NV12 (BT.709, studio range), which is what
// a consumer assumes for an HD frame and what the media type declares.
// Chroma is box-averaged over each 2x2 block.
//
// Both dimensions must be even. An odd width would need ceil(width/2)*2 bytes
// per chroma row -- more than y_stride -- so the last UV pair would spill into
// the following row. The canvas is a fixed 1920x1080, so rejecting odd sizes
// costs nothing and removes the whole class of overflow.
//
// Returns false when either dimension is zero or odd, when a stride is too
// small for its width, or when a buffer is too small for its plane.
[[nodiscard]] auto bgra_to_nv12(std::span<const std::byte> bgra, std::size_t bgra_stride,
                                std::uint32_t width, std::uint32_t height,
                                std::span<std::byte> nv12, std::size_t y_stride) noexcept -> bool;

}  // namespace noisefactor::sync::camera
