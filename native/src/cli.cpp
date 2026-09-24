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
  if (value.empty() || value.size() > 2048) {
    return false;
  }
  if (value.starts_with("app://")) return normalize_origin(value).ok();
  if (!(value.starts_with("https://") || value.starts_with("http://"))) {
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

bool valid_camera_device_path(std::string_view value) noexcept {
  constexpr std::string_view prefix = "/dev/video";
  if (value.size() <= prefix.size() || value.size() >= 64 ||
      !value.starts_with(prefix)) {
    return false;
  }
  for (const unsigned char byte : value.substr(prefix.size())) {
    if (byte < static_cast<unsigned char>('0') ||
        byte > static_cast<unsigned char>('9')) {
      return false;
    }
  }
  return true;
}

// A session id or share link, or a server URL, handed to sync-render as one
// argv entry. Bounded and printable, with no spaces, so it cannot smuggle a
// second argument or a control sequence into the helper's command line.
bool valid_render_text(std::string_view value) noexcept {
  if (value.empty() || value.size() > 2048) return false;
  for (const unsigned char byte : value) {
    if (byte <= 0x20U || byte >= 0x7fU) return false;
  }
  return true;
}

// An audio source name or a media spec (camera, camera:<name>, file:<path>)
// for sync-render. Device names and paths contain spaces, which is safe: the
// helper is spawned with an argument vector, never through a shell. Control
// characters and a leading dash are refused, so the value cannot read as a
// flag or carry a terminal sequence. So is a double quote: on Windows libuv
// joins the vector into one command line, and a quote is the character whose
// round trip through that quoting depends most on getting every rule right.
bool valid_render_input(std::string_view value) noexcept {
  if (value.empty() || value.size() > 2048 || value.front() == '-') return false;
  for (const unsigned char byte : value) {
    if (byte < 0x20U || byte == 0x7fU || byte == '"') return false;
  }
  return true;
}

constexpr std::size_t kMaxRenderMedia = 16;

bool valid_render_url(std::string_view value) noexcept {
  return valid_render_text(value) &&
         (value.starts_with("https://") || value.starts_with("http://"));
}

bool parse_dimension(std::string_view value, std::uint32_t& output) noexcept {
  if (value.empty() || value.front() == '0') return false;
  std::uint32_t parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() || parsed > 4096U) return false;
  output = parsed;
  return true;
}

