#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include <sync/platform/linux_camera_device.hpp>

namespace noisefactor::sync::linux_camera_setup {

enum class LinuxCameraSetupResult {
  Configured,
  Adopted,
  NeedsNewLogin,
  Conflict,
  InvalidUser,
  MustRunAsRoot,
  ModuleLoadFailed,
  DeviceValidationFailed,
  Io,
};

struct LinuxCameraSetupOptions {
  std::string_view user_name;
  std::string_view template_directory = "/usr/share/noisedeck-sync";
  std::string_view etc_root = "/etc";
};

struct LinuxAccount {
  std::array<char, 64> name{};
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
  std::array<char, 1'024> home{};
};

struct LinuxCameraMachineState {
  bool module_loaded = false;
  bool has_any_loopback_device = false;
  bool has_compatible_sync_camera = false;
  bool has_modprobe_options = false;
};

class LinuxCameraSetupOps {
 public:
  virtual ~LinuxCameraSetupOps() = default;
  [[nodiscard]] virtual auto effective_uid() const noexcept -> std::uint32_t = 0;
  virtual auto resolve_user(std::string_view name,
                            LinuxAccount& output) noexcept -> bool = 0;
  virtual auto inspect_state() noexcept -> LinuxCameraMachineState = 0;
  virtual auto ensure_group_and_member(const LinuxAccount& account) noexcept
      -> bool = 0;
  virtual auto managed_file_matches(std::string_view destination) noexcept
      -> bool = 0;
  virtual auto install_managed_file(std::string_view source,
                                    std::string_view destination) noexcept
      -> bool = 0;
  virtual void remove_managed_file(std::string_view destination) noexcept = 0;
  virtual auto run_fixed(std::span<const std::string_view> arguments) noexcept
      -> int = 0;
  virtual auto discover_camera() noexcept -> camera::LinuxCameraOpenResult = 0;
};

[[nodiscard]] auto default_linux_camera_setup_ops() noexcept
    -> LinuxCameraSetupOps&;
[[nodiscard]] auto setup_linux_camera(const LinuxCameraSetupOptions& options,
                                      LinuxCameraSetupOps& operations) noexcept
    -> LinuxCameraSetupResult;
[[nodiscard]] auto describe(LinuxCameraSetupResult result) noexcept
    -> const char*;

}  // namespace noisefactor::sync::linux_camera_setup
