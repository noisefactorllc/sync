#include "cli.hpp"

#include <sync/pairing.hpp>
#include <sync/pairing_store.hpp>
#include <sync/publisher_hub.hpp>
#include <sync/server.hpp>

#include <sync/platform/ndi_publisher.hpp>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <sync/platform/camera_publisher.hpp>
#include <sync/platform/cmio_camera_sink.hpp>
#include <sync/platform/metal_frame_consumer.hpp>
#include <sync/platform/metal_frame_publisher.hpp>
#include <sync/platform/pairing_prompt.hpp>
#include <sync/platform/syphon_consumer.hpp>
#endif

#if defined(_WIN32)
#include <sync/platform/camera_publisher.hpp>
#include <sync/platform/camera_registration.hpp>
#include <sync/platform/mf_camera_sink.hpp>
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
//
// NOT named `sync`: this file is compiled as Objective-C++ on macOS, where
// Foundation pulls in <unistd.h> and its POSIX sync() function, and a
// namespace alias of the same name is a redefinition of a different kind of
// symbol that fails the whole translation unit.
namespace nfsync = noisefactor::sync;

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
[[nodiscard]] bool configured(const nfsync::cli::Options &command,
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
  //
  // `reason` explains an unavailable provider in one short, non-secret phrase.
  // Reporting only `available:false` left an operator with nothing to pull on:
  // the control protocol said no, the log said nothing, and every cause --
  // absent runtime, unloadable runtime, runtime present but wrong -- looked
  // the same from outside.
  void offer(std::string_view id, bool configured, bool available,
             nfsync::FramePublisher *publisher, const char *reason) noexcept {
    if (!configured || capability_count_ >= nfsync::kMaximumProviderCapabilities) {
      return;
    }
    if (!available) {
      // stderr, never stdout: the ready record on stdout is a machine-read
      // protocol and must keep its exact shape.
      std::cerr << "syncd: provider \"" << id << "\" is selected but unavailable: "
                << (reason != nullptr ? reason : "no diagnosis was recorded") << '\n';
    }
    capabilities_[capability_count_++] = {
        .id = std::string(id),
        .direction = nfsync::ProviderDirection::Send,
        .available = available,
        .selected = true,
    };
    if (available && publisher != nullptr && publisher_count_ < publishers_.size()) {
      publishers_[publisher_count_++] = publisher;
    }
  }

  void apply(nfsync::ServerOptions &options) const noexcept {
    for (std::size_t index = 0; index < capability_count_; ++index) {
      options.providers[index] = capabilities_[index];
    }
    options.provider_count = capability_count_;
  }

  [[nodiscard]] std::span<nfsync::FramePublisher *const> publishers()
      const noexcept {
    return {publishers_.data(), publisher_count_};
  }

private:
  std::array<nfsync::ProviderCapability, nfsync::kMaximumProviderCapabilities>
      capabilities_{};
  std::size_t capability_count_ = 0;
  std::array<nfsync::FramePublisher *, nfsync::PublisherHub::kMaximumProviders>
      publishers_{};
  std::size_t publisher_count_ = 0;
};

