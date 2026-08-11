#include "cli.hpp"

#include <sync/pairing.hpp>
#include <sync/pairing_store.hpp>
#include <sync/server.hpp>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <sync/platform/metal_frame_consumer.hpp>
#include <sync/platform/metal_frame_publisher.hpp>
#include <sync/platform/pairing_prompt.hpp>
#include <sync/platform/syphon_consumer.hpp>
#include <sync/publisher_hub.hpp>
#endif

#include <array>
#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

#if defined(__APPLE__)
void pump_macos_events(void *) noexcept {
  @autoreleasepool {
    constexpr std::size_t kMaximumSourcesPerPump = 16;
    for (std::size_t index = 0; index < kMaximumSourcesPerPump; ++index) {
      if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) !=
          kCFRunLoopRunHandledSource) {
        break;
      }
    }
  }
}

int run_syphon(noisefactor::sync::ServerOptions& options,
               std::string_view framework_path) {
  noisefactor::sync::SyphonMetalConsumer syphon({
      .framework_path = framework_path,
  });
  const std::array<noisefactor::sync::MetalFrameConsumer*, 1> consumers{
      {&syphon}};
  noisefactor::sync::MetalFramePublisher metal(consumers);
  const std::array<noisefactor::sync::FramePublisher*, 1> publishers{{&metal}};
  noisefactor::sync::PublisherHub hub(publishers);
  options.providers[0] = {
      .id = "syphon",
      .direction = noisefactor::sync::ProviderDirection::Send,
      .available = syphon.available() && metal.available(),
      .selected = true,
  };
  options.provider_count = 1;
  options.platform_event_pump = pump_macos_events;
  return noisefactor::sync::run_server(options, &hub);
}
#endif

}  // namespace

int main(int argc, char** argv) {
  try {
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
    const noisefactor::sync::cli::ParseResult parsed =
        noisefactor::sync::cli::parse(arguments);
    if (!parsed.ok()) {
      noisefactor::sync::cli::print_usage(std::cerr);
      return noisefactor::sync::cli::kUsageExit;
    }
    const noisefactor::sync::cli::Options& command = parsed.options;
    if (command.mode == noisefactor::sync::cli::Mode::ListPairings ||
        command.mode == noisefactor::sync::cli::Mode::RevokeOrigin) {
      return noisefactor::sync::cli::run_management(command, std::cout,
                                                     std::cerr);
    }

    noisefactor::sync::ServerOptions options;
    options.port = command.port;
    if (command.mode == noisefactor::sync::cli::Mode::StaticTest) {
      options.allowed_origin = command.allowed_origin;
      options.test_token = command.test_token;
      options.test_receiver = command.test_receiver;
      if (options.test_receiver) {
        options.providers[0] = {
            .id = "test",
            .direction = noisefactor::sync::ProviderDirection::Send,
            .available = true,
            .selected = true,
        };
        options.provider_count = 1;
        return noisefactor::sync::run_server(options);
      }

#if defined(__APPLE__)
      return run_syphon(options, command.syphon_framework_path);
#else
      noisefactor::sync::cli::print_usage(std::cerr);
      return noisefactor::sync::cli::kUsageExit;
#endif
    }

#if defined(__APPLE__)
    std::array<char, noisefactor::sync::kMaximumPairingStorePathBytes>
        store_path{};
    std::size_t store_path_length = 0;
    if (noisefactor::sync::default_pairing_store_path(
            store_path, store_path_length) !=
        noisefactor::sync::PairingStoreError::None) {
      std::cerr << "syncd: default pairing store path is unavailable\n";
      return noisefactor::sync::cli::kFailureExit;
    }
    noisefactor::sync::PairingStore store;
    if (store.open({.path = {store_path.data(), store_path_length}}) !=
        noisefactor::sync::PairingStoreError::None) {
      std::cerr << "syncd: failed to open the pairing store\n";
      return noisefactor::sync::cli::kFailureExit;
    }
    noisefactor::sync::pairing::StorePairingAuthority authority(store);
    noisefactor::sync::platform::MacPairingPrompt prompt;
    options.pairing_authority = &authority;
    options.pairing_prompt = &prompt;
    return run_syphon(options, command.syphon_framework_path);
#else
    noisefactor::sync::cli::print_usage(std::cerr);
    return noisefactor::sync::cli::kUsageExit;
#endif
  } catch (const std::exception& error) {
    std::cerr << "syncd: fatal error: " << error.what() << '\n';
    return noisefactor::sync::cli::kFailureExit;
  }
}
