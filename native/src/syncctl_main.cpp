#include "syncctl_cli.hpp"

#include <sync/platform/linux_diagnostics.hpp>
#include <sync/platform/linux_camera_setup.hpp>

#include <exception>
#include <iostream>
#include <cstdlib>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
  namespace syncctl = noisefactor::sync::syncctl;
  try {
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) {
      arguments.emplace_back(argv[index]);
    }
    const auto parsed = syncctl::parse(arguments);
    if (!parsed.ok()) {
      syncctl::print_usage(std::cerr);
      return syncctl::kUsageExit;
    }
    if (parsed.options.command == syncctl::Command::Doctor) {
      const char* runtime = std::getenv("XDG_RUNTIME_DIR");
      const auto report = noisefactor::sync::linux_diagnostics::run_linux_doctor({
          .runtime_directory = runtime != nullptr ? runtime : "",
      });
      std::cout << (parsed.options.json
                        ? noisefactor::sync::linux_diagnostics::render_linux_doctor_json(report)
                        : noisefactor::sync::linux_diagnostics::render_linux_doctor_human(report))
                << '\n';
      return report.required_ready ? syncctl::kSuccessExit
                                   : syncctl::kFailureExit;
    }
    if (parsed.options.command == syncctl::Command::CameraSetup) {
      namespace camera_setup = noisefactor::sync::linux_camera_setup;
      const auto result = camera_setup::setup_linux_camera(
          {.user_name = parsed.options.user},
          camera_setup::default_linux_camera_setup_ops());
      const bool success = result == camera_setup::LinuxCameraSetupResult::Configured ||
                           result == camera_setup::LinuxCameraSetupResult::Adopted ||
                           result == camera_setup::LinuxCameraSetupResult::NeedsNewLogin;
      (success ? std::cout : std::cerr) << "syncctl: "
                                        << camera_setup::describe(result) << '\n';
      return success ? syncctl::kSuccessExit : syncctl::kFailureExit;
    }
    return syncctl::execute(parsed.options, std::cin, std::cout, std::cerr);
  } catch (const std::exception& exception) {
    std::cerr << "syncctl: fatal error: " << exception.what() << '\n';
    return syncctl::kFailureExit;
  }
}
