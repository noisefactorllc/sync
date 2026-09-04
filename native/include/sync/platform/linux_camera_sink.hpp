#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <sync/daemon_metrics.hpp>
#include <sync/platform/camera_sink.hpp>
#include <sync/platform/linux_camera_device.hpp>

namespace noisefactor::sync::camera {

class LinuxCameraSink final : public CameraSink {
 public:
  struct Options {
    std::string_view device_path{};
    LinuxCameraDeviceOps* device_operations = nullptr;
    DaemonMetrics* metrics = nullptr;
    std::uint64_t (*clock_ms)() noexcept = nullptr;
    void (*health_changed)(void* context, bool healthy,
                           CameraSinkUnavailableReason reason,
                           std::int32_t status) noexcept = nullptr;
    void* health_context = nullptr;
  };

  LinuxCameraSink();
  explicit LinuxCameraSink(Options options);
  ~LinuxCameraSink() noexcept override;
  LinuxCameraSink(const LinuxCameraSink&) = delete;
  LinuxCameraSink& operator=(const LinuxCameraSink&) = delete;

  [[nodiscard]] auto available() const noexcept -> bool override;
  [[nodiscard]] auto unavailable_reason() const noexcept
      -> CameraSinkUnavailableReason override;
  [[nodiscard]] auto unavailable_status() const noexcept
      -> std::int32_t override;
  [[nodiscard]] auto has_capacity() const noexcept -> bool override;
  auto submit(const CameraSinkFrame& frame) noexcept
      -> CameraSinkSubmit override;
  [[nodiscard]] auto healthy() const noexcept -> bool;
  [[nodiscard]] auto current_reason() const noexcept
      -> CameraSinkUnavailableReason;
  [[nodiscard]] auto current_status() const noexcept -> std::int32_t;
  [[nodiscard]] auto device_path() const noexcept -> std::string_view;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace noisefactor::sync::camera
