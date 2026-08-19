#include "cli.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <ostream>
#include <span>
#include <string_view>

namespace noisefactor::sync::cli {
namespace {

bool valid_test_origin(std::string_view value) noexcept {
  if (value.empty() || value.size() > 2048 ||
      !(value.starts_with("https://") || value.starts_with("http://"))) {
    return false;
  }
  for (const unsigned char byte : value) {
    if (byte <= 0x20U || byte == 0x7fU) return false;
  }
  return true;
}

bool valid_test_token(std::string_view value) noexcept {
  if (value.empty() || value.size() > 256) return false;
  for (const unsigned char byte : value) {
    if (byte < 0x20U || byte > 0x7eU) return false;
  }
  return true;
}

// Shared by every provider runtime-path flag. The daemon loads a module
// from this path, so it is bounded and free of control characters here and
// re-validated by the provider that consumes it.
bool valid_runtime_path(std::string_view value) noexcept {
  if (value.empty() || value.size() > 4096) return false;
  for (const unsigned char byte : value) {
    if (byte < 0x20U || byte == 0x7fU) return false;
  }
  return true;
}

bool parse_port(std::string_view value, std::uint16_t& output) noexcept {
  if (value.empty() || (value.size() > 1 && value.front() == '0')) return false;
  unsigned int parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() ||
      parsed > 65535U) {
    return false;
  }
  output = static_cast<std::uint16_t>(parsed);
  return true;
}

void print_store_error(std::ostream& error, std::string_view code) {
  error << "{\"type\":\"error\",\"code\":\"" << code << "\"}\n";
}

}  // namespace

bool Options::selects_publisher(std::string_view id) const noexcept {
  for (std::size_t index = 0; index < publisher_count; ++index) {
    if (publishers[index] == id) return true;
  }
  return false;
}

bool is_known_publisher(std::string_view value) noexcept {
  return value == "syphon" || value == "spout" || value == "ndi";
}

ParseResult parse(std::span<const std::string_view> arguments) {
  ParseResult result;
  Options& options = result.options;
  bool saw_port = false;
  bool saw_origin = false;
  bool saw_token = false;
  bool saw_test_receiver = false;
  bool saw_syphon_framework = false;
  bool saw_spout_library = false;
  bool saw_ndi_runtime = false;
  bool saw_list = false;
  bool saw_revoke = false;

  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string_view argument = arguments[index];
    if (argument == "--test-receiver") {
      if (saw_test_receiver) return result;
      saw_test_receiver = true;
      options.test_receiver = true;
      continue;
    }
    if (argument == "--list-pairings") {
      if (saw_list) return result;
      saw_list = true;
      continue;
    }
    if (argument != "--port" && argument != "--test-origin" &&
        argument != "--test-token" && argument != "--publisher" &&
        argument != "--syphon-framework" && argument != "--spout-library" &&
        argument != "--ndi-runtime" && argument != "--revoke-origin") {
      return result;
    }
    if (++index >= arguments.size()) return result;
    const std::string_view value = arguments[index];
    if (argument == "--port" && !saw_port &&
        parse_port(value, options.port)) {
      saw_port = true;
    } else if (argument == "--test-origin" && !saw_origin &&
               valid_test_origin(value)) {
      saw_origin = true;
      options.allowed_origin.assign(value);
    } else if (argument == "--test-token" && !saw_token &&
               valid_test_token(value)) {
      saw_token = true;
      options.test_token.assign(value);
    } else if (argument == "--publisher" && is_known_publisher(value) &&
               !options.selects_publisher(value) &&
               options.publisher_count < kMaximumPublishers) {
      options.publishers[options.publisher_count++].assign(value);
    } else if (argument == "--syphon-framework" && !saw_syphon_framework &&
               valid_runtime_path(value)) {
      saw_syphon_framework = true;
      options.syphon_framework_path.assign(value);
    } else if (argument == "--spout-library" && !saw_spout_library &&
               valid_runtime_path(value)) {
      saw_spout_library = true;
      options.spout_library_path.assign(value);
    } else if (argument == "--ndi-runtime" && !saw_ndi_runtime &&
               valid_runtime_path(value)) {
      saw_ndi_runtime = true;
      options.ndi_runtime_path.assign(value);
    } else if (argument == "--revoke-origin" && !saw_revoke) {
      const auto normalized = normalize_origin(value);
      if (!normalized.ok()) return result;
      saw_revoke = true;
      options.revoke_origin = normalized.origin;
    } else {
      return result;
    }
  }

  const bool saw_publisher = options.publisher_count != 0;
  const bool saw_runtime_path =
      saw_syphon_framework || saw_spout_library || saw_ndi_runtime;
  // A runtime path only means something for the provider it configures, so
  // supplying one without naming that provider is a usage error rather than
  // a silently ignored argument.
  const bool runtime_path_without_publisher =
      (saw_syphon_framework && !options.selects_publisher("syphon")) ||
      (saw_spout_library && !options.selects_publisher("spout")) ||
      (saw_ndi_runtime && !options.selects_publisher("ndi"));

  const bool management = saw_list || saw_revoke;
  const bool test_shape = saw_origin || saw_token || saw_test_receiver;
  if (management) {
    if (saw_list == saw_revoke || saw_port || test_shape || saw_publisher ||
        saw_runtime_path) {
      return result;
    }
    options.mode = saw_list ? Mode::ListPairings : Mode::RevokeOrigin;
    result.valid = true;
    return result;
  }

  if (test_shape) {
    if (!saw_port || !saw_origin || !saw_token ||
        saw_test_receiver == saw_publisher || runtime_path_without_publisher) {
      return result;
    }
    options.mode = Mode::StaticTest;
    result.valid = true;
    return result;
  }

  if (runtime_path_without_publisher || options.port == 0) {
    return result;
  }
  options.mode = Mode::Production;
  result.valid = true;
  return result;
}
void print_usage(std::ostream& error) {
  error << "usage: syncd [--port <1-65535>]"
           " [--publisher <syphon|spout|ndi>]..."
           " [--syphon-framework <path>] [--spout-library <path>]"
           " [--ndi-runtime <path>]\n"
           "       syncd --port <0-65535> --test-origin <origin>"
           " --test-token <token>"
           " (--test-receiver | --publisher <syphon|spout|ndi>...)\n"
           "       syncd --list-pairings\n"
           "       syncd --revoke-origin <origin>\n"
           "\n"
           "Naming no publisher selects every provider this platform"
           " offers.\n";
}

