#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

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

// Working memory the fitter needs for a frame that must be scaled. Owned by
// the caller so it is sized once to the working set and reused: the daemon
// fits sixty frames a second for the length of a show, and an allocation per
// frame is both time on the hot path and heap churn that compounds over hours.
// A frame that already matches the canvas needs none of it.
struct CameraFitScratch {
  // Permuted, premultiplied, opaque BGRA copy of the source at source size.
  std::vector<std::byte> swapped;
  // Scratch for the scale step. Sized by the implementation; unused when the
  // frame already matches the canvas.
  std::vector<std::byte> scale_temp;
};

// Writes the frame into canvas_bytes as top-down 32BGRA with opaque alpha,
// black outside the placement. Straight alpha is premultiplied over black.
// Returns false for anything but a top-down RGBA8 frame whose payload covers
// row_stride * height, or a canvas buffer smaller than canvas_stride * height.
// Only the bars outside the placement are painted black; the placement itself
// is fully overwritten by the frame.
[[nodiscard]] auto fit_camera_frame(const protocol::FrameView& frame,
                                    std::span<std::byte> canvas_bytes,
                                    std::size_t canvas_stride, CameraCanvas canvas,
                                    CameraFitScratch& scratch) noexcept -> bool;

// Convenience for one-off fits: allocates scratch for this call. Not for the
// per-frame path.
[[nodiscard]] auto fit_camera_frame(const protocol::FrameView& frame,
                                    std::span<std::byte> canvas_bytes,
                                    std::size_t canvas_stride,
                                    CameraCanvas canvas) noexcept -> bool;

}  // namespace noisefactor::sync::camera
