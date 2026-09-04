#include <sync/platform/linux_camera_setup.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <grp.h>
#include <pwd.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace noisefactor::sync::linux_camera_setup {
namespace {

std::string join(std::string_view base, std::string_view suffix) {
  std::string result(base);
  if (result.ends_with('/')) result.pop_back();
  result.append(suffix);
  return result;
}

bool read_file(std::string_view path, std::string& output) noexcept {
  const std::string owned(path);
  const int descriptor = ::open(owned.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return false;
  output.clear();
  std::array<char, 4096> buffer{};
  while (output.size() <= 16'384) {
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    output.append(buffer.data(), static_cast<std::size_t>(count));
  }
  ::close(descriptor);
  return output.size() <= 16'384;
}

bool write_all(int descriptor, std::string_view bytes) noexcept {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count =
        ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

class SystemSetupOps final : public LinuxCameraSetupOps {
 public:
  auto effective_uid() const noexcept -> std::uint32_t override {
    return static_cast<std::uint32_t>(::geteuid());
  }

  auto resolve_user(std::string_view name,
                    LinuxAccount& output) noexcept -> bool override {
    if (name.empty() || name.size() >= output.name.size()) return false;
    std::string owned(name);
    passwd record{};
    passwd* found = nullptr;
    std::array<char, 16'384> buffer{};
    if (::getpwnam_r(owned.c_str(), &record, buffer.data(), buffer.size(),
                     &found) != 0 ||
        found == nullptr || record.pw_name == nullptr ||
        record.pw_dir == nullptr || std::string_view(record.pw_name) != name) {
      return false;
    }
    const std::string_view home(record.pw_dir);
    if (home.empty() || home.front() != '/' || home.size() >= output.home.size()) {
      return false;
    }
    std::copy(name.begin(), name.end(), output.name.begin());
    std::copy(home.begin(), home.end(), output.home.begin());
    output.uid = static_cast<std::uint32_t>(record.pw_uid);
    output.gid = static_cast<std::uint32_t>(record.pw_gid);
    return true;
  }

  auto inspect_state() noexcept -> LinuxCameraMachineState override {
    LinuxCameraMachineState state;
    struct stat module_status {};
    state.module_loaded =
        ::lstat("/sys/module/v4l2loopback", &module_status) == 0;

    auto& camera_ops = camera::default_linux_camera_device_ops();
    std::array<std::array<char, 64>, 64> paths{};
    const std::size_t count = camera_ops.enumerate(paths);
    for (std::size_t index = 0; index < std::min(count, paths.size()); ++index) {
      const auto end = std::find(paths[index].begin(), paths[index].end(), '\0');
      const std::string_view path(paths[index].data(),
                                  static_cast<std::size_t>(end - paths[index].begin()));
      const int descriptor = camera_ops.open_no_follow(path);
      if (descriptor < 0) continue;
      v4l2_capability capabilities{};
      if (camera_ops.query_capabilities(descriptor, capabilities) == 0) {
        const std::string_view driver(
            reinterpret_cast<const char*>(capabilities.driver),
            strnlen(reinterpret_cast<const char*>(capabilities.driver),
                    sizeof(capabilities.driver)));
        if (driver == "v4l2 loopback") state.has_any_loopback_device = true;
      }
      camera_ops.close_descriptor(descriptor);
    }
    const auto compatible = camera::open_linux_camera({}, camera_ops);
    if (compatible.error == camera::LinuxCameraDeviceError::None) {
      state.has_compatible_sync_camera = true;
      camera_ops.close_descriptor(compatible.descriptor);
    }

    const std::array<std::string_view, 2> config_paths{
        "/etc/modprobe.d/noisedeck-sync-camera.conf",
        "/etc/modprobe.d/v4l2loopback.conf"};
    for (const auto path : config_paths) {
      std::string bytes;
      if (read_file(path, bytes) &&
          bytes.find("options v4l2loopback") != std::string::npos) {
        state.has_modprobe_options = true;
      }
    }
    return state;
  }

  auto ensure_group_and_member(const LinuxAccount& account) noexcept
      -> bool override {
    group group_record{};
    group* found = nullptr;
    std::array<char, 16'384> buffer{};
    if (::getgrnam_r("noisedeck-sync", &group_record, buffer.data(),
                     buffer.size(), &found) != 0) {
      return false;
    }
    if (found == nullptr) {
      const std::array<std::string_view, 3> add{
          "/usr/sbin/groupadd", "--system", "noisedeck-sync"};
      if (run_fixed(add) != 0) return false;
      found = nullptr;
      if (::getgrnam_r("noisedeck-sync", &group_record, buffer.data(),
                       buffer.size(), &found) != 0 ||
          found == nullptr) {
        return false;
      }
    }
    bool member = account.gid == static_cast<std::uint32_t>(group_record.gr_gid);
    for (char** item = group_record.gr_mem; !member && item != nullptr && *item != nullptr;
         ++item) {
      member = std::string_view(*item) == std::string_view(account.name.data());
    }
    if (member) return true;
    const std::array<std::string_view, 5> add_member{
        "/usr/sbin/usermod", "--append", "--groups", "noisedeck-sync",
        std::string_view(account.name.data())};
    return run_fixed(add_member) == 0;
  }

  auto managed_file_matches(std::string_view destination) noexcept
      -> bool override {
    std::string actual;
    if (!read_file(destination, actual)) return false;
    const auto slash = destination.rfind('/');
    if (slash == std::string_view::npos) return false;
    const std::string_view name = destination.substr(slash + 1);
    const std::string source = join("/usr/share/noisedeck-sync", "/" +
                                    std::string(name));
    std::string expected;
    return read_file(source, expected) && actual == expected;
  }

  auto install_managed_file(std::string_view source,
                            std::string_view destination) noexcept
      -> bool override {
    std::string expected;
    if (!read_file(source, expected)) return false;
    std::string actual;
    if (read_file(destination, actual)) return actual == expected;
    const auto slash = destination.rfind('/');
    if (slash == std::string_view::npos || slash == 0) return false;
    const std::string directory_path(destination.substr(0, slash));
    const std::string name(destination.substr(slash + 1));
    const int directory = ::open(directory_path.c_str(),
                                 O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                     O_NOFOLLOW);
    if (directory < 0) return false;
    const std::string temporary = ".noisedeck-sync-" +
                                  std::to_string(::getpid()) + ".tmp";
    const int file = ::openat(directory, temporary.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                  O_NOFOLLOW,
                              0644);
    bool ok = file >= 0 && write_all(file, expected) && ::fsync(file) == 0;
    if (file >= 0) ::close(file);
    if (ok) ok = ::renameat(directory, temporary.c_str(), directory,
                            name.c_str()) == 0 &&
                 ::fsync(directory) == 0;
    if (!ok) (void)::unlinkat(directory, temporary.c_str(), 0);
    ::close(directory);
    return ok;
  }

  void remove_managed_file(std::string_view destination) noexcept override {
    const auto slash = destination.rfind('/');
    if (slash == std::string_view::npos) return;
    const std::string_view name = destination.substr(slash + 1);
    const std::string source = join("/usr/share/noisedeck-sync", "/" +
                                    std::string(name));
    std::string expected;
    std::string actual;
    if (read_file(source, expected) && read_file(destination, actual) &&
        expected == actual) {
      const std::string owned(destination);
      (void)::unlink(owned.c_str());
    }
  }

  auto run_fixed(std::span<const std::string_view> arguments) noexcept
      -> int override {
    if (arguments.empty() || arguments.size() > 16 ||
        arguments.front().front() != '/') {
      return -1;
    }
    try {
      std::vector<std::string> owned;
      std::vector<char*> argv;
      for (const auto argument : arguments) owned.emplace_back(argument);
      for (auto& argument : owned) argv.push_back(argument.data());
      argv.push_back(nullptr);
      const pid_t child = ::fork();
      if (child == 0) {
        ::execv(argv[0], argv.data());
        ::_exit(127);
      }
      if (child < 0) return -1;
      int status = 0;
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    } catch (...) {
      return -1;
    }
  }

  auto discover_camera() noexcept -> camera::LinuxCameraOpenResult override {
    auto result = camera::open_linux_camera({},
                                            camera::default_linux_camera_device_ops());
    if (result.descriptor >= 0) {
      camera::default_linux_camera_device_ops().close_descriptor(result.descriptor);
      result.descriptor = -1;
    }
    return result;
  }
};

void rollback(LinuxCameraSetupOps& operations,
              std::span<const std::string> created) noexcept {
  for (auto iterator = created.rbegin(); iterator != created.rend(); ++iterator) {
    operations.remove_managed_file(*iterator);
  }
}

}  // namespace

auto default_linux_camera_setup_ops() noexcept -> LinuxCameraSetupOps& {
  static SystemSetupOps operations;
  return operations;
}

auto setup_linux_camera(const LinuxCameraSetupOptions& options,
                        LinuxCameraSetupOps& operations) noexcept
    -> LinuxCameraSetupResult {
  if (operations.effective_uid() != 0) {
    return LinuxCameraSetupResult::MustRunAsRoot;
  }
  LinuxAccount account;
  if (!operations.resolve_user(options.user_name, account) || account.uid == 0 ||
      std::string_view(account.name.data()) != options.user_name ||
      account.home[0] != '/') {
    return LinuxCameraSetupResult::InvalidUser;
  }
  const LinuxCameraMachineState state = operations.inspect_state();
  if (state.has_compatible_sync_camera) {
    if (!operations.ensure_group_and_member(account)) {
      return LinuxCameraSetupResult::Io;
    }
    const std::array<std::string_view, 3> reload{
        "/usr/bin/udevadm", "control", "--reload-rules"};
    if (operations.run_fixed(reload) != 0) return LinuxCameraSetupResult::Io;
    const auto discovered = operations.discover_camera();
    return discovered.error == camera::LinuxCameraDeviceError::None
               ? LinuxCameraSetupResult::Adopted
               : LinuxCameraSetupResult::DeviceValidationFailed;
  }
  if (state.module_loaded || state.has_any_loopback_device ||
      state.has_modprobe_options) {
    return LinuxCameraSetupResult::Conflict;
  }
  if (!operations.ensure_group_and_member(account)) {
    return LinuxCameraSetupResult::Io;
  }
  try {
    const std::array<std::string, 3> sources{
        join(options.template_directory, "/70-noisedeck-sync-camera.rules"),
        join(options.template_directory, "/noisedeck-sync-camera.modprobe"),
        join(options.template_directory, "/noisedeck-sync-camera.modules-load")};
    const std::array<std::string, 3> destinations{
        join(options.etc_root, "/udev/rules.d/70-noisedeck-sync-camera.rules"),
        join(options.etc_root, "/modprobe.d/noisedeck-sync-camera.conf"),
        join(options.etc_root,
             "/modules-load.d/noisedeck-sync-camera.conf")};
    std::array<std::string, 3> created{};
    std::size_t created_count = 0;
    for (std::size_t index = 0; index < sources.size(); ++index) {
      const bool existed = operations.managed_file_matches(destinations[index]);
      if (!operations.install_managed_file(sources[index], destinations[index])) {
        rollback(operations, std::span(created).first(created_count));
        return LinuxCameraSetupResult::Io;
      }
      if (!existed) created[created_count++] = destinations[index];
    }
    const std::array<std::string_view, 3> reload{
        "/usr/bin/udevadm", "control", "--reload-rules"};
    const std::array<std::string_view, 2> load{
        "/usr/sbin/modprobe", "v4l2loopback"};
    const std::array<std::string_view, 2> settle{
        "/usr/bin/udevadm", "settle"};
    if (operations.run_fixed(reload) != 0) {
      rollback(operations, std::span(created).first(created_count));
      return LinuxCameraSetupResult::Io;
    }
    if (operations.run_fixed(load) != 0) {
      rollback(operations, std::span(created).first(created_count));
      return LinuxCameraSetupResult::ModuleLoadFailed;
    }
    if (operations.run_fixed(settle) != 0) {
      rollback(operations, std::span(created).first(created_count));
      return LinuxCameraSetupResult::Io;
    }
    const auto discovered = operations.discover_camera();
    if (discovered.error != camera::LinuxCameraDeviceError::None) {
      // Do not unload a live module. Managed files remain so the next boot
      // can reproduce the intended safe configuration for diagnosis.
      return LinuxCameraSetupResult::DeviceValidationFailed;
    }
    return LinuxCameraSetupResult::NeedsNewLogin;
  } catch (...) {
    return LinuxCameraSetupResult::Io;
  }
}

auto describe(LinuxCameraSetupResult result) noexcept -> const char* {
  switch (result) {
    case LinuxCameraSetupResult::Configured: return "configured";
    case LinuxCameraSetupResult::Adopted: return "adopted";
    case LinuxCameraSetupResult::NeedsNewLogin: return "configured; start a new login session before running syncd";
    case LinuxCameraSetupResult::Conflict: return "existing v4l2loopback configuration must be reviewed; run syncctl doctor";
    case LinuxCameraSetupResult::InvalidUser: return "invalid non-root target user";
    case LinuxCameraSetupResult::MustRunAsRoot: return "camera setup must run as root";
    case LinuxCameraSetupResult::ModuleLoadFailed: return "v4l2loopback could not be loaded";
    case LinuxCameraSetupResult::DeviceValidationFailed: return "Sync Camera was not validated after setup";
    case LinuxCameraSetupResult::Io: return "camera setup I/O failed";
  }
  return "camera setup failed";
}

}  // namespace noisefactor::sync::linux_camera_setup
