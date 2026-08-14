#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace noisefactor::sync::companion {

enum class CompanionState {
  Starting,
  Running,
  Recovering,
  RecoveryExhausted,
  External,
  Stopped,
  Failed,
  PortConflict,
};

struct HealthSnapshot {
  bool reachable = false;
  bool compatible = false;
  std::string product;
  std::string version;
  bool syphon_available = false;
  std::optional<std::size_t> active_senders;
};

struct RecoverySchedule {
  std::uint64_t generation = 0;
  std::size_t attempt = 0;
  std::uint64_t due_ms = 0;

  auto operator==(const RecoverySchedule&) const -> bool = default;
};

class CompanionModel {
 public:
  static constexpr std::size_t kMaximumStderrBytes = 65'536;
  static constexpr std::size_t kMaximumRecoveryAttempts = 3;
  static constexpr std::uint64_t kRecoveryProbationMs = 60'000;
  static constexpr std::uint64_t kRecoveryStartupTimeoutMs = 5'000;

  explicit CompanionModel(std::string product_version);

  void begin_start();
  void helper_started(int pid);
  void helper_started(int pid, std::uint64_t now_ms);
  void observe_health(HealthSnapshot health, std::uint64_t now_ms);
  [[nodiscard]] bool observe_health_failure(std::uint64_t now_ms);
  [[nodiscard]] auto helper_exited(int status, bool expected,
                                   std::uint64_t now_ms)
      -> std::optional<RecoverySchedule>;
  [[nodiscard]] bool begin_recovery_attempt(
      const RecoverySchedule& schedule, std::uint64_t now_ms) noexcept;
  [[nodiscard]] auto recovery_launch_failed(int status,
                                             std::uint64_t now_ms)
      -> std::optional<RecoverySchedule>;
  [[nodiscard]] auto recovery_preflight_conflict(
      HealthSnapshot health, std::uint64_t now_ms)
      -> std::optional<RecoverySchedule>;
  void cancel_recovery() noexcept;
  void manual_restart();
  void append_stderr(std::string_view bytes);

  [[nodiscard]] CompanionState state() const noexcept { return state_; }
  [[nodiscard]] const HealthSnapshot& health() const noexcept { return health_; }
  [[nodiscard]] std::optional<int> owned_pid() const noexcept { return owned_pid_; }
  [[nodiscard]] std::optional<int> last_exit_status() const noexcept {
    return last_exit_status_;
  }
  [[nodiscard]] std::string_view recent_stderr() const noexcept {
    return recent_stderr_;
  }
  [[nodiscard]] std::uint64_t recovery_generation() const noexcept {
    return recovery_generation_;
  }
  [[nodiscard]] std::size_t recovery_attempts() const noexcept {
    return recovery_attempts_;
  }
  [[nodiscard]] bool recovery_active() const noexcept {
    return recovery_episode_active_;
  }
  [[nodiscard]] std::string diagnostics() const;

 private:
  [[nodiscard]] auto schedule_next_recovery(std::uint64_t now_ms)
      -> std::optional<RecoverySchedule>;
  void advance_recovery_generation() noexcept;
  void clear_recovery_episode(bool advance_generation) noexcept;

  std::string product_version_;
  CompanionState state_ = CompanionState::Stopped;
  HealthSnapshot health_;
  std::optional<int> owned_pid_;
  std::optional<int> last_exit_status_;
  std::string recent_stderr_;
  std::uint64_t recovery_generation_ = 0;
  std::size_t recovery_attempts_ = 0;
  bool recovery_episode_active_ = false;
  bool recovery_attempt_claimed_ = false;
  bool recovery_termination_requested_ = false;
  std::optional<RecoverySchedule> recovery_schedule_;
  std::optional<std::uint64_t> recovery_startup_deadline_ms_;
  std::optional<std::uint64_t> recovery_healthy_since_ms_;
};

[[nodiscard]] std::string_view state_name(CompanionState state) noexcept;

} // namespace noisefactor::sync::companion