// WIDTHxHEIGHT, each 1..4096: the render ring's and the wire protocol's limit.
bool parse_render_size(std::string_view value, std::uint32_t& width,
                       std::uint32_t& height) noexcept {
  const std::size_t x = value.find('x');
  if (x == std::string_view::npos) return false;
  return parse_dimension(value.substr(0, x), width) &&
         parse_dimension(value.substr(x + 1), height);
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
  return value == "syphon" || value == "spout" || value == "ndi" || value == "camera";
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
  bool saw_camera_device = false;
  bool saw_list = false;
  bool saw_revoke = false;
  bool saw_register_camera = false;
  bool saw_unregister_camera = false;
  bool saw_render_join = false;
  bool saw_render_helper = false;
  bool saw_render_url = false;
  bool saw_render_size = false;
  bool saw_render_audio = false;

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
    if (argument == "--register-camera") {
      if (saw_register_camera) return result;
      saw_register_camera = true;
      continue;
    }
    if (argument == "--unregister-camera") {
      if (saw_unregister_camera) return result;
      saw_unregister_camera = true;
      continue;
    }
    if (argument != "--port" && argument != "--test-origin" &&
        argument != "--test-token" && argument != "--publisher" &&
        argument != "--syphon-framework" && argument != "--spout-library" &&
        argument != "--ndi-runtime" && argument != "--camera-device" &&
        argument != "--revoke-origin" && argument != "--render-join" &&
        argument != "--render-helper" && argument != "--render-seance-url" &&
        argument != "--render-size" && argument != "--render-audio" &&
        argument != "--render-media") {
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
    } else if (argument == "--camera-device" && !saw_camera_device &&
               valid_camera_device_path(value)) {
      saw_camera_device = true;
      options.camera_device_path.assign(value);
    } else if (argument == "--render-join" && !saw_render_join && valid_render_text(value)) {
      saw_render_join = true;
      options.render_join.assign(value);
    } else if (argument == "--render-helper" && !saw_render_helper &&
               valid_runtime_path(value)) {
      saw_render_helper = true;
      options.render_helper_path.assign(value);
    } else if (argument == "--render-seance-url" && !saw_render_url &&
               valid_render_url(value)) {
      saw_render_url = true;
      options.render_seance_url.assign(value);
    } else if (argument == "--render-audio" && !saw_render_audio &&
               valid_render_input(value)) {
      saw_render_audio = true;
      options.render_audio.assign(value);
    } else if (argument == "--render-media" &&
               options.render_media.size() < kMaxRenderMedia && valid_render_input(value)) {
      options.render_media.emplace_back(value);
    } else if (argument == "--render-size" && !saw_render_size &&
               parse_render_size(value, options.render_width, options.render_height)) {
      saw_render_size = true;
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
  const bool camera_device_without_publisher =
      saw_camera_device && saw_publisher &&
      !options.selects_publisher("camera");
  // The helper, server and size only configure a render session; naming
  // them without one is a usage error, like a runtime path without its
  // provider.
  const bool saw_render = saw_render_join || saw_render_helper || saw_render_url ||
                          saw_render_size || saw_render_audio || !options.render_media.empty();
  const bool render_settings_without_join = saw_render && !saw_render_join;

  // Each camera command is the whole command line. Combining them with each
  // other, with pairing management, or with any server argument is a usage
  // error rather than a silent precedence rule.
  const bool camera_management = saw_register_camera || saw_unregister_camera;
  if (camera_management) {
    if (saw_register_camera == saw_unregister_camera || saw_list || saw_revoke || saw_port ||
        saw_origin || saw_token || saw_test_receiver || saw_publisher ||
        saw_runtime_path || saw_camera_device || saw_render) {
      return result;
    }
    options.mode = saw_register_camera ? Mode::RegisterCamera : Mode::UnregisterCamera;
    result.valid = true;
    return result;
  }

  const bool management = saw_list || saw_revoke;
  const bool test_shape = saw_origin || saw_token || saw_test_receiver;
  if (management) {
    if (saw_list == saw_revoke || saw_port || test_shape || saw_publisher ||
        saw_runtime_path || saw_camera_device || saw_render) {
      return result;
    }
    options.mode = saw_list ? Mode::ListPairings : Mode::RevokeOrigin;
    result.valid = true;
    return result;
  }

  if (test_shape) {
    if (!saw_port || !saw_origin || !saw_token ||
        saw_test_receiver == saw_publisher || runtime_path_without_publisher ||
        camera_device_without_publisher || render_settings_without_join ||
        (saw_test_receiver && saw_camera_device)) {
      return result;
    }
    options.mode = Mode::StaticTest;
    result.valid = true;
    return result;
  }

  if (runtime_path_without_publisher || camera_device_without_publisher ||
      render_settings_without_join || options.port == 0) {
    return result;
  }
  options.mode = Mode::Production;
  result.valid = true;
  return result;
}
void print_usage(std::ostream& error) {
  error << "usage: syncd [--port <1-65535>]"
           " [--publisher <syphon|spout|ndi|camera>]..."
           " [--syphon-framework <path>] [--spout-library <path>]"
           " [--ndi-runtime <path>] [--camera-device /dev/videoN]\n"
           "             [--render-join <session-id-or-link> [--render-helper <path>]"
           " [--render-seance-url <url>] [--render-size <WxH>]]\n"
           "       syncd --port <0-65535> --test-origin <origin>"
           " --test-token <token>"
           " (--test-receiver | --publisher <syphon|spout|ndi|camera>...)\n"
           "       syncd --list-pairings\n"
           "       syncd --revoke-origin <origin>\n"
           "       syncd --register-camera\n"
           "       syncd --unregister-camera\n"
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
