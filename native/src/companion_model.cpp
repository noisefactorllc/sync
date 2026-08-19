#include <sync/companion_model.hpp>

#include <algorithm>
#include <sstream>
#include <utility>

namespace noisefactor::sync::companion {
namespace {

constexpr std::uint64_t recovery_delay_ms(std::size_t attempt) noexcept {
  switch (attempt) {
  case 1:
    return 250;
  case 2:
    return 1'000;
  case 3:
    return 4'000;
  default:
    return 0;
  }
}

} // namespace

std::string_view provider_display_name(std::string_view provider_id) noexcept {
  if (provider_id == "syphon") return "Syphon";
  if (provider_id == "spout") return "Spout";
  if (provider_id == "ndi") return "NDI";
  return provider_id;
}

bool AvailableProviders::add(std::string_view provider_id) noexcept {
  if (provider_id.empty() ||
      provider_id.size() > kMaximumHealthProviderIdBytes) {
    return false;
  }
  if (contains(provider_id)) return true;
  if (count_ >= kMaximumHealthProviders) return false;
  Entry &entry = entries_[count_];
  std::copy(provider_id.begin(), provider_id.end(), entry.id.begin());
  entry.length = provider_id.size();
  ++count_;
  return true;
}

bool AvailableProviders::contains(std::string_view provider_id) const noexcept {
  for (std::size_t index = 0; index < count_; ++index) {
    if (entries_[index].view() == provider_id) return true;
  }
  return false;
}

std::string_view AvailableProviders::operator[](std::size_t index) const noexcept {
  if (index >= count_) return {};
  return entries_[index].view();
}

std::string AvailableProviders::summary() const {
  std::string result;
  for (std::size_t index = 0; index < count_; ++index) {
    if (index != 0) result += ", ";
    const std::string_view label = provider_display_name(entries_[index].view());
    result.append(label.data(), label.size());
  }
  return result;
}

CompanionModel::CompanionModel(std::string product_version)
    : product_version_(std::move(product_version)) {}

void CompanionModel::begin_start() {
  cancel_recovery();
  state_ = CompanionState::Starting;
  health_ = {};
  owned_pid_.reset();
  last_exit_status_.reset();
}

void CompanionModel::helper_started(int pid) {
  if (pid <= 0) {
    state_ = CompanionState::Failed;
    owned_pid_.reset();
    return;
  }
  owned_pid_ = pid;
  state_ = CompanionState::Starting;
}

void CompanionModel::helper_started(int pid, std::uint64_t now_ms) {
  if (!recovery_episode_active_ || !recovery_attempt_claimed_) {
    helper_started(pid);
    return;
  }
  if (pid <= 0) {
    owned_pid_.reset();
    state_ = CompanionState::Recovering;
    return;
  }
  owned_pid_ = pid;
  state_ = CompanionState::Recovering;
  recovery_startup_deadline_ms_ = now_ms + kRecoveryStartupTimeoutMs;
  recovery_healthy_since_ms_.reset();
  recovery_termination_requested_ = false;
}

void CompanionModel::observe_health(HealthSnapshot health,
                                    std::uint64_t now_ms) {
  health_ = std::move(health);
  if (!health_.reachable) {
    (void)observe_health_failure(now_ms);
    return;
  }
  if (!health_.compatible) {
    if (!owned_pid_.has_value() && recovery_episode_active_) {
      clear_recovery_episode(true);
    }
    state_ = CompanionState::PortConflict;
    return;
  }
  if (!owned_pid_.has_value()) {
    if (recovery_episode_active_) clear_recovery_episode(true);
    state_ = CompanionState::External;
    return;
  }
  if (recovery_episode_active_) {
    if (recovery_termination_requested_) {
      state_ = CompanionState::Recovering;
      return;
    }
    recovery_startup_deadline_ms_.reset();
    if (!recovery_healthy_since_ms_.has_value()) {
      recovery_healthy_since_ms_ = now_ms;
    } else if (now_ms >= *recovery_healthy_since_ms_ &&
               now_ms - *recovery_healthy_since_ms_ >=
                   kRecoveryProbationMs) {
      clear_recovery_episode(true);
    }
  }
  state_ = CompanionState::Running;
}

bool CompanionModel::observe_health_failure(std::uint64_t now_ms) {
  health_ = {};
  if (recovery_episode_active_) {
    recovery_healthy_since_ms_.reset();
    if (!owned_pid_.has_value()) {
      state_ = recovery_attempts_ >= kMaximumRecoveryAttempts &&
                       !recovery_schedule_.has_value()
                   ? CompanionState::RecoveryExhausted
                   : CompanionState::Recovering;
      return false;
    }
    state_ = CompanionState::Recovering;
    if (!recovery_startup_deadline_ms_.has_value()) {
      recovery_startup_deadline_ms_ = now_ms + kRecoveryStartupTimeoutMs;
    }
    if (!recovery_termination_requested_ &&
        now_ms >= *recovery_startup_deadline_ms_) {
      recovery_termination_requested_ = true;
      return true;
    }
    return false;
  }
  if (state_ == CompanionState::Failed) return false;
  if (!owned_pid_.has_value()) {
    state_ = CompanionState::Stopped;
  } else if (state_ != CompanionState::Starting) {
    state_ = CompanionState::Failed;
  }
  return false;
}

auto CompanionModel::helper_exited(int status, bool expected,
                                   std::uint64_t now_ms)
    -> std::optional<RecoverySchedule> {
  owned_pid_.reset();
  health_ = {};
  last_exit_status_ = status;
  if (expected) {
    clear_recovery_episode(true);
    state_ = CompanionState::Stopped;
    return std::nullopt;
  }
  if (recovery_schedule_.has_value()) return recovery_schedule_;
  recovery_startup_deadline_ms_.reset();
  recovery_healthy_since_ms_.reset();
  recovery_termination_requested_ = false;
  recovery_attempt_claimed_ = false;
  if (!recovery_episode_active_) {
    recovery_episode_active_ = true;
    recovery_attempts_ = 0;
    advance_recovery_generation();
  }
  return schedule_next_recovery(now_ms);
}

bool CompanionModel::begin_recovery_attempt(
    const RecoverySchedule& schedule, std::uint64_t now_ms) noexcept {
  if (!recovery_episode_active_ || recovery_attempt_claimed_ ||
      !recovery_schedule_.has_value() ||
      recovery_schedule_->generation != schedule.generation ||
      recovery_schedule_->attempt != schedule.attempt ||
      recovery_schedule_->due_ms != schedule.due_ms ||
      now_ms < schedule.due_ms) {
    return false;
  }
  recovery_schedule_.reset();
  recovery_attempt_claimed_ = true;
  recovery_startup_deadline_ms_.reset();
  recovery_healthy_since_ms_.reset();
  recovery_termination_requested_ = false;
  state_ = CompanionState::Recovering;
  return true;
}

auto CompanionModel::recovery_launch_failed(int status,
                                            std::uint64_t now_ms)
    -> std::optional<RecoverySchedule> {
  if (!recovery_episode_active_ || !recovery_attempt_claimed_ ||
      owned_pid_.has_value()) {
    return std::nullopt;
  }
  last_exit_status_ = status;
  recovery_attempt_claimed_ = false;
  recovery_startup_deadline_ms_.reset();
  recovery_healthy_since_ms_.reset();
  recovery_termination_requested_ = false;
  return schedule_next_recovery(now_ms);
}

auto CompanionModel::recovery_preflight_conflict(
    HealthSnapshot health, std::uint64_t now_ms)
    -> std::optional<RecoverySchedule> {
  if (!recovery_episode_active_ || !recovery_attempt_claimed_ ||
      owned_pid_.has_value() || !health.reachable || health.compatible) {
    return std::nullopt;
  }
  health_ = std::move(health);
  recovery_attempt_claimed_ = false;
  recovery_startup_deadline_ms_.reset();
  recovery_healthy_since_ms_.reset();
  recovery_termination_requested_ = false;
  return schedule_next_recovery(now_ms);
}

void CompanionModel::advance_recovery_generation() noexcept {
  ++recovery_generation_;
  if (recovery_generation_ == 0) ++recovery_generation_;
}

void CompanionModel::clear_recovery_episode(bool advance_generation) noexcept {
  if (advance_generation) advance_recovery_generation();
  recovery_attempts_ = 0;
  recovery_episode_active_ = false;
  recovery_attempt_claimed_ = false;
  recovery_termination_requested_ = false;
  recovery_schedule_.reset();
  recovery_startup_deadline_ms_.reset();
  recovery_healthy_since_ms_.reset();
}

void CompanionModel::cancel_recovery() noexcept {
  clear_recovery_episode(true);
}

auto CompanionModel::schedule_next_recovery(std::uint64_t now_ms)
    -> std::optional<RecoverySchedule> {
  if (!recovery_episode_active_) return std::nullopt;
  if (recovery_attempts_ >= kMaximumRecoveryAttempts) {
    state_ = CompanionState::RecoveryExhausted;
    recovery_schedule_.reset();
    return std::nullopt;
  }
  const std::size_t attempt = recovery_attempts_ + 1;
  recovery_attempts_ = attempt;
  recovery_schedule_ = RecoverySchedule{
      .generation = recovery_generation_,
      .attempt = attempt,
      .due_ms = now_ms + recovery_delay_ms(attempt),
  };
  state_ = CompanionState::Recovering;
  return recovery_schedule_;
}

void CompanionModel::manual_restart() { begin_start(); }

void CompanionModel::append_stderr(std::string_view bytes) {
  if (bytes.size() >= kMaximumStderrBytes) {
    recent_stderr_.assign(bytes.end() - kMaximumStderrBytes, bytes.end());
    return;
  }
  const std::size_t combined = recent_stderr_.size() + bytes.size();
  if (combined > kMaximumStderrBytes) {
    recent_stderr_.erase(0, combined - kMaximumStderrBytes);
  }
  recent_stderr_.append(bytes);
}

std::string CompanionModel::diagnostics() const {
  std::ostringstream output;
  output << "Sync " << product_version_ << '\n';
  output << "State: " << state_name(state_) << '\n';
  output << "Managed helper PID: ";
  if (owned_pid_.has_value()) {
    output << *owned_pid_;
  } else {
    output << "none";
  }
  output << '\n';
  output << "Service version: "
         << (health_.version.empty() ? "unavailable" : health_.version) << '\n';
  output << "Providers: ";
  if (!health_.reachable) {
    output << "unknown";
  } else if (health_.providers.empty()) {
    output << "none available";
  } else {
    output << health_.providers.summary();
  }
  output << '\n';
  output << "Active senders: ";
  if (health_.active_senders.has_value()) {
    output << *health_.active_senders;
  } else {
    output << "unavailable";
  }
  output << '\n';
  if (last_exit_status_.has_value()) {
    output << "Last helper exit status: " << *last_exit_status_ << '\n';
  }
  if (recovery_episode_active_) {
    output << "Recovery attempts: " << recovery_attempts_ << '/'
           << kMaximumRecoveryAttempts << '\n';
  }
  if (!recent_stderr_.empty()) {
    output << "\nRecent helper output:\n" << recent_stderr_;
    if (recent_stderr_.back() != '\n') output << '\n';
  }
  return output.str();
}

std::string_view state_name(CompanionState state) noexcept {
  switch (state) {
  case CompanionState::Starting:
    return "Starting";
  case CompanionState::Running:
    return "Running";
  case CompanionState::Recovering:
    return "Recovering";
  case CompanionState::RecoveryExhausted:
    return "Recovery exhausted";
  case CompanionState::External:
    return "External";
  case CompanionState::Stopped:
    return "Stopped";
  case CompanionState::Failed:
    return "Failed";
  case CompanionState::PortConflict:
    return "Port conflict";
  }
  return "Unknown";
}

} // namespace noisefactor::sync::companion
