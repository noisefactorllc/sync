#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace noisefactor::sync::ndi {

// An NDI frame rate, frame_rate_N / frame_rate_D frames per second.
struct FrameRate {
  int numerator = 0;
  int denominator = 1;

  friend constexpr auto operator==(FrameRate, FrameRate) -> bool = default;
};

// Declared before a sender's second frame gives its cadence: the 1080p60
// that Sync is qualified for.
inline constexpr FrameRate kInitialFrameRate{60, 1};

// The rates NDI receivers and broadcast tools expect. A measured cadence
// within kStandardRateTolerance of one of them declares it exactly.
inline constexpr std::array<FrameRate, 13> kStandardFrameRates{{
    {24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {48, 1}, {50, 1},
    {60000, 1001}, {60, 1}, {72, 1}, {90, 1}, {100, 1}, {120, 1},
}};
inline constexpr double kStandardRateTolerance = 0.02;

// A gap longer than this is a pause, not the sender's cadence.
inline constexpr std::uint64_t kMaximumFrameIntervalUs = 1'000'000;

// The NDI SDK defines frame_rate_N / frame_rate_D as "the framerate of the
// current frame". A Sync frame carries no rate, so each sender's rate comes
// from the spacing of its presentation times: the mean of the recent
// intervals, leaving out those far from their median. The mean absorbs the
// jitter of a browser's capture timestamps; leaving out the outliers keeps a
// dropped or late frame from moving it.
class FrameRateTracker {
 public:
  // The rate to declare on the frame with this presentation time.
  auto observe(std::uint64_t presentation_time_us) noexcept -> FrameRate {
    if (has_previous_ && presentation_time_us > previous_us_) {
      const std::uint64_t interval = presentation_time_us - previous_us_;
      if (interval <= kMaximumFrameIntervalUs) {
        intervals_[next_] = interval;
        next_ = (next_ + 1) % intervals_.size();
        count_ = std::min(count_ + 1, intervals_.size());
      }
    }
    has_previous_ = true;
    previous_us_ = presentation_time_us;
    return count_ == 0 ? kInitialFrameRate : rate_for_interval(typical_interval());
  }

  // The declared rate for a frame interval in microseconds.
  [[nodiscard]] static auto rate_for_interval(std::uint64_t interval_us) noexcept -> FrameRate {
    const double measured = 1'000'000.0 / static_cast<double>(interval_us);
    const FrameRate* nearest = nullptr;
    double nearest_error = 0.0;
    for (const FrameRate& rate : kStandardFrameRates) {
      const double standard = static_cast<double>(rate.numerator) / rate.denominator;
      const double error = std::abs(measured - standard) / standard;
      if (nearest == nullptr || error < nearest_error) {
        nearest = &rate;
        nearest_error = error;
      }
    }
    if (nearest != nullptr && nearest_error <= kStandardRateTolerance) return *nearest;
    return {1'000'000, static_cast<int>(interval_us)};
  }

 private:
  [[nodiscard]] auto typical_interval() const noexcept -> std::uint64_t {
    std::array<std::uint64_t, kWindow> sorted{};
    std::copy_n(intervals_.begin(), count_, sorted.begin());
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count_));
    const double median = static_cast<double>(sorted[count_ / 2]);
    double sum = 0.0;
    std::size_t kept = 0;
    for (std::size_t index = 0; index < count_; ++index) {
      const double interval = static_cast<double>(sorted[index]);
      if (interval * 3.0 >= median * 2.0 && interval * 2.0 <= median * 3.0) {
        sum += interval;
        ++kept;
      }
    }
    return static_cast<std::uint64_t>(std::llround(sum / static_cast<double>(kept)));
  }

  // Half a second at 60 fps.
  static constexpr std::size_t kWindow = 30;
  std::array<std::uint64_t, kWindow> intervals_{};
  std::size_t next_ = 0;
  std::size_t count_ = 0;
  bool has_previous_ = false;
  std::uint64_t previous_us_ = 0;
};

}  // namespace noisefactor::sync::ndi
