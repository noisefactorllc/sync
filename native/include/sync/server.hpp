#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace noisefactor::sync {

class DaemonMetrics;

namespace pairing {
class PairingAuthority;
class PairingPrompt;
} // namespace pairing

#ifndef SYNC_PRODUCT_VERSION
#define SYNC_PRODUCT_VERSION "0.2.0"
#endif

inline constexpr std::string_view kProductVersion = SYNC_PRODUCT_VERSION;
inline constexpr std::size_t kMaximumProviderCapabilities = 4;
inline constexpr std::size_t kMaximumProviderIdBytes = 32;

enum class ProviderDirection {
  Send,
  Receive,
};

struct ProviderCapability {
  std::string id;
  ProviderDirection direction = ProviderDirection::Send;
  bool available = false;
  bool selected = false;
};

struct ServerOptions {
  std::uint16_t port = 0;
  std::string allowed_origin;
  std::string test_token;
  bool test_receiver = false;
  std::array<ProviderCapability, kMaximumProviderCapabilities> providers{};
  std::size_t provider_count = 0;
  pairing::PairingAuthority *pairing_authority = nullptr;
  pairing::PairingPrompt *pairing_prompt = nullptr;
  DaemonMetrics *metrics = nullptr;
  void (*platform_event_pump)(void *context) noexcept = nullptr;
  void *platform_event_pump_context = nullptr;
};

class FramePublisher;

// Runs until a stop signal arrives: SIGINT or SIGTERM on POSIX, SIGINT or
// SIGBREAK on Windows, where SIGTERM is never delivered to an event loop and
// the companion stops its helper with CTRL_BREAK_EVENT instead. Returns zero
// only after an orderly shutdown.
int run_server(const ServerOptions &options,
               FramePublisher *publisher = nullptr);

} // namespace noisefactor::sync
