#pragma once

#if !defined(__APPLE__)
#error "camera_frame_fitter.hpp is available only on Apple platforms"
#endif

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <sync/platform/camera_identity.hpp>
#include <sync/protocol.hpp>

namespace noisefactor::sync::camera {

struct CameraPlacement {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

// Aspect-preserving fit of a source into the canvas, centered. Empty when
// either dimension is zero.
[[nodiscard]] auto compute_camera_placement(std::uint32_t source_width,
                                            std::uint32_t source_height,
                                            CameraCanvas canvas) noexcept
    -> std::optional<CameraPlacement>;

// Writes the frame into canvas_bytes as top-down 32BGRA with opaque alpha,
// black outside the placement. Straight alpha is premultiplied over black.
// Returns false for anything but a top-down RGBA8 frame whose payload covers
// row_stride * height, or a canvas buffer smaller than canvas_stride * height.
[[nodiscard]] auto fit_camera_frame(const protocol::FrameView& frame,
                                    std::span<std::byte> canvas_bytes,
                                    std::size_t canvas_stride,
                                    CameraCanvas canvas) noexcept -> bool;

}  // namespace noisefactor::sync::camera