// Providers are stack-owned here because they must outlive run_server and must
// be torn down in reverse construction order when it returns.
int run_with_providers(nfsync::ServerOptions &options,
                       const nfsync::cli::Options &command) {
#if defined(__APPLE__)
  nfsync::SyphonMetalConsumer syphon({
      .framework_path = command.syphon_framework_path,
  });
  const std::array<nfsync::MetalFrameConsumer *, 1> consumers{{&syphon}};
  nfsync::MetalFramePublisher metal(consumers);
  nfsync::camera::CmioCameraSink camera_sink;
  nfsync::camera::CameraFramePublisher camera(camera_sink);
#endif
#if defined(_WIN32)
  nfsync::SpoutFramePublisher spout({
      .library_path = command.spout_library_path,
  });
  nfsync::camera::MfCameraSink camera_sink;
  nfsync::camera::CameraFramePublisher camera(camera_sink);
#endif
  nfsync::NdiFramePublisher ndi({
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
  nfsync::FramePublisher *const syphon_publisher = &metal;
  // Syphon rides on the Metal publisher, so an unavailable Syphon can be
  // either half. Blaming the framework for a missing Metal device would send
  // an operator to reinstall something that was never at fault.
  const char *const syphon_reason =
      syphon.available() ? "no usable Metal device was found"
                         : nfsync::describe(syphon.unavailable_reason());
#else
  constexpr bool kSyphonImplemented = false;
  constexpr bool syphon_available = false;
  nfsync::FramePublisher *const syphon_publisher = nullptr;
  const char *const syphon_reason = "this build does not implement syphon on this platform";
#endif
#if defined(_WIN32)
  constexpr bool kSpoutImplemented = true;
  const bool spout_available = spout.available();
  nfsync::FramePublisher *const spout_publisher = &spout;
  // Hedged on purpose: SpoutFramePublisher::available() also covers a rejected
  // library path and a GL context that would not initialize, and each of those
  // has a different remedy. Naming only the load would send an operator to
  // reinstall something that was never at fault.
  const char *const spout_reason =
      "the Spout runtime did not load, or failed to initialize";
#else
  constexpr bool kSpoutImplemented = false;
  constexpr bool spout_available = false;
  nfsync::FramePublisher *const spout_publisher = nullptr;
  const char *const spout_reason = "this build does not implement spout on this platform";
#endif
// The two implementations differ entirely underneath -- a CoreMediaIO
// extension against a Media Foundation virtual camera -- but they meet at
// CameraFramePublisher, so the provider looks the same from here.
#if defined(__APPLE__) || defined(_WIN32)
  constexpr bool kCameraImplemented = true;
  const bool camera_available = camera.available();
  nfsync::FramePublisher *const camera_publisher = &camera;
  const std::string camera_reason_text = nfsync::camera::describe_unavailability(
      camera.unavailable_reason(), camera.unavailable_status());
  const char *const camera_reason = camera_reason_text.c_str();
#else
  constexpr bool kCameraImplemented = false;
  constexpr bool camera_available = false;
  nfsync::FramePublisher *const camera_publisher = nullptr;
  const char *const camera_reason = "this build does not implement camera on this platform";
#endif

  ProviderAssembly assembly;
  assembly.offer("syphon", configured(command, "syphon", kSyphonImplemented),
                 syphon_available, syphon_publisher, syphon_reason);
  assembly.offer("spout", configured(command, "spout", kSpoutImplemented),
                 spout_available, spout_publisher, spout_reason);
  assembly.offer("ndi", configured(command, "ndi", true), ndi.available(), &ndi,
                 "the NDI runtime did not load, or failed to initialize");
  assembly.offer("camera", configured(command, "camera", kCameraImplemented),
                 camera_available, camera_publisher, camera_reason);

  assembly.apply(options);
#if defined(__APPLE__)
  options.platform_event_pump = pump_macos_events;
#elif defined(_WIN32)
  options.platform_event_pump = pump_windows_events;
#endif

  nfsync::PublisherHub hub(assembly.publishers());
  return nfsync::run_server(options, &hub);
}

#if defined(__APPLE__) || defined(_WIN32)
// A short, non-secret phrase for each way opening the store can fail. It never
// names a token or a hash -- only which class of problem occurred, so the
// message can be printed to stderr and pasted into a bug report safely.
const char *describe(nfsync::PairingStoreError error) noexcept {
  switch (error) {
    case nfsync::PairingStoreError::None:
      return "no error";
    case nfsync::PairingStoreError::InvalidPath:
      return "the store path is not usable";
    case nfsync::PairingStoreError::DirectorySecurity:
      return "the containing directory is not owner-only, or is a link";
    case nfsync::PairingStoreError::FileSecurity:
      return "the store or lock file is not owner-only, or is a link";
    case nfsync::PairingStoreError::Io:
      return "the store could not be read or written";
    case nfsync::PairingStoreError::Corrupt:
      return "the store file is damaged";
    case nfsync::PairingStoreError::UnknownVersion:
      return "the store was written by a newer version of Sync";
    case nfsync::PairingStoreError::Capacity:
      return "the store is full";
    case nfsync::PairingStoreError::RandomFailure:
      return "secure random numbers were unavailable";
    case nfsync::PairingStoreError::InvalidToken:
      return "a stored record is invalid";
    case nfsync::PairingStoreError::Busy:
      return "another Sync instance is using the store";
    case nfsync::PairingStoreError::Canceled:
      return "the operation was canceled";
  }
  return "unrecognized error";
}

// Production mode issues and stores real credentials, so it needs both a
// durable pairing store and a native prompt a person can answer. A platform
// without both cannot honestly offer it.
int run_production(nfsync::ServerOptions &options,
                   const nfsync::cli::Options &command) {
  std::array<char, nfsync::kMaximumPairingStorePathBytes> store_path{};
  std::size_t store_path_length = 0;
  if (nfsync::default_pairing_store_path(store_path, store_path_length) !=
      nfsync::PairingStoreError::None) {
    std::cerr << "syncd: default pairing store path is unavailable\n";
    return nfsync::cli::kFailureExit;
  }
  nfsync::PairingStore store;
  const nfsync::PairingStoreError opened =
      store.open({.path = {store_path.data(), store_path_length}});
  if (opened != nfsync::PairingStoreError::None) {
    // The bare message this used to print gave a person nothing to act on,
    // and gave a diagnostic log even less: a store that cannot be opened is
    // the difference between "another instance holds it", "someone widened
    // the permissions on it", and "the file is damaged", and every one of
    // those has a different remedy. Naming the reason and the path costs one
    // line and is what makes the failure actionable.
    std::cerr << "syncd: failed to open the pairing store: "
              << describe(opened) << " ("
              << std::string_view(store_path.data(), store_path_length)
              << ")\n";
    // The path is deliberately included even though it carries the account
    // name, because it is the actionable half: without it the two messages
    // below name a folder the reader cannot find. It is the user's own path
    // on the user's own machine, and syncd's stderr is surfaced only in the
    // diagnostics the user chooses to copy.
    if (opened == nfsync::PairingStoreError::DirectorySecurity ||
        opened == nfsync::PairingStoreError::FileSecurity) {
      // Sync will not use a store it does not exclusively own, and it will
      // not silently take ownership of one it finds -- so a store left behind
      // by something else is refused every time, forever, with no way out
      // that the product itself offers. Saying how to clear it is the
      // difference between a fixable problem and a dead install.
      std::cerr << "syncd: Sync only uses a pairing store it exclusively "
                   "owns. Correct that folder's ownership and permissions, "
                   "or delete it to start over -- paired browsers will "
                   "simply ask to pair again.\n";
    }
    return nfsync::cli::kFailureExit;
  }
  nfsync::pairing::StorePairingAuthority authority(store);
#if defined(__APPLE__)
  nfsync::platform::MacPairingPrompt prompt;
#else
  nfsync::platform::WindowsPairingPrompt prompt;
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
    const nfsync::cli::ParseResult parsed = nfsync::cli::parse(arguments);
    if (!parsed.ok()) {
      nfsync::cli::print_usage(std::cerr);
      return nfsync::cli::kUsageExit;
    }
    const nfsync::cli::Options& command = parsed.options;
    if (command.mode == nfsync::cli::Mode::ListPairings ||
        command.mode == nfsync::cli::Mode::RevokeOrigin) {
      return nfsync::cli::run_management(command, std::cout, std::cerr);
    }
#if defined(_WIN32)
    // Both run elevated and do nothing else: the tray app raises a UAC prompt
    // for the first, the uninstaller runs the second.
    if (command.mode == nfsync::cli::Mode::RegisterCamera) {
      return nfsync::camera::register_camera_source();
    }
    if (command.mode == nfsync::cli::Mode::UnregisterCamera) {
      return nfsync::camera::unregister_camera_source();
    }
#else
    // The parser accepts these everywhere so one set of CLI tests covers every
    // platform, the same reason it accepts a publisher this platform cannot
    // provide. A build with no camera to register says so rather than
    // pretending to succeed.
    if (command.mode == nfsync::cli::Mode::RegisterCamera ||
        command.mode == nfsync::cli::Mode::UnregisterCamera) {
      std::cerr << "syncd: this build does not implement camera registration on this platform\n";
      return nfsync::cli::kFailureExit;
    }
#endif

    nfsync::ServerOptions options;
    options.port = command.port;
    if (command.mode == nfsync::cli::Mode::StaticTest) {
      options.allowed_origin = command.allowed_origin;
      options.test_token = command.test_token;
      options.test_receiver = command.test_receiver;
      if (options.test_receiver) {
        options.providers[0] = {
            .id = "test",
            .direction = nfsync::ProviderDirection::Send,
            .available = true,
            .selected = true,
        };
        options.provider_count = 1;
        return nfsync::run_server(options);
      }
      // Static test mode authenticates against a supplied token instead of the
      // pairing store, so it needs no prompt and runs wherever a provider does.
      return run_with_providers(options, command);
    }

#if defined(__APPLE__) || defined(_WIN32)
    return run_production(options, command);
#else
    std::cerr << "syncd: production mode requires macOS or Windows\n";
    nfsync::cli::print_usage(std::cerr);
    return nfsync::cli::kUsageExit;
#endif
  } catch (const std::exception& error) {
    std::cerr << "syncd: fatal error: " << error.what() << '\n';
    return nfsync::cli::kFailureExit;
  }
}
