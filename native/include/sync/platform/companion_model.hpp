#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace noisefactor::sync::companion {

enum class CompanionState {
  Starting,
  Running,
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

class CompanionModel {
 public:
  static constexpr std::size_t kMaximumStderrBytes = 65'536;

  explicit CompanionModel(std::string product_version);

  void begin_start();
  void helper_started(int pid);
  void observe_health(HealthSnapshot health);
  void observe_health_failure();
  void helper_exited(int status, bool expected);
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
  [[nodiscard]] std::string diagnostics() const;

 private:
  std::string product_version_;
  CompanionState state_ = CompanionState::Stopped;
  HealthSnapshot health_;
  std::optional<int> owned_pid_;
  std::optional<int> last_exit_status_;
  std::string recent_stderr_;
};

[[nodiscard]] std::string_view state_name(CompanionState state) noexcept;

} // namespace noisefactor::sync::companion
