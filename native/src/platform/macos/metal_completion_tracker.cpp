#include "metal_completion_tracker.hpp"

#include <stdexcept>
#include <utility>

namespace noisefactor::sync::detail {

bool MetalFailureLatch::report(ProviderFailure failure) noexcept {
  State expected = State::Empty;
  if (!state_.compare_exchange_strong(expected, State::Writing,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
    return false;
  }
  failure_ = failure;
  state_.store(State::Ready, std::memory_order_release);
  return true;
}

bool MetalFailureLatch::failed() const noexcept {
  return state_.load(std::memory_order_acquire) != State::Empty;
}

auto MetalFailureLatch::failure() const noexcept
    -> std::optional<ProviderFailure> {
  if (state_.load(std::memory_order_acquire) != State::Ready) {
    return std::nullopt;
  }
  return failure_;
}

MetalCompletionTracker::MetalCompletionTracker(
    std::shared_ptr<MetalFailureLatch> failure_latch)
    : failure_latch_(std::move(failure_latch)) {
  if (failure_latch_ == nullptr) {
    throw std::invalid_argument(
        "Metal completion tracker requires a failure latch");
  }
}

bool MetalCompletionTracker::try_begin(
    std::uint64_t submitted_at_ms) noexcept {
  State observed = state_.load(std::memory_order_acquire);
  while (observed == State::Idle || observed == State::Succeeded) {
    if (state_.compare_exchange_weak(observed, State::Preparing,
                                     std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
      submitted_at_ms_ = submitted_at_ms;
      state_.store(State::InFlight, std::memory_order_release);
      return true;
    }
  }
  return false;
}

void MetalCompletionTracker::cancel_before_commit() noexcept {
  State expected = State::InFlight;
  if (state_.compare_exchange_strong(expected, State::Idle,
                                     std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
    return;
  }
  expected = State::Preparing;
  (void)state_.compare_exchange_strong(expected, State::Idle,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire);
}

void MetalCompletionTracker::complete_success() noexcept {
  State expected = State::InFlight;
  (void)state_.compare_exchange_strong(expected, State::Succeeded,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire);
}

void MetalCompletionTracker::complete_failure(
    std::uint32_t native_status, std::int64_t native_error_code) noexcept {
  State expected = State::InFlight;
  if (!state_.compare_exchange_strong(expected, State::CompletingFailure,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
    return;
  }
  (void)failure_latch_->report({
      .kind = ProviderFailureKind::MetalCommandFailed,
      .native_status = native_status,
      .native_error_code = native_error_code,
  });
  state_.store(State::Failed, std::memory_order_release);
}

auto MetalCompletionTracker::poll_watchdog(
    std::uint64_t now_ms, std::uint64_t timeout_ms) noexcept
    -> std::optional<ProviderFailure> {
  State expected = State::InFlight;
  if (timeout_ms != 0 && now_ms >= submitted_at_ms_ &&
      now_ms - submitted_at_ms_ >= timeout_ms &&
      state_.compare_exchange_strong(expected, State::TimingOut,
                                     std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
    (void)failure_latch_->report({
        .kind = ProviderFailureKind::MetalWatchdogTimeout,
    });
    state_.store(State::TimedOut, std::memory_order_release);
  }
  return failure_latch_->failure();
}

bool MetalCompletionTracker::available() const noexcept {
  const State observed = state_.load(std::memory_order_acquire);
  return observed == State::Idle || observed == State::Succeeded;
}

}  // namespace noisefactor::sync::detail
