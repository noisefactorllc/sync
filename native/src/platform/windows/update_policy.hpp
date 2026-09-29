#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace noisefactor::sync::windows_update {
struct CheckSchedule {
  bool enabled = true;
  std::uint64_t paused_until = 0;
  std::uint64_t last_check = 0;
  static constexpr std::uint64_t day = 86400;
  static constexpr std::uint64_t indefinite = std::numeric_limits<std::uint64_t>::max();
  bool paused(std::uint64_t now) const { return paused_until == indefinite || paused_until > now; }
  bool automatic_due(std::uint64_t now, bool metadata_only_supported = false) const {
    return metadata_only_supported && enabled && !paused(now) &&
           (last_check == 0 || (now >= last_check && now - last_check >= day));
  }
  void pause_for_day(std::uint64_t now) {
    paused_until = now > indefinite - day ? indefinite : now + day;
  }
  void pause_indefinitely() { paused_until = indefinite; }
  void resume() { paused_until = 0; }
};

struct Configuration {
  std::string feed_url;
  std::string eddsa_public_key;
  std::string publisher_sha256;
  std::string framework_sha256;
};

inline bool is_sha256(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  });
}

inline bool digest_matches(std::string_view left, std::string_view right) {
  if (!is_sha256(left) || !is_sha256(right)) return false;
  const auto lower = [](char c) { return c >= 'A' && c <= 'F' ? c + ('a' - 'A') : c; };
  return std::equal(left.begin(), left.end(), right.begin(),
                    [&](char a, char b) { return lower(a) == lower(b); });
}

inline std::string_view configuration_error(const Configuration& config) {
  // Preview clients must not silently switch channel, architecture or origin.
  if (config.feed_url !=
      "https://downloads.noisefactor.io/updates/sync/preview/windows-x64.xml") {
    return "The Windows preview update feed is not configured.";
  }
  const auto& key = config.eddsa_public_key;
  if (key.size() != 44 || key.back() != '=' ||
      !std::all_of(key.begin(), key.end() - 1, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '+' || c == '/';
      }) || key == std::string(43, 'A') + '=') {
    return "The update signing key is not configured.";
  }
  // WinSparkle also validates the decoded public key before initialization.
  if (!is_sha256(config.publisher_sha256)) return "The Windows publisher is not configured.";
  if (!is_sha256(config.framework_sha256)) return "The WinSparkle binary pin is not configured.";
  return {};
}
}  // namespace noisefactor::sync::windows_update
