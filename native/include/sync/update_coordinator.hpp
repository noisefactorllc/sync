#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace noisefactor::sync::update {
// One coordinator per daemon. Admission and maintenance reservation share a
// mutex: a status observation by itself is never permission to install.
class Coordinator final {
 public:
  static constexpr std::uint64_t kIdleMs = 60000;
  static constexpr std::uint64_t kPreparationMs = 30000;
  void set_external_state(bool known_idle, std::uint64_t now_ms);
  [[nodiscard]] bool ready(std::uint64_t now_ms);
  [[nodiscard]] bool begin_activity(std::uint64_t now_ms);
  void end_activity(std::uint64_t now_ms);
  [[nodiscard]] std::optional<std::uint64_t> reserve(
      std::string_view sha256, std::uint64_t now_ms);
  [[nodiscard]] bool commit(std::uint64_t token, std::string_view sha256,
                            std::uint64_t now_ms);
  // Cancellation is restricted to the reservation owner by the caller's
  // authenticated management channel. Committed exclusion lasts until cancel
  // or daemon exit; an installer must then own a separate OS installation lock.
  [[nodiscard]] bool cancel(std::uint64_t token, std::uint64_t now_ms);
  [[nodiscard]] bool reserved() const;
 private:
  bool observe_clock(std::uint64_t now_ms);
  void expire(std::uint64_t now_ms);
  mutable std::mutex mutex_;
  std::uint64_t latest_ms_ = 0;
  std::optional<std::uint64_t> idle_since_;
  std::size_t activities_ = 0;
  bool external_idle_ = false;
  std::uint64_t sequence_ = 0;
  std::uint64_t token_ = 0;
  std::uint64_t prepared_ms_ = 0;
  bool committed_ = false;
  std::string digest_;
};
[[nodiscard]] bool valid_digest(std::string_view value) noexcept;
} // namespace noisefactor::sync::update
