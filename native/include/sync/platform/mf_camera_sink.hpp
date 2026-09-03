#pragma once

#if !defined(_WIN32)
#error "mf_camera_sink.hpp is available only on Windows"
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <sync/camera/frame_ring.hpp>
#include <sync/platform/camera_sink.hpp>

namespace noisefactor::sync::camera {

// Feeds the Sync camera through the shared ring the media source owns: open
// the section by name, map it, and write one fitted frame per submit.
//
// The mirror of CmioCameraSink, with the ownership reversed. On macOS the
// extension hands the daemon a queue; here the daemon opens a section the
// source created, because only a process in session 0 can create one.
//
// Discovery happens once at construction, exactly as it does on macOS. A
// source registered later is picked up by restarting the daemon, which the
// tray app does when it finishes enabling the camera.
class MfCameraSink final : public CameraSink {
 public:
  struct Options {
    // The kernel objects the media source owns. Production uses the Global
    // names; a test overrides them with Local ones, because creating a Global
    // object needs SeCreateGlobalPrivilege and only a session 0 service has
    // it -- the same asymmetry that made the source own the section in the
    // first place.
    std::wstring section = section_name();
    std::wstring frame_event = frame_event_name();
  };

  MfCameraSink();
  explicit MfCameraSink(Options options);
  ~MfCameraSink() override;

  MfCameraSink(const MfCameraSink&) = delete;
  auto operator=(const MfCameraSink&) -> MfCameraSink& = delete;
  MfCameraSink(MfCameraSink&&) = delete;
  auto operator=(MfCameraSink&&) -> MfCameraSink& = delete;

  [[nodiscard]] auto available() const noexcept -> bool override;
  [[nodiscard]] auto unavailable_reason() const noexcept
      -> CameraSinkUnavailableReason override;
  [[nodiscard]] auto unavailable_status() const noexcept -> std::int32_t override;
  [[nodiscard]] auto has_capacity() const noexcept -> bool override;
  auto submit(const CameraSinkFrame& frame) noexcept -> CameraSinkSubmit override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// True when the running Windows build is 22000 or later, where
// MFCreateVirtualCamera exists at all.
[[nodiscard]] auto windows_supports_virtual_cameras() noexcept -> bool;

}  // namespace noisefactor::sync::camera
