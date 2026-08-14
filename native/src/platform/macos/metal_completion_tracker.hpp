#pragma once

#include <sync/frame_receiver.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace noisefactor::sync::detail {

class MetalFailureLatch final {
 public:
  [[nodiscard]] bool report(ProviderFailure failure) noexcept;
  [[nodiscard]] bool failed() const noexcept;
  [[nodiscard]] auto failure() const noexcept
      -> std::optional<ProviderFailure>;

 private:
  enum class State : std::uint8_t {
    Empty,
    Writing,
    Ready,
  };

  std::atomic<State> state_{State::Empty};
  ProviderFailure failure_{};
};

class MetalCompletionTracker final {
 public:
  explicit MetalCompletionTracker(
      std::shared_ptr<MetalFailureLatch> failure_latch);

  [[nodiscard]] bool try_begin(std::uint64_t submitted_at_ms) noexcept;
  void cancel_before_commit() noexcept;
  void complete_success() noexcept;
  void complete_failure(std::uint32_t native_status,
                        std::int64_t native_error_code) noexcept;
  [[nodiscard]] auto poll_watchdog(std::uint64_t now_ms,
                                   std::uint64_t timeout_ms) noexcept
      -> std::optional<ProviderFailure>;
  [[nodiscard]] bool available() const noexcept;

 private:
  enum class State : std::uint8_t {
    Idle,
    Preparing,
    InFlight,
    CompletingFailure,
    TimingOut,
    Succeeded,
    Failed,
    TimedOut,
  };

  std::shared_ptr<MetalFailureLatch> failure_latch_;
  std::atomic<State> state_{State::Idle};
  std::uint64_t submitted_at_ms_ = 0;
};

}  // namespace noisefactor::sync::detail
