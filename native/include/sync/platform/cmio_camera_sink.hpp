#pragma once

#if !defined(__APPLE__)
#error "cmio_camera_sink.hpp is available only on Apple platforms"
#endif

#include <cstddef>
#include <memory>
#include <string_view>

#include <sync/platform/camera_identity.hpp>
#include <sync/platform/camera_sink.hpp>

namespace noisefactor::sync::camera {

// Feeds the Sync Camera extension through its CoreMediaIO sink stream: find
// the device by UID, take its output-direction stream, copy its buffer queue,
// start it, and enqueue one sample buffer per frame.
//
// Discovery happens once at construction. An extension approved later is
// picked up by restarting the daemon, which Sync.app does when activation
// completes; the provider capabilities are computed once per run anyway.
class CmioCameraSink final : public CameraSink {
 public:
  struct Options {
    std::string_view device_uid = kDeviceUid;
    // Frames allowed in flight before submit() reports Backpressured.
    std::size_t queue_depth = 3;
  };

  CmioCameraSink();
  explicit CmioCameraSink(Options options);
  ~CmioCameraSink() override;

  CmioCameraSink(const CmioCameraSink&) = delete;
  auto operator=(const CmioCameraSink&) -> CmioCameraSink& = delete;
  CmioCameraSink(CmioCameraSink&&) = delete;
  auto operator=(CmioCameraSink&&) -> CmioCameraSink& = delete;

  [[nodiscard]] auto available() const noexcept -> bool override;
  [[nodiscard]] auto unavailable_reason() const noexcept
      -> CameraSinkUnavailableReason override;
  auto submit(const CameraSinkFrame& frame) noexcept -> CameraSinkSubmit override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace noisefactor::sync::camera
