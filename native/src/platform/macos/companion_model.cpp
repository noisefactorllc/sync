#include <sync/platform/companion_model.hpp>

#include <algorithm>
#include <sstream>
#include <utility>

namespace noisefactor::sync::companion {

CompanionModel::CompanionModel(std::string product_version)
    : product_version_(std::move(product_version)) {}

void CompanionModel::begin_start() {
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

void CompanionModel::observe_health(HealthSnapshot health) {
  health_ = std::move(health);
  if (!health_.reachable) {
    observe_health_failure();
    return;
  }
  if (!health_.compatible) {
    state_ = CompanionState::PortConflict;
    return;
  }
  state_ = owned_pid_.has_value() ? CompanionState::Running
                                  : CompanionState::External;
}

void CompanionModel::observe_health_failure() {
  health_ = {};
  // A helper that exited unexpectedly already dropped its pid, so treating a
  // missing pid as a clean stop would repaint Failed as Stopped on the next
  // poll and erase the only visible sign that it crashed. Failed is terminal
  // until the operator restarts.
  if (state_ == CompanionState::Failed) return;
  if (!owned_pid_.has_value()) {
    state_ = CompanionState::Stopped;
  } else if (state_ != CompanionState::Starting) {
    state_ = CompanionState::Failed;
  }
}

void CompanionModel::helper_exited(int status, bool expected) {
  owned_pid_.reset();
  health_ = {};
  last_exit_status_ = status;
  state_ = expected ? CompanionState::Stopped : CompanionState::Failed;
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
  output << "Syphon: "
         << (health_.reachable
                 ? (health_.syphon_available ? "available" : "unavailable")
                 : "unknown")
         << '\n';
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
