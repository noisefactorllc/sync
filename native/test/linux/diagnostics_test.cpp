#include "../test_harness.hpp"

#include <sync/camera/nv12.hpp>
#include <sync/platform/linux_diagnostics.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace diagnostics = noisefactor::sync::linux_diagnostics;
namespace camera = noisefactor::sync::camera;

class TempRoot {
 public:
  TempRoot() {
    const auto base = std::filesystem::temp_directory_path();
    path_ = base / ("sync-doctor-test-" + std::to_string(++counter_));
    std::filesystem::create_directories(path_ / "etc");
    std::filesystem::create_directories(path_ / "usr/lib");
    std::filesystem::create_directories(path_ / "sys/module/v4l2loopback");
    std::ofstream(path_ / "usr/lib/os-release")
        << "ID=ubuntu\nVERSION_ID=\"24.04\"\n";
    std::filesystem::create_symlink("../usr/lib/os-release",
                                    path_ / "etc/os-release");
  }
  ~TempRoot() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  auto path() const -> std::string { return path_.string(); }

 private:
  static inline unsigned counter_ = 0;
  std::filesystem::path path_;
};

class ProcessOps final : public diagnostics::LinuxDiagnosticProcessOps {
 public:
  auto run(std::span<const std::string_view> arguments) noexcept
      -> diagnostics::LinuxDiagnosticProcessResult override {
    calls.emplace_back(arguments.front());
    diagnostics::LinuxDiagnosticProcessResult result;
    result.exit_code = 0;
    const std::string text = arguments.front().find("pw-cli") != std::string_view::npos
                                 ? "Sync Camera"
                                 : "ok";
    std::copy(text.begin(), text.end(), result.output.begin());
    result.output_length = text.size();
    return result;
  }
  std::vector<std::string> calls;
};

class CameraOps final : public camera::LinuxCameraDeviceOps {
 public:
  auto enumerate(std::span<std::array<char, 64>> output) noexcept
      -> std::size_t override {
    if (!present) return 0;
    constexpr std::string_view path = "/dev/video7";
    std::copy(path.begin(), path.end(), output[0].begin());
    return 1;
  }
  auto open_no_follow(std::string_view) noexcept -> int override { return 7; }
  auto validate_character_device(int) noexcept -> bool override { return true; }
  auto query_capabilities(int, v4l2_capability& output) noexcept
      -> int override {
    output = {};
    std::copy_n("v4l2 loopback", 13, output.driver);
    std::copy_n("Sync Camera", 11, output.card);
    output.capabilities =
        (live ? V4L2_CAP_VIDEO_CAPTURE : V4L2_CAP_VIDEO_OUTPUT) |
        V4L2_CAP_READWRITE;
    return 0;
  }
  auto set_nv12_format(int, v4l2_format& format) noexcept -> int override {
    ++format_calls;
    format.fmt.pix.width = 1920;
    format.fmt.pix.height = 1080;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    format.fmt.pix.bytesperline = 1920;
    format.fmt.pix.sizeimage = 1920 * 1080 * 3 / 2;
    return 0;
  }
  auto set_frame_rate(int, v4l2_streamparm&) noexcept -> int override {
    return 0;
  }
  auto write_frame(int, std::span<const std::byte>) noexcept
      -> std::pair<std::ptrdiff_t, std::int32_t> override {
    return {-1, ENOSYS};
  }
  void close_descriptor(int) noexcept override { ++closes; }
  bool present = true;
  bool live = false;
  unsigned closes = 0;
  unsigned format_calls = 0;
};

auto finding(const diagnostics::LinuxDoctorReport& report,
             std::string_view code)
    -> const diagnostics::DiagnosticFinding* {
  for (std::size_t index = 0; index < report.count; ++index) {
    if (std::string_view(report.findings[index].code.data()) == code) {
      return &report.findings[index];
    }
  }
  return nullptr;
}

}  // namespace

SYNC_TEST(linux_doctor_reports_platform_camera_and_optional_desktop_state) {
  TempRoot root;
  ProcessOps processes;
  CameraOps camera;
  const auto report = diagnostics::run_linux_doctor({
      .filesystem_root = root.path(),
      .runtime_directory = "/tmp/absent-sync-runtime",
      .camera_operations = &camera,
      .process_operations = &processes,
  });
  SYNC_REQUIRE(report.count >= 6);
  SYNC_REQUIRE(finding(report, "ubuntu_24_04") != nullptr);
  SYNC_REQUIRE(finding(report, "ubuntu_24_04")->ok);
  SYNC_REQUIRE(finding(report, "v4l2loopback_module")->ok);
  SYNC_REQUIRE(finding(report, "camera_device")->ok);
  SYNC_REQUIRE(finding(report, "pipewire") != nullptr);
  SYNC_REQUIRE(finding(report, "pipewire")->severity ==
               diagnostics::DiagnosticSeverity::Optional);
  SYNC_REQUIRE(camera.closes == 1);
}

SYNC_TEST(linux_doctor_camera_absence_is_required_but_ndi_is_optional) {
  TempRoot root;
  ProcessOps processes;
  CameraOps camera;
  camera.present = false;
  const auto report = diagnostics::run_linux_doctor({
      .filesystem_root = root.path(),
      .camera_operations = &camera,
      .process_operations = &processes,
  });
  const auto* device = finding(report, "camera_device");
  const auto* ndi = finding(report, "ndi_runtime");
  SYNC_REQUIRE(device != nullptr && !device->ok);
  SYNC_REQUIRE(device->severity == diagnostics::DiagnosticSeverity::Required);
  SYNC_REQUIRE(ndi != nullptr);
  SYNC_REQUIRE(ndi->severity == diagnostics::DiagnosticSeverity::Optional);
  SYNC_REQUIRE(!report.required_ready);
}

SYNC_TEST(linux_doctor_accepts_the_exact_camera_while_the_daemon_owns_output) {
  TempRoot root;
  ProcessOps processes;
  CameraOps camera;
  camera.live = true;
  const auto report = diagnostics::run_linux_doctor({
      .filesystem_root = root.path(),
      .camera_operations = &camera,
      .process_operations = &processes,
  });
  const auto* device = finding(report, "camera_device");
  SYNC_REQUIRE(device != nullptr && device->ok);
  SYNC_REQUIRE(camera.format_calls == 0);
}

SYNC_TEST(linux_doctor_renderers_redact_token_shaped_values) {
  diagnostics::LinuxDoctorReport report;
  report.count = 1;
  report.findings[0].severity = diagnostics::DiagnosticSeverity::Info;
  report.findings[0].ok = false;
  std::copy_n("journal", 7, report.findings[0].code.begin());
  const std::string token(64, 'a');
  const std::string detail = "failure token=" + token;
  std::copy(detail.begin(), detail.end(), report.findings[0].detail.begin());
  const std::string json = diagnostics::render_linux_doctor_json(report);
  const std::string human = diagnostics::render_linux_doctor_human(report);
  SYNC_REQUIRE(json.find(token) == std::string::npos);
  SYNC_REQUIRE(human.find(token) == std::string::npos);
  SYNC_REQUIRE(json.find("[redacted]") != std::string::npos);
  SYNC_REQUIRE(human.find("[redacted]") != std::string::npos);
}
