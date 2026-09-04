#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include <sync/platform/linux_camera_device.hpp>

namespace noisefactor::sync::linux_diagnostics {

enum class DiagnosticSeverity { Info, Optional, Required };

struct DiagnosticFinding {
  std::array<char, 48> code{};
  DiagnosticSeverity severity = DiagnosticSeverity::Info;
  bool ok = false;
  std::array<char, 512> detail{};
};

struct LinuxDoctorReport {
  std::array<DiagnosticFinding, 64> findings{};
  std::size_t count = 0;
  bool required_ready = false;
};

struct LinuxDiagnosticProcessResult {
  int exit_code = -1;
  bool truncated = false;
  std::array<char, 16'384> output{};
  std::size_t output_length = 0;
};

class LinuxDiagnosticProcessOps {
 public:
  virtual ~LinuxDiagnosticProcessOps() = default;
  virtual auto run(std::span<const std::string_view> arguments) noexcept
      -> LinuxDiagnosticProcessResult = 0;
};

struct LinuxDoctorOptions {
  std::string_view filesystem_root = "/";
  std::string_view runtime_directory{};
  std::string_view selected_camera_path{};
  bool camera_selected = true;
  bool ndi_selected = true;
  camera::LinuxCameraDeviceOps* camera_operations = nullptr;
  LinuxDiagnosticProcessOps* process_operations = nullptr;
};

[[nodiscard]] auto default_linux_diagnostic_process_ops() noexcept
    -> LinuxDiagnosticProcessOps&;
[[nodiscard]] auto run_linux_doctor(const LinuxDoctorOptions& options) noexcept
    -> LinuxDoctorReport;
[[nodiscard]] auto render_linux_doctor_json(const LinuxDoctorReport& report)
    -> std::string;
[[nodiscard]] auto render_linux_doctor_human(const LinuxDoctorReport& report)
    -> std::string;

}  // namespace noisefactor::sync::linux_diagnostics
