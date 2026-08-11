#pragma once

#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

#include <sync/origin.hpp>
#include <sync/pairing_store.hpp>

namespace noisefactor::sync::cli {

inline constexpr std::uint16_t kDefaultPort = 53979;
inline constexpr int kFailureExit = 1;
inline constexpr int kUsageExit = 2;
inline constexpr int kDurabilityUncertainExit = 3;

enum class Mode { Production, StaticTest, ListPairings, RevokeOrigin };

struct Options {
  Mode mode = Mode::Production;
  std::uint16_t port = kDefaultPort;
  std::string allowed_origin;
  std::string test_token;
  bool test_receiver = false;
  std::string publisher = "syphon";
  std::string syphon_framework_path;
  NormalizedOrigin revoke_origin{};
};

struct ParseResult {
  bool valid = false;
  Options options{};
  [[nodiscard]] bool ok() const noexcept { return valid; }
};

[[nodiscard]] ParseResult parse(std::span<const std::string_view> arguments);
void print_usage(std::ostream& error);

// Caller-owned test seam for exercising commit outcomes without touching the
// current user's default pairing store.
struct ManagementOverride {
  std::string_view store_path;
  PairingStoreFailPoint fail_point = PairingStoreFailPoint::None;
};

[[nodiscard]] int run_management(
    const Options& options,
    std::ostream& output,
    std::ostream& error,
    const ManagementOverride* test_override = nullptr);

}  // namespace noisefactor::sync::cli
