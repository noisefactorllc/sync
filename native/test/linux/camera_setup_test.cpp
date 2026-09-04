#include "../test_harness.hpp"

#include <sync/platform/linux_camera_setup.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace setup = noisefactor::sync::linux_camera_setup;
namespace camera = noisefactor::sync::camera;

class Operations final : public setup::LinuxCameraSetupOps {
 public:
  auto effective_uid() const noexcept -> std::uint32_t override { return uid; }
  auto resolve_user(std::string_view name,
                    setup::LinuxAccount& output) noexcept -> bool override {
    ++resolve_calls;
    if (!user_exists) return false;
    std::copy(name.begin(), name.end(), output.name.begin());
    output.uid = user_uid;
    output.gid = 1000;
    std::copy_n("/home/artist", 12, output.home.begin());
    return true;
  }
  auto inspect_state() noexcept -> setup::LinuxCameraMachineState override {
    ++inspect_calls;
    return state;
  }
  auto ensure_group_and_member(const setup::LinuxAccount&) noexcept
      -> bool override {
    calls.emplace_back("group");
    return !fail_at("group");
  }
  auto managed_file_matches(std::string_view destination) noexcept
      -> bool override {
    return std::ranges::find(preexisting, std::string(destination)) !=
           preexisting.end();
  }
  auto install_managed_file(std::string_view, std::string_view destination) noexcept
      -> bool override {
    calls.emplace_back("install:" + std::string(destination));
    return !fail_at("install");
  }
  void remove_managed_file(std::string_view destination) noexcept override {
    removed.emplace_back(destination);
  }
  auto run_fixed(std::span<const std::string_view> arguments) noexcept
      -> int override {
    calls.emplace_back(std::string(arguments.front()));
    return fail_at("run") ? 1 : 0;
  }
  auto discover_camera() noexcept -> camera::LinuxCameraOpenResult override {
    calls.emplace_back("discover");
    return discovered;
  }

  bool fail_at(std::string_view kind) {
    ++mutation_index;
    return failure_index != 0 && mutation_index == failure_index &&
           (failure_kind.empty() || failure_kind == kind);
  }

  std::uint32_t uid = 0;
  bool user_exists = true;
  std::uint32_t user_uid = 1000;
  setup::LinuxCameraMachineState state{};
  camera::LinuxCameraOpenResult discovered{
      .error = camera::LinuxCameraDeviceError::None, .descriptor = 7};
  unsigned resolve_calls = 0;
  unsigned inspect_calls = 0;
  unsigned mutation_index = 0;
  unsigned failure_index = 0;
  std::string failure_kind;
  std::vector<std::string> calls;
  std::vector<std::string> removed;
  std::vector<std::string> preexisting;
};

setup::LinuxCameraSetupOptions options() {
  return {.user_name = "artist",
          .template_directory = "/templates",
          .etc_root = "/fixture/etc"};
}

}  // namespace

SYNC_TEST(linux_camera_setup_refuses_unsafe_states_before_mutation) {
  Operations nonroot;
  nonroot.uid = 1000;
  SYNC_REQUIRE(setup::setup_linux_camera(options(), nonroot) ==
               setup::LinuxCameraSetupResult::MustRunAsRoot);
  SYNC_REQUIRE(nonroot.calls.empty());
  SYNC_REQUIRE(nonroot.resolve_calls == 0);

  Operations missing_user;
  missing_user.user_exists = false;
  SYNC_REQUIRE(setup::setup_linux_camera(options(), missing_user) ==
               setup::LinuxCameraSetupResult::InvalidUser);
  SYNC_REQUIRE(missing_user.calls.empty());

  Operations root_user;
  root_user.user_uid = 0;
  SYNC_REQUIRE(setup::setup_linux_camera(options(), root_user) ==
               setup::LinuxCameraSetupResult::InvalidUser);
  SYNC_REQUIRE(root_user.calls.empty());

  for (const setup::LinuxCameraMachineState state : {
           setup::LinuxCameraMachineState{.module_loaded = true},
           setup::LinuxCameraMachineState{.has_any_loopback_device = true},
           setup::LinuxCameraMachineState{.has_modprobe_options = true},
       }) {
    Operations conflict;
    conflict.state = state;
    SYNC_REQUIRE(setup::setup_linux_camera(options(), conflict) ==
                 setup::LinuxCameraSetupResult::Conflict);
    SYNC_REQUIRE(conflict.calls.empty());
  }
}

SYNC_TEST(linux_camera_setup_adopts_compatible_device_without_module_changes) {
  Operations operations;
  operations.state = {.module_loaded = true,
                      .has_any_loopback_device = true,
                      .has_compatible_sync_camera = true,
                      .has_modprobe_options = true};
  SYNC_REQUIRE(setup::setup_linux_camera(options(), operations) ==
               setup::LinuxCameraSetupResult::Adopted);
  SYNC_REQUIRE(operations.calls.size() == 3);
  SYNC_REQUIRE(operations.calls[0] == "group");
  SYNC_REQUIRE(operations.calls[1] == "/usr/bin/udevadm");
  SYNC_REQUIRE(operations.calls[2] == "discover");
}

SYNC_TEST(linux_camera_setup_clean_machine_uses_only_fixed_operations) {
  Operations operations;
  SYNC_REQUIRE(setup::setup_linux_camera(options(), operations) ==
               setup::LinuxCameraSetupResult::NeedsNewLogin);
  SYNC_REQUIRE(operations.calls.size() == 8);
  SYNC_REQUIRE(operations.calls[0] == "group");
  SYNC_REQUIRE(operations.calls[1].starts_with("install:"));
  SYNC_REQUIRE(operations.calls[2].starts_with("install:"));
  SYNC_REQUIRE(operations.calls[3].starts_with("install:"));
  SYNC_REQUIRE(operations.calls[4] == "/usr/bin/udevadm");
  SYNC_REQUIRE(operations.calls[5] == "/usr/sbin/modprobe");
  SYNC_REQUIRE(operations.calls[6] == "/usr/bin/udevadm");
  SYNC_REQUIRE(operations.calls[7] == "discover");
}

SYNC_TEST(linux_camera_setup_failure_removes_only_new_managed_files) {
  Operations operations;
  operations.failure_index = 6;
  SYNC_REQUIRE(setup::setup_linux_camera(options(), operations) ==
               setup::LinuxCameraSetupResult::ModuleLoadFailed);
  SYNC_REQUIRE(operations.removed.size() == 3);

  Operations preexisting;
  preexisting.preexisting = {
      "/fixture/etc/udev/rules.d/70-noisedeck-sync-camera.rules"};
  preexisting.failure_index = 6;
  SYNC_REQUIRE(setup::setup_linux_camera(options(), preexisting) ==
               setup::LinuxCameraSetupResult::ModuleLoadFailed);
  SYNC_REQUIRE(std::ranges::find(preexisting.removed,
                                preexisting.preexisting.front()) ==
               preexisting.removed.end());
}
