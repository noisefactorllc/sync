#include "cli.hpp"

#include <sync/pairing.hpp>
#include <sync/pairing_store.hpp>
#include <sync/publisher_hub.hpp>
#include <sync/server.hpp>

#include <sync/platform/ndi_publisher.hpp>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <sync/platform/metal_frame_consumer.hpp>
#include <sync/platform/metal_frame_publisher.hpp>
#include <sync/platform/pairing_prompt.hpp>
#include <sync/platform/syphon_consumer.hpp>
#endif

#if defined(_WIN32)
#include <sync/platform/pairing_prompt.hpp>
#include <sync/platform/spout_publisher.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#endif

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Shorthand for a file that names types from this namespace on nearly every
// line; the alias sits outside the anonymous namespace so main() can use it.
namespace sync = noisefactor::sync;

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
#endif

#if defined(_WIN32)
// The Windows pairing prompt owns its own thread, but a daemon process still
// needs to drain its message queue so window messages posted to this thread
// (prompt cancellation, shutdown) are dispatched rather than accumulating.
void pump_windows_events(void *) noexcept {
  constexpr std::size_t kMaximumMessagesPerPump = 16;
  MSG message;
  for (std::size_t index = 0; index < kMaximumMessagesPerPump; ++index) {
    if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) == 0) break;
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}
#endif

// Whether a provider is part of this run at all.
//
// An empty explicit selection means "every provider this platform offers", so
// a plain `syncd` publishes through everything this build implements. An
// explicit selection means exactly what it names -- including a provider this
// platform has no implementation for, which is then advertised as unavailable
// rather than dropped. The CLI grammar is platform-neutral on purpose, so
// `--publisher syphon` on Windows has to start and report syphon unavailable;
// silently offering nothing would leave the daemon with no providers at all
// and fail as a confusing configuration error instead.
[[nodiscard]] bool configured(const sync::cli::Options &command,
                              std::string_view id,
                              bool implemented_here) noexcept {
  return command.publisher_count == 0 ? implemented_here
                                      : command.selects_publisher(id);
}

// Collects the providers this build can offer into the shape run_server wants:
// a capability descriptor per provider for the control protocol, and a hub
// holding only the providers that are actually going to publish.
class ProviderAssembly {
public:
  // The two capability flags answer different questions, and the control
  // protocol has always kept them apart: `selected` says this daemon run is
  // configured to use the provider, `available` says the provider's runtime
  // actually loaded. A client needs both to tell "not offered here" from
  // "offered but the runtime is missing".
  //
  // Only an available provider reaches the hub, because PublisherHub opens a
  // sender across all of its providers as a unit — registering one that cannot
  // publish would fail every sender for the ones that can.
  void offer(std::string_view id, bool configured, bool available,
             sync::FramePublisher *publisher) noexcept {
    if (!configured || capability_count_ >= sync::kMaximumProviderCapabilities) {
      return;
    }
    capabilities_[capability_count_++] = {
        .id = std::string(id),
        .direction = sync::ProviderDirection::Send,
        .available = available,
        .selected = true,
    };
    if (available && publisher != nullptr && publisher_count_ < publishers_.size()) {
      publishers_[publisher_count_++] = publisher;
    }
  }

  void apply(sync::ServerOptions &options) const noexcept {
    for (std::size_t index = 0; index < capability_count_; ++index) {
      options.providers[index] = capabilities_[index];
    }
    options.provider_count = capability_count_;
  }

  [[nodiscard]] std::span<sync::FramePublisher *const> publishers()
      const noexcept {
    return {publishers_.data(), publisher_count_};
  }

private:
  std::array<sync::ProviderCapability, sync::kMaximumProviderCapabilities>
      capabilities_{};
  std::size_t capability_count_ = 0;
  std::array<sync::FramePublisher *, sync::PublisherHub::kMaximumProviders>
      publishers_{};
  std::size_t publisher_count_ = 0;
};

