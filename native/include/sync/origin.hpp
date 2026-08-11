#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace noisefactor::sync {

inline constexpr std::size_t kMaximumOriginBytes = 512;
inline constexpr std::size_t kMaximumOriginInputBytes = 1024;

enum class OriginError {
  None,
  Empty,
  TooLong,
  Malformed,
  UnsupportedScheme,
  InsecureRemote,
};

struct NormalizeOriginResult;

class NormalizedOrigin {
 public:
  [[nodiscard]] std::string_view view() const noexcept {
    return {bytes_.data(), length_};
  }
  [[nodiscard]] bool empty() const noexcept { return length_ == 0; }
  friend bool operator==(const NormalizedOrigin& left,
                         const NormalizedOrigin& right) noexcept {
    return left.view() == right.view();
  }

 private:
  friend struct NormalizeOriginResult;
  friend NormalizeOriginResult normalize_origin(std::string_view) noexcept;
  std::array<char, kMaximumOriginBytes> bytes_{};
  std::size_t length_ = 0;
};

struct NormalizeOriginResult {
  OriginError error = OriginError::Malformed;
  NormalizedOrigin origin{};
  [[nodiscard]] bool ok() const noexcept { return error == OriginError::None; }
};

[[nodiscard]] NormalizeOriginResult normalize_origin(std::string_view input) noexcept;

}  // namespace noisefactor::sync
