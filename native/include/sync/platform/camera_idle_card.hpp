#pragma once

#if !defined(__APPLE__)
#error "camera_idle_card.hpp is available only on Apple platforms"
#endif

#include <cstddef>
#include <span>

#include <sync/platform/camera_identity.hpp>

namespace noisefactor::sync::camera {

// The frame the camera shows while no sender is feeding it: a dark card with
// the words "Sync: waiting for Noisedeck", so a consumer that opened the
// camera early sees that Sync is present rather than a black picture that
// reads as a broken device. Drawn once into a top-down 32BGRA buffer of
// canvas_stride * canvas.height bytes with opaque alpha. Returns false when
// the buffer cannot hold the canvas (left untouched) or when CoreGraphics
// cannot draw into it (painted opaque black instead).
[[nodiscard]] auto draw_camera_idle_card(std::span<std::byte> bgra, std::size_t canvas_stride,
                                         CameraCanvas canvas) noexcept -> bool;

}  // namespace noisefactor::sync::camera
