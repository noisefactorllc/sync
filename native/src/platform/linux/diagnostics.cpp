#include <sync/platform/linux_diagnostics.hpp>

#include <sync/platform/ndi_publisher.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

namespace noisefactor::sync::linux_diagnostics {
namespace {

template <std::size_t Size>
void copy_text(std::array<char, Size>& output, std::string_view value) noexcept {
  output.fill('\0');
  const std::size_t count = std::min(value.size(), Size - 1);
  std::copy_n(value.data(), count, output.data());
}

std::string_view array_text(const auto& value) noexcept {
  const auto end = std::find(value.begin(), value.end(), '\0');
  return {value.data(), static_cast<std::size_t>(end - value.begin())};
}

std::string rooted(std::string_view root, std::string_view absolute) {
  if (root == "/") return std::string(absolute);
  std::string result(root);
  if (result.ends_with('/')) result.pop_back();
  result.append(absolute);
  return result;
}

bool path_exists_no_follow(std::string_view path) noexcept {
  const std::string owned(path);
  struct stat status {};
  return ::lstat(owned.c_str(), &status) == 0;
}

bool read_bounded(std::string_view path, std::string& output,
                  std::size_t maximum = 16'384) noexcept {
  const std::string owned(path);
  const int descriptor = ::open(owned.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return false;
  output.clear();
  std::array<char, 4096> buffer{};
  while (output.size() < maximum) {
    const std::size_t remaining = maximum - output.size();
    const auto count = ::read(descriptor, buffer.data(),
                              std::min(buffer.size(), remaining));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    output.append(buffer.data(), static_cast<std::size_t>(count));
  }
  char extra = 0;
  const auto beyond = ::read(descriptor, &extra, 1);
  ::close(descriptor);
  return beyond == 0;
}

void add(LinuxDoctorReport& report, std::string_view code,
         DiagnosticSeverity severity, bool ok, std::string_view detail) noexcept {
  if (report.count == report.findings.size()) return;
  DiagnosticFinding& finding = report.findings[report.count++];
  copy_text(finding.code, code);
  finding.severity = severity;
  finding.ok = ok;
  copy_text(finding.detail, detail);
}

const char* device_error(camera::LinuxCameraDeviceError error) noexcept {
  switch (error) {
    case camera::LinuxCameraDeviceError::None: return "ready";
    case camera::LinuxCameraDeviceError::NotFound: return "device not found";
    case camera::LinuxCameraDeviceError::Ambiguous: return "multiple matching devices";
    case camera::LinuxCameraDeviceError::InvalidPath: return "invalid device path";
    case camera::LinuxCameraDeviceError::OpenDenied: return "device permission denied";
    case camera::LinuxCameraDeviceError::NotCharacterDevice: return "not a character device";
    case camera::LinuxCameraDeviceError::WrongCard: return "wrong card label";
    case camera::LinuxCameraDeviceError::WrongDriver: return "wrong kernel driver";
    case camera::LinuxCameraDeviceError::MissingOutputCapability: return "missing output capability";
    case camera::LinuxCameraDeviceError::FormatRejected: return "NV12 format rejected";
    case camera::LinuxCameraDeviceError::InvalidFormatBounds: return "unsafe returned format";
    case camera::LinuxCameraDeviceError::Io: return "device I/O failed";
  }
  return "unknown device error";
}

bool hex_byte(char value) noexcept {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

std::string redact(std::string_view input) {
  std::string output;
  for (std::size_t index = 0; index < input.size();) {
    std::size_t end = index;
    while (end < input.size() && hex_byte(input[end])) ++end;
    if (end - index >= 64) {
      output += "[redacted]";
      index = end;
    } else {
      output.append(input.substr(index, end == index ? 1 : end - index));
      index = end == index ? index + 1 : end;
    }
  }
  return output;
}

std::string json_string(std::string_view input) {
  constexpr char hex[] = "0123456789abcdef";
  std::string output = "\"";
  for (const unsigned char byte : input) {
    if (byte == '"' || byte == '\\') {
      output.push_back('\\');
      output.push_back(static_cast<char>(byte));
    } else if (byte < 0x20U) {
      output += "\\u00";
      output.push_back(hex[(byte >> 4U) & 0xfU]);
      output.push_back(hex[byte & 0xfU]);
    } else {
      output.push_back(static_cast<char>(byte));
    }
  }
  output.push_back('"');
  return output;
}

class SystemProcessOps final : public LinuxDiagnosticProcessOps {
 public:
  auto run(std::span<const std::string_view> arguments) noexcept
      -> LinuxDiagnosticProcessResult override {
    LinuxDiagnosticProcessResult result;
    if (arguments.empty() || arguments.size() > 16 ||
        arguments.front().empty() || arguments.front().front() != '/') {
      return result;
    }
    try {
      std::vector<std::string> owned;
      std::vector<char*> argv;
      owned.reserve(arguments.size());
      argv.reserve(arguments.size() + 1);
      for (const std::string_view argument : arguments) {
        if (argument.size() > 4096 || argument.find('\0') != std::string_view::npos) {
          return result;
        }
        owned.emplace_back(argument);
      }
      for (std::string& argument : owned) argv.push_back(argument.data());
      argv.push_back(nullptr);
      std::array<int, 2> pipe{{-1, -1}};
      if (::pipe2(pipe.data(), O_CLOEXEC) != 0) return result;
      const pid_t child = ::fork();
      if (child == 0) {
        (void)::dup2(pipe[1], STDOUT_FILENO);
        (void)::dup2(pipe[1], STDERR_FILENO);
        ::close(pipe[0]);
        ::close(pipe[1]);
        ::execv(argv[0], argv.data());
        ::_exit(127);
      }
      ::close(pipe[1]);
      if (child < 0) {
        ::close(pipe[0]);
        return result;
      }
      while (result.output_length < result.output.size()) {
        const auto count = ::read(pipe[0],
                                  result.output.data() + result.output_length,
                                  result.output.size() - result.output_length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        result.output_length += static_cast<std::size_t>(count);
      }
      char extra = 0;
      result.truncated = ::read(pipe[0], &extra, 1) > 0;
      ::close(pipe[0]);
      int status = 0;
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    } catch (...) {
      return {};
    }
    return result;
  }
};

}  // namespace

auto default_linux_diagnostic_process_ops() noexcept
    -> LinuxDiagnosticProcessOps& {
  static SystemProcessOps operations;
  return operations;
}

auto run_linux_doctor(const LinuxDoctorOptions& options) noexcept
    -> LinuxDoctorReport {
  LinuxDoctorReport report;
  try {
    std::string os_release;
    const bool os_read =
        read_bounded(rooted(options.filesystem_root, "/etc/os-release"),
                     os_release) ||
        read_bounded(rooted(options.filesystem_root, "/usr/lib/os-release"),
                     os_release);
    const bool ubuntu = os_read && os_release.find("ID=ubuntu") != std::string::npos &&
                        (os_release.find("VERSION_ID=\"24.04\"") !=
                             std::string::npos ||
                         os_release.find("VERSION_ID=24.04") !=
                             std::string::npos);
    add(report, "ubuntu_24_04", DiagnosticSeverity::Required, ubuntu,
        ubuntu ? "Ubuntu 24.04 LTS" : "certified platform is Ubuntu 24.04 LTS");

    utsname system{};
    const bool named = ::uname(&system) == 0;
    const bool architecture = named && std::string_view(system.machine) == "x86_64";
    add(report, "architecture", DiagnosticSeverity::Required, architecture,
        named ? std::string_view(system.machine) : "uname failed");

    const bool module = path_exists_no_follow(
        rooted(options.filesystem_root, "/sys/module/v4l2loopback"));
    add(report, "v4l2loopback_module",
        options.camera_selected ? DiagnosticSeverity::Required
                                : DiagnosticSeverity::Info,
        !options.camera_selected || module,
        module ? "v4l2loopback is loaded" : "v4l2loopback is not loaded");

    camera::LinuxCameraDeviceOps& camera_ops =
        options.camera_operations != nullptr
            ? *options.camera_operations
            : camera::default_linux_camera_device_ops();
    camera::LinuxCameraOpenResult camera_result{};
    if (options.camera_selected) {
      camera_result = camera::probe_linux_camera(options.selected_camera_path,
                                                 camera_ops);
    } else {
      camera_result.error = camera::LinuxCameraDeviceError::None;
    }
    const bool camera_ready =
        camera_result.error == camera::LinuxCameraDeviceError::None;
    if (camera_result.descriptor >= 0) {
      camera_ops.close_descriptor(camera_result.descriptor);
    }
    add(report, "camera_device",
        options.camera_selected ? DiagnosticSeverity::Required
                                : DiagnosticSeverity::Info,
        camera_ready, device_error(camera_result.error));

    LinuxDiagnosticProcessOps& processes =
        options.process_operations != nullptr
            ? *options.process_operations
            : default_linux_diagnostic_process_ops();
    const std::array<std::string_view, 3> pipewire_args{
        "/usr/bin/pw-cli", "ls", "Node"};
    const auto pipewire = processes.run(pipewire_args);
    const std::string_view pipewire_output(pipewire.output.data(),
                                           pipewire.output_length);
    const bool pipewire_ready =
        pipewire.exit_code == 0 &&
        (!options.camera_selected ||
         pipewire_output.find("Sync Camera") != std::string_view::npos);
    add(report, "pipewire", DiagnosticSeverity::Optional, pipewire_ready,
        pipewire_ready ? "PipeWire observes Sync Camera"
                       : "PipeWire is optional or has not observed Sync Camera");

    NdiFramePublisher ndi;
    const bool ndi_ready = !options.ndi_selected || ndi.available();
    add(report, "ndi_runtime", DiagnosticSeverity::Optional, ndi_ready,
        options.ndi_selected ? describe(ndi.unavailable_reason())
                             : "NDI is not selected");

    const bool avahi = path_exists_no_follow(
        rooted(options.filesystem_root, "/run/avahi-daemon/socket"));
    add(report, "avahi", DiagnosticSeverity::Optional, avahi,
        avahi ? "Avahi discovery socket is present"
              : "Avahi is optional and its socket is absent");

    bool daemon = false;
    if (!options.runtime_directory.empty()) {
      const std::string socket = std::string(options.runtime_directory) +
                                 "/noisedeck-sync/control.sock";
      struct stat status {};
      daemon = ::lstat(socket.c_str(), &status) == 0 && S_ISSOCK(status.st_mode);
    }
    add(report, "daemon", DiagnosticSeverity::Info, daemon,
        daemon ? "syncd control socket is present"
               : "syncd control socket is absent");

    report.required_ready = true;
    for (std::size_t index = 0; index < report.count; ++index) {
      if (report.findings[index].severity == DiagnosticSeverity::Required &&
          !report.findings[index].ok) {
        report.required_ready = false;
      }
    }
  } catch (...) {
    add(report, "doctor_internal", DiagnosticSeverity::Required, false,
        "diagnostics could not complete");
    report.required_ready = false;
  }
  return report;
}

auto render_linux_doctor_json(const LinuxDoctorReport& report) -> std::string {
  std::string output = "{\"version\":1,\"requiredReady\":";
  output += report.required_ready ? "true" : "false";
  output += ",\"findings\":[";
  for (std::size_t index = 0; index < report.count; ++index) {
    if (index != 0) output.push_back(',');
    const auto& finding = report.findings[index];
    const std::string detail = redact(array_text(finding.detail));
    const std::string_view severity =
        finding.severity == DiagnosticSeverity::Required
            ? "required"
            : (finding.severity == DiagnosticSeverity::Optional ? "optional"
                                                                 : "info");
    output += "{\"code\":" + json_string(array_text(finding.code)) +
              ",\"severity\":" + json_string(severity) +
              ",\"ok\":" + (finding.ok ? "true" : "false") +
              ",\"detail\":" + json_string(detail) + "}";
  }
  output += "]}";
  return output;
}

auto render_linux_doctor_human(const LinuxDoctorReport& report) -> std::string {
  std::string output;
  for (const DiagnosticSeverity severity : {DiagnosticSeverity::Required,
                                            DiagnosticSeverity::Optional,
                                            DiagnosticSeverity::Info}) {
    output += severity == DiagnosticSeverity::Required
                  ? "Required:\n"
                  : (severity == DiagnosticSeverity::Optional ? "Optional:\n"
                                                               : "Info:\n");
    for (std::size_t index = 0; index < report.count; ++index) {
      const auto& finding = report.findings[index];
      if (finding.severity != severity) continue;
      output += finding.ok ? "  [ok] " : "  [!!] ";
      output += array_text(finding.code);
      output += ": ";
      output += redact(array_text(finding.detail));
      output.push_back('\n');
    }
  }
  return output;
}

}  // namespace noisefactor::sync::linux_diagnostics