// Providers are stack-owned here because they must outlive run_server and must
// be torn down in reverse construction order when it returns.
int run_with_providers(sync::ServerOptions &options,
                       const sync::cli::Options &command) {
#if defined(__APPLE__)
  sync::SyphonMetalConsumer syphon({
      .framework_path = command.syphon_framework_path,
  });
  const std::array<sync::MetalFrameConsumer *, 1> consumers{{&syphon}};
  sync::MetalFramePublisher metal(consumers);
#endif
#if defined(_WIN32)
  sync::SpoutFramePublisher spout({
      .library_path = command.spout_library_path,
  });
#endif
  sync::NdiFramePublisher ndi({
      .runtime_path = command.ndi_runtime_path,
  });

  // Each provider is offered in a stable order with three facts: whether this
  // build implements it at all, whether it is part of this run, and whether
  // its runtime actually loaded.
#if defined(__APPLE__)
  // Syphon needs both halves: the Metal publisher that owns the shared texture
  // ring and the Syphon consumer that republishes it.
  constexpr bool kSyphonImplemented = true;
  const bool syphon_available = syphon.available() && metal.available();
  sync::FramePublisher *const syphon_publisher = &metal;
#else
  constexpr bool kSyphonImplemented = false;
  constexpr bool syphon_available = false;
  sync::FramePublisher *const syphon_publisher = nullptr;
#endif
#if defined(_WIN32)
  constexpr bool kSpoutImplemented = true;
  const bool spout_available = spout.available();
  sync::FramePublisher *const spout_publisher = &spout;
#else
  constexpr bool kSpoutImplemented = false;
  constexpr bool spout_available = false;
  sync::FramePublisher *const spout_publisher = nullptr;
#endif

  ProviderAssembly assembly;
  assembly.offer("syphon", configured(command, "syphon", kSyphonImplemented),
                 syphon_available, syphon_publisher);
  assembly.offer("spout", configured(command, "spout", kSpoutImplemented),
                 spout_available, spout_publisher);
  assembly.offer("ndi", configured(command, "ndi", true), ndi.available(), &ndi);

  assembly.apply(options);
#if defined(__APPLE__)
  options.platform_event_pump = pump_macos_events;
#elif defined(_WIN32)
  options.platform_event_pump = pump_windows_events;
#endif

  sync::PublisherHub hub(assembly.publishers());
  return sync::run_server(options, &hub);
}

#if defined(__APPLE__) || defined(_WIN32)
// Production mode issues and stores real credentials, so it needs both a
// durable pairing store and a native prompt a person can answer. A platform
// without both cannot honestly offer it.
int run_production(sync::ServerOptions &options,
                   const sync::cli::Options &command) {
  std::array<char, sync::kMaximumPairingStorePathBytes> store_path{};
  std::size_t store_path_length = 0;
  if (sync::default_pairing_store_path(store_path, store_path_length) !=
      sync::PairingStoreError::None) {
    std::cerr << "syncd: default pairing store path is unavailable\n";
    return sync::cli::kFailureExit;
  }
  sync::PairingStore store;
  if (store.open({.path = {store_path.data(), store_path_length}}) !=
      sync::PairingStoreError::None) {
    std::cerr << "syncd: failed to open the pairing store\n";
    return sync::cli::kFailureExit;
  }
  sync::pairing::StorePairingAuthority authority(store);
#if defined(__APPLE__)
  sync::platform::MacPairingPrompt prompt;
#else
  sync::platform::WindowsPairingPrompt prompt;
#endif
  options.pairing_authority = &authority;
  options.pairing_prompt = &prompt;
  return run_with_providers(options, command);
}
#endif

}  // namespace

int main(int argc, char** argv) {
  try {
#if defined(_WIN32)
    // The ready record and the management-command output are a machine-read
    // protocol, not human text. Windows' default text mode would rewrite every
    // '\n' in them to "\r\n", so the exact bytes a caller parses would differ
    // by platform for no reason. Binary mode keeps one wire format everywhere.
    ::_setmode(::_fileno(stdout), _O_BINARY);
    ::_setmode(::_fileno(stderr), _O_BINARY);
#endif
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
    const sync::cli::ParseResult parsed = sync::cli::parse(arguments);
    if (!parsed.ok()) {
      sync::cli::print_usage(std::cerr);
      return sync::cli::kUsageExit;
    }
    const sync::cli::Options& command = parsed.options;
    if (command.mode == sync::cli::Mode::ListPairings ||
        command.mode == sync::cli::Mode::RevokeOrigin) {
      return sync::cli::run_management(command, std::cout, std::cerr);
    }

    sync::ServerOptions options;
    options.port = command.port;
    if (command.mode == sync::cli::Mode::StaticTest) {
      options.allowed_origin = command.allowed_origin;
      options.test_token = command.test_token;
      options.test_receiver = command.test_receiver;
      if (options.test_receiver) {
        options.providers[0] = {
            .id = "test",
            .direction = sync::ProviderDirection::Send,
            .available = true,
            .selected = true,
        };
        options.provider_count = 1;
        return sync::run_server(options);
      }
      // Static test mode authenticates against a supplied token instead of the
      // pairing store, so it needs no prompt and runs wherever a provider does.
      return run_with_providers(options, command);
    }

#if defined(__APPLE__) || defined(_WIN32)
    return run_production(options, command);
#else
    std::cerr << "syncd: production mode requires macOS or Windows\n";
    sync::cli::print_usage(std::cerr);
    return sync::cli::kUsageExit;
#endif
  } catch (const std::exception& error) {
    std::cerr << "syncd: fatal error: " << error.what() << '\n';
    return sync::cli::kFailureExit;
  }
}
