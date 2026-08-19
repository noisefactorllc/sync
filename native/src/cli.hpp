#pragma once

#include <array>
#include <cstddef>
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

// Bounded by PublisherHub's fan-out capacity: the daemon cannot register more
// providers than the hub can hold, so the CLI refuses to accept more.
inline constexpr std::size_t kMaximumPublishers = 4;

enum class Mode { Production, StaticTest, ListPairings, RevokeOrigin };

// Every publisher this build knows how to name. The parser is deliberately
// platform-neutral: naming a publisher that this platform cannot provide is
// not a usage error, it simply resolves to an unavailable provider at startup.
// Keeping the grammar identical everywhere means one set of CLI tests covers
// every platform, and a script written on one machine parses on all of them.
[[nodiscard]] bool is_known_publisher(std::string_view value) noexcept;

struct Options {
  Mode mode = Mode::Production;
  std::uint16_t port = kDefaultPort;
  std::string allowed_origin;
  std::string test_token;
  bool test_receiver = false;
  // An empty selection means "every provider this platform offers"; an
  // explicit selection restricts the daemon to exactly the named providers.
  std::array<std::string, kMaximumPublishers> publishers{};
  std::size_t publisher_count = 0;
  std::string syphon_framework_path;
  std::string spout_library_path;
  std::string ndi_runtime_path;
  NormalizedOrigin revoke_origin{};

  [[nodiscard]] bool selects_publisher(std::string_view id) const noexcept;
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