int run_management(const Options& options,
                   std::ostream& output,
                   std::ostream& error,
                   const ManagementOverride* test_override) {
  if (options.mode != Mode::ListPairings &&
      options.mode != Mode::RevokeOrigin) {
    print_store_error(error, "invalid_management_mode");
    return kFailureExit;
  }

  std::array<char, kMaximumPairingStorePathBytes> default_path{};
  std::size_t default_path_length = 0;
  std::string_view path;
  PairingStoreFailPoint fail_point = PairingStoreFailPoint::None;
  if (test_override != nullptr) {
    path = test_override->store_path;
    fail_point = test_override->fail_point;
  } else {
    const PairingStoreError path_error =
        default_pairing_store_path(default_path, default_path_length);
    if (path_error != PairingStoreError::None) {
      print_store_error(error, "store_path_unavailable");
      return kFailureExit;
    }
    path = {default_path.data(), default_path_length};
  }

  PairingStore store;
  const PairingStoreError open_error =
      store.open({.path = path, .fail_point = fail_point});
  if (open_error != PairingStoreError::None) {
    print_store_error(error, "store_open_failed");
    return kFailureExit;
  }

  if (options.mode == Mode::ListPairings) {
    std::array<NormalizedOrigin, kMaximumPairingOrigins> origins{};
    const PairingListResult listed = store.list(origins);
    if (listed.error != PairingStoreError::None) {
      print_store_error(error, "store_list_failed");
      return kFailureExit;
    }
    output << "{\"type\":\"pairings\",\"origins\":[";
    for (std::size_t index = 0; index < listed.count; ++index) {
      if (index != 0) output << ',';
      output << '\"' << origins[index].view() << '\"';
    }
    output << "]}\n";
    return 0;
  }

  const PairingRevocationResult revoked = store.revoke(options.revoke_origin);
  if (revoked.commit == PairingCommitState::CommittedDurabilityUncertain) {
    output << "{\"type\":\"revocation\",\"origin\":\""
           << options.revoke_origin.view()
           << "\",\"status\":\"revoked_durability_uncertain\"}\n";
    return kDurabilityUncertainExit;
  }
  if (revoked.error != PairingStoreError::None ||
      (revoked.revoked &&
       revoked.commit != PairingCommitState::CommittedDurable)) {
    print_store_error(error, "revoke_not_committed");
    return kFailureExit;
  }
  output << "{\"type\":\"revocation\",\"origin\":\""
         << options.revoke_origin.view() << "\",\"status\":\""
         << (revoked.revoked ? "revoked" : "not_found") << "\"}\n";
  return 0;
}

}  // namespace noisefactor::sync::cli
