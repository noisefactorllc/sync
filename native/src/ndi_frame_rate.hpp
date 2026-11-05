#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>

namespace noisefactor::sync::ndi {

// An NDI frame rate, frame_rate_N / frame_rate_D frames per second.
struct FrameRate {
  int numerator = 0;
  int denominator = 1;

  friend constexpr auto operator==(FrameRate, FrameRate) -> bool = default;
};

// Declared until a sender's frames give its cadence: the 1080p60 that Sync is
// qualified for.
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
// from the spacing of its presentation times, and a receiver must not see it
// change while the cadence does not.
//
// The period is the least-squares slope of presentation time against frame
// number over the frames since the last restart, up to kFitCapacity of them. A
// frame's number advances by its interval divided by the reference period,
// rounded, so a dropped frame moves the fit by whole periods rather than
// bending it. Once the fit holds kEstablishedFitFrames frames the reference is
// the fitted period. Before that, whenever a frame arrives, every frame of the
// fit is numbered again from its time by each of a few candidate periods and
// then by the fitted period until the numbering holds, and the numbering whose
// residuals are the smallest share of its period is kept, the longest of those
// about as good: every frame the next one, recent_period(), the period fitted
// on the previous frame, or half of recent_period(). Drops only lengthen intervals, so recent_period() comes
// from the short end of the recent intervals; their median lies well above
// the period of a loop that drops many frames. Video shown on a display that
// refreshes faster has no single-refresh intervals at all, so half of
// recent_period() is the refresh period when the intervals are two and three
// refreshes.
//
// The declared rate changes only when the slope's confidence interval, from
// the fit's own residuals and a Student t value, lies wholly in one place:
//   - in the rates of another family (59.94 and 60, 29.97 and 30, 23.976 and
//     24, or one other standard rate): its integer rate;
//   - outside every standard rate's tolerance: the measured rate, unless the
//     declared measured rate is still within that tolerance of the interval;
//   - in one rate of the declared rate's own family: that rate.
// Until then the declared rate stands. A fit whose residuals are not small
// next to its period has numbered some frames wrongly, which a wide spread of
// frame numbers can hide from the interval; it decides nothing and numbers no
// frame, and a long one restarts like a cadence change. Before the fit is
// established a measured rate also needs residuals within
// kMeasuredResidualShare of the period, which a numbering by the wrong period
// does not leave. The interval is at a two-sided 1e-6 level while the fit
// holds fewer than kReliableFitFrames frames: so few residuals can be small by
// chance, and a fit of three jittered frames can look exact. From
// kReliableFitFrames on, the choice within a family uses a 1e-3 level. 60 and
// 59.94 are 0.1% apart, closer than capture jitter moves a short average, so
// that choice waits for the fit to narrow; families are at least 4% apart and
// a cadence change between them is clear much sooner.
//
// The cadence has changed when most recent intervals are no whole number of
// fitted periods, when the recent frames' own period under their numbering
// moves away from the fitted period, or when the latest kHarmonicIntervals
// intervals are all whole numbers of periods with a common divisor of two or
// more: halving the rate, 60 to 30 fps, keeps every interval a whole number of
// the old period, and random drops leave single-period gaps far more often
// than that. The fit then restarts from the recent frames that match the new
// period.
//
// A sender whose frames fill less than kEnterDeliveredShare of the fitted
// periods of the last kDeliveryWindowUs, on every frame for kDeliveryHoldUs, is
// labelled with the rate it delivers instead: the least-squares rate of its
// frames since the drops began, as the nearest standard rate within tolerance
// or else whole frames a second. That label changes only when the rate
// delivered since then stays more than kDeliveredBand from it for
// kDeliveryHoldUs, and gives way to the cadence label again once its frames
// fill more than kLeaveDeliveredShare of the periods for as long. While the
// delivered share is being held below its threshold, the cadence label shown
// does not change.
class FrameRateTracker {
 public:
  // The rate to declare on the frame with this presentation time.
  auto observe(std::uint64_t presentation_time_us) noexcept -> FrameRate {
    if (!has_previous_) {
      has_previous_ = true;
      previous_us_ = presentation_time_us;
      restart_fit(presentation_time_us);
      return label();
    }
    if (presentation_time_us <= previous_us_) {
      return label();
    }
    const std::uint64_t interval = presentation_time_us - previous_us_;
    previous_us_ = presentation_time_us;
    if (interval > kMaximumFrameIntervalUs) {
      restart_fit(presentation_time_us);
      delivered_since_us_ = presentation_time_us;
      delivered_frames_ = 1;
      return label();
    }
    record_interval(interval);
    const double recent = recent_period();
    const bool long_fit = fit_count_ >= kEstablishedFitFrames;
    const bool established = long_fit && fit_consistent_;
    const double reference = established ? fitted_period_ : recent;
    const std::int64_t periods =
        std::max<std::int64_t>(1, std::llround(static_cast<double>(interval) / reference));
    append_point(point(fit_count_ - 1).index + periods, presentation_time_us);
    if (long_fit && (!fit_consistent_ || cadence_changed())) {
      restart_from_matching_run(recent);
    } else if (const std::int64_t multiple = established ? repeated_multiple() : 0; multiple >= 2) {
      restart_from_matching_run(fitted_period_ * static_cast<double>(multiple));
    } else if (!established) {
      number_best(reference);
    }
    if (fit_count_ >= 3) {
      const Fit fit = fit_period();
      fit_consistent_ = fit.residual_rms <= kMaximumResidualShare * fit.period;
      if (fit_consistent_) {
        fitted_period_ = fit.period;
        decide(fit);
      }
    }
    follow_delivery(presentation_time_us);
    if (sparse_since_us_ == kNotHeld) {
      shown_ = declared_;
    }
    return label();
  }

  // The declared rate for a frame interval in microseconds: the nearest
  // standard rate within tolerance, otherwise the measured rate.
  [[nodiscard]] static auto rate_for_interval(std::uint64_t interval_us) noexcept -> FrameRate {
    if (const FrameRate* standard = nearest_standard(static_cast<double>(interval_us))) {
      return *standard;
    }
    return {1'000'000, static_cast<int>(interval_us)};
  }

 private:
  struct Point {
    std::int64_t index = 0;
    std::uint64_t time_us = 0;
  };

  struct Fit {
    double period = 0.0;
    double standard_error = 0.0;
    double residual_rms = 0.0;
  };

  // The rate this tracker declares now.
  [[nodiscard]] auto label() const noexcept -> FrameRate { return delivering_ ? delivered_ : shown_; }

  [[nodiscard]] static constexpr auto frames_per_second(FrameRate rate) noexcept -> double {
    return static_cast<double>(rate.numerator) / rate.denominator;
  }

  // Whether `condition` has held for kDeliveryHoldUs, counted from `since`,
  // which it starts and resets.
  [[nodiscard]] static auto sustained(std::uint64_t& since, bool condition, std::uint64_t now_us) noexcept
      -> bool {
    if (!condition) {
      since = kNotHeld;
      return false;
    }
    if (since == kNotHeld) since = now_us;
    return now_us - since >= kDeliveryHoldUs;
  }

  // The share of the fitted periods in the last kDeliveryWindowUs that carried
  // a frame, or a negative value while the fit cannot say.
  [[nodiscard]] auto delivered_share(std::uint64_t now_us) const noexcept -> double {
    if (fit_count_ < kEstablishedFitFrames || !fit_consistent_ || point(0).time_us + kDeliveryWindowUs > now_us) {
      return -1.0;
    }
    std::size_t first = fit_count_ - 1;
    while (first > 0 && point(first - 1).time_us + kDeliveryWindowUs >= now_us) --first;
    const std::int64_t periods = point(fit_count_ - 1).index - point(first).index;
    return periods > 0 ? static_cast<double>(fit_count_ - 1 - first) / static_cast<double>(periods) : -1.0;
  }

  // The least-squares slope of presentation time against the count of the
  // frames of the fit from `first` on.
  [[nodiscard]] auto delivered_slope(std::size_t first) const noexcept -> double {
    const double count = static_cast<double>(fit_count_ - first);
    const std::uint64_t origin = point(first).time_us;
    double time_sum = 0.0;
    for (std::size_t offset = first; offset < fit_count_; ++offset) {
      time_sum += static_cast<double>(point(offset).time_us - origin);
    }
    const double count_mean = (count - 1.0) / 2.0;
    const double time_mean = time_sum / count;
    double spread = 0.0;
    double covariance = 0.0;
    for (std::size_t offset = first; offset < fit_count_; ++offset) {
      const double frame = static_cast<double>(offset - first) - count_mean;
      spread += frame * frame;
      covariance += frame * (static_cast<double>(point(offset).time_us - origin) - time_mean);
    }
    return covariance / spread;
  }

  // The label for a delivered `rate` in frames a second.
  [[nodiscard]] static auto delivered_label(double rate) noexcept -> FrameRate {
    if (const FrameRate* standard = nearest_standard(1'000'000.0 / rate)) return *standard;
    return {static_cast<int>(std::max<long long>(1, std::llround(rate))), 1};
  }

  // Enters, updates or leaves the delivered label for the frame at `now_us`.
  void follow_delivery(std::uint64_t now_us) noexcept {
    const double share = delivered_share(now_us);
    if (!delivering_) {
      if (sustained(sparse_since_us_, share >= 0.0 && share < kEnterDeliveredShare, now_us)) {
        const std::uint64_t start = sparse_since_us_ - kDeliveryWindowUs;
        std::size_t first = fit_count_ - 1;
        while (first > 0 && point(first - 1).time_us >= start) --first;
        delivered_since_us_ = point(first).time_us;
        delivered_frames_ = fit_count_ - first;
        delivered_ = delivered_label(1e6 / delivered_slope(first));
        delivering_ = true;
        sparse_since_us_ = dense_since_us_ = off_since_us_ = kNotHeld;
      }
      return;
    }
    if (sustained(dense_since_us_, share > kLeaveDeliveredShare, now_us)) {
      delivering_ = false;
      sparse_since_us_ = dense_since_us_ = off_since_us_ = kNotHeld;
      return;
    }
    ++delivered_frames_;
    const auto span_us = static_cast<double>(now_us - delivered_since_us_);
    if (span_us < static_cast<double>(kDeliveryWindowUs)) return;
    const double rate = static_cast<double>(delivered_frames_ - 1) * 1e6 / span_us;
    const double declared = frames_per_second(delivered_);
    if (sustained(off_since_us_, std::abs(rate - declared) > kDeliveredBand * declared, now_us)) {
      delivered_ = delivered_label(rate);
      off_since_us_ = kNotHeld;
    }
  }

  [[nodiscard]] static auto nearest_standard(double interval_us) noexcept -> const FrameRate* {
    if (!(interval_us > 0.0)) {
      return nullptr;
    }
    const double measured = 1'000'000.0 / interval_us;
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
    return nearest != nullptr && nearest_error <= kStandardRateTolerance ? nearest : nullptr;
  }

  // 60 for 60 and 60000/1001, 30 for 30 and 30000/1001, and so on.
  [[nodiscard]] static constexpr auto family(FrameRate rate) noexcept -> int {
    return rate.denominator == 1001 ? rate.numerator / 1000 : rate.numerator;
  }

  [[nodiscard]] static auto is_standard(FrameRate rate) noexcept -> bool {
    return std::find(kStandardFrameRates.begin(), kStandardFrameRates.end(), rate) !=
           kStandardFrameRates.end();
  }

  // Whether any standard rate's tolerance overlaps [low_us, high_us].
  [[nodiscard]] static auto overlaps_standard(double low_us, double high_us) noexcept -> bool {
    for (const FrameRate& rate : kStandardFrameRates) {
      const double standard = static_cast<double>(rate.numerator) / rate.denominator;
      const double shortest = 1'000'000.0 / ((1.0 + kStandardRateTolerance) * standard);
      const double longest = 1'000'000.0 / ((1.0 - kStandardRateTolerance) * standard);
      if (high_us >= shortest && low_us <= longest) {
        return true;
      }
    }
    return false;
  }

  // Two-sided Student t values for 1 to 30 degrees of freedom, then 40, 60
  // and 120. Between tabled degrees of freedom the smaller one's value is
  // used, which only widens the interval.
  struct TTable {
    std::array<double, 30> first;
    double at_40;
    double at_60;
    double at_120;
  };
  // Two-sided 1e-3.
  static constexpr TTable kSiblingLevel{
      {636.62, 31.599, 12.924, 8.610, 6.869, 5.959, 5.408, 5.041, 4.781, 4.587,
       4.437,  4.318,  4.221,  4.140, 4.073, 4.015, 3.965, 3.922, 3.883, 3.850,
       3.819,  3.792,  3.768,  3.745, 3.725, 3.707, 3.690, 3.674, 3.659, 3.646},
      3.551, 3.460, 3.373};
  // Two-sided 1e-6.
  static constexpr TTable kStrictLevel{
      {636619.77, 1000.0, 130.155, 49.459, 28.478, 20.048, 15.767, 13.257, 11.637, 10.516,
       9.701,     9.085,  8.603,   8.218,  7.903,  7.642,  7.421,  7.232,  7.069,  6.927,
       6.802,     6.691,  6.593,   6.504,  6.424,  6.352,  6.286,  6.225,  6.170,  6.119},
      5.768, 5.449, 5.158};

  [[nodiscard]] static auto t_critical(const TTable& table, std::size_t degrees_of_freedom) noexcept
      -> double {
    if (degrees_of_freedom >= 1 && degrees_of_freedom <= table.first.size()) {
      return table.first[degrees_of_freedom - 1];
    }
    if (degrees_of_freedom >= 120) return table.at_120;
    if (degrees_of_freedom >= 60) return table.at_60;
    if (degrees_of_freedom >= 40) return table.at_40;
    return table.first.back();
  }

  void decide(const Fit& fit) noexcept {
    const std::size_t degrees_of_freedom = fit_count_ - 2;
    const double half_width = t_critical(kStrictLevel, degrees_of_freedom) * fit.standard_error;
    const double low = fit.period - half_width;
    const double high = fit.period + half_width;
    if (!(low > 0.0)) {
      return;
    }
    const FrameRate* shortest = nearest_standard(low);
    const FrameRate* longest = nearest_standard(high);
    if (shortest == nullptr && longest == nullptr) {
      const auto declared_us = static_cast<double>(declared_.denominator);
      const bool still_measured = !is_standard(declared_) && declared_.numerator == 1'000'000 &&
                                  high >= declared_us / (1.0 + kStandardRateTolerance) &&
                                  low <= declared_us / (1.0 - kStandardRateTolerance);
      const bool explained = fit_count_ >= kEstablishedFitFrames ||
                             (interval_count_ >= kHarmonicIntervals &&
                              fit.residual_rms <= kMeasuredResidualShare * fit.period);
      if (!still_measured && explained && !overlaps_standard(low, high)) {
        declared_ = {1'000'000, static_cast<int>(std::max<long long>(1, std::llround(fit.period)))};
      }
      return;
    }
    if (shortest == nullptr || longest == nullptr || family(*shortest) != family(*longest)) {
      return;
    }
    if (!is_standard(declared_) || family(declared_) != family(*shortest)) {
      declared_ = {family(*shortest), 1};
      return;
    }
    const double sibling_half_width =
        t_critical(fit_count_ >= kReliableFitFrames ? kSiblingLevel : kStrictLevel, degrees_of_freedom) *
        fit.standard_error;
    const FrameRate* sibling = nearest_standard(fit.period - sibling_half_width);
    if (sibling != nullptr && fit.period - sibling_half_width > 0.0 &&
        sibling == nearest_standard(fit.period + sibling_half_width)) {
      declared_ = *sibling;
    }
  }

  void record_interval(std::uint64_t interval) noexcept {
    intervals_[next_interval_] = interval;
    next_interval_ = (next_interval_ + 1) % intervals_.size();
    interval_count_ = std::min(interval_count_ + 1, intervals_.size());
  }

  // The median of the recent intervals no longer than one and a half times
  // their tenth percentile. A loop that drops most of its frames still has
  // single intervals at the tenth percentile, and the bound leaves out every
  // interval that spans a dropped frame.
  [[nodiscard]] auto recent_period() const noexcept -> double {
    std::array<std::uint64_t, kRecentIntervals> sorted{};
    std::copy_n(intervals_.begin(), interval_count_, sorted.begin());
    const auto end = sorted.begin() + static_cast<std::ptrdiff_t>(interval_count_);
    std::sort(sorted.begin(), end);
    const double bound = 1.5 * static_cast<double>(sorted[(interval_count_ - 1) / 10]);
    const auto kept = static_cast<std::size_t>(
        std::upper_bound(sorted.begin(), end, static_cast<std::uint64_t>(bound)) - sorted.begin());
    return static_cast<double>(sorted[kept / 2]);
  }

  // Whether the recent frames left the fitted period: their own period under
  // their numbering moved by more than kCadenceChange, or more than half of a
  // full window of intervals lies more than a quarter period from a whole
  // number of periods.
  [[nodiscard]] auto cadence_changed() const noexcept -> bool {
    const std::size_t span = std::min(fit_count_ - 1, kRecentIntervals);
    const Point& newest = point(fit_count_ - 1);
    const Point& oldest = point(fit_count_ - 1 - span);
    const double local = static_cast<double>(newest.time_us - oldest.time_us) /
                         static_cast<double>(newest.index - oldest.index);
    if (std::abs(local - fitted_period_) > kCadenceChange * fitted_period_) {
      return true;
    }
    if (interval_count_ < intervals_.size()) {
      return false;
    }
    std::size_t off_period = 0;
    for (const std::uint64_t interval : intervals_) {
      const double periods = static_cast<double>(interval) / fitted_period_;
      if (periods < 0.75 || std::abs(periods - std::round(periods)) > 0.25) {
        ++off_period;
      }
    }
    return off_period * 2 > intervals_.size();
  }

  [[nodiscard]] auto point(std::size_t offset) const noexcept -> const Point& {
    return points_[(fit_start_ + offset) % points_.size()];
  }

  void restart_fit(std::uint64_t time_us) noexcept {
    fit_start_ = 0;
    fit_count_ = 1;
    points_[0] = {0, time_us};
  }

  void append_point(std::int64_t index, std::uint64_t time_us) noexcept {
    if (fit_count_ == points_.size()) {
      fit_start_ = (fit_start_ + 1) % points_.size();
      --fit_count_;
    }
    points_[(fit_start_ + fit_count_) % points_.size()] = {index, time_us};
    ++fit_count_;
  }

  // The greatest common divisor of the whole numbers of fitted periods that
  // the latest kHarmonicIntervals intervals span, or 0 when one of them is not
  // a whole number of two or more.
  [[nodiscard]] auto repeated_multiple() const noexcept -> std::int64_t {
    if (interval_count_ < kHarmonicIntervals) {
      return 0;
    }
    std::int64_t multiple = 0;
    for (std::size_t back = 1; back <= kHarmonicIntervals; ++back) {
      const std::uint64_t interval = intervals_[(next_interval_ + intervals_.size() - back) % intervals_.size()];
      const double periods = static_cast<double>(interval) / fitted_period_;
      const std::int64_t whole = std::llround(periods);
      if (whole < 2 || std::abs(periods - static_cast<double>(whole)) > 0.25) {
        return 0;
      }
      multiple = std::gcd(multiple, whole);
    }
    return multiple;
  }

  // The mean interval over the fit: the period when every frame is the next.
  [[nodiscard]] auto content_period() const noexcept -> double {
    return static_cast<double>(point(fit_count_ - 1).time_us - point(0).time_us) /
           static_cast<double>(fit_count_ - 1);
  }

  // Numbers the fit by the candidate period whose numbering leaves residuals
  // that are the smallest share of its fitted period: every frame the next
  // one, the reference, the period fitted on the previous frame, and, once the
  // fit has more than kHarmonicIntervals frames, half the reference. Of
  // numberings within kResidualTie of the smallest share, the one with the
  // longest period wins, so a finer grid that explains no more is not chosen.
  void number_best(double reference) noexcept {
    if (fit_count_ < 3) {
      number_from(reference);
      return;
    }
    const std::array<double, 4> candidates{content_period(), reference,
                                           fitted_period_ > 0.0 ? fitted_period_ : reference, reference / 2.0};
    const std::size_t considered = fit_count_ > kHarmonicIntervals ? candidates.size() : 3;
    std::array<double, 4> shares{};
    std::array<double, 4> periods{};
    double smallest = 0.0;
    for (std::size_t index = 0; index < considered; ++index) {
      number_from(candidates[index]);
      const Fit fit = fit_period();
      periods[index] = fit.period;
      shares[index] = fit.period > 0.0 ? fit.residual_rms / fit.period : 1.0;
      if (index == 0 || shares[index] < smallest) smallest = shares[index];
    }
    const double bound = smallest + kResidualTie;
    std::size_t chosen = 1;
    for (std::size_t index = 0; index < considered; ++index) {
      if (shares[index] <= bound && (shares[chosen] > bound || periods[index] > periods[chosen])) {
        chosen = index;
      }
    }
    number_from(candidates[chosen]);
  }

  // Numbers the fit by `period`, then by its own fitted period until the
  // numbering holds.
  void number_from(double period) noexcept {
    renumber(period);
    for (std::size_t pass = 0; pass < kRenumberPasses && fit_count_ >= 3; ++pass) {
      const double fitted = fit_period().period;
      if (!(fitted > 0.0) || !renumber(fitted)) {
        break;
      }
    }
  }

  // Numbers every frame of the fit again from its time, by `period`, and
  // returns whether any number changed.
  auto renumber(double period) noexcept -> bool {
    bool changed = false;
    for (std::size_t offset = 1; offset < fit_count_; ++offset) {
      const Point& earlier = point(offset - 1);
      Point& later = points_[(fit_start_ + offset) % points_.size()];
      const double gap = static_cast<double>(later.time_us - earlier.time_us);
      const std::int64_t index = earlier.index + std::max<std::int64_t>(1, std::llround(gap / period));
      changed = changed || index != later.index;
      later.index = index;
    }
    return changed;
  }

  // Keeps the newest frames whose spacing is a whole number of `period`,
  // numbered by that period.
  void restart_from_matching_run(double period) noexcept {
    std::array<Point, kFitCapacity> run{};
    std::size_t length = 1;
    run[0] = point(fit_count_ - 1);
    for (std::size_t offset = fit_count_ - 1; offset > 0; --offset) {
      const Point& older = point(offset - 1);
      const double gap = static_cast<double>(run[length - 1].time_us - older.time_us);
      const long long periods = std::llround(gap / period);
      if (periods < 1 || periods > 3 || std::abs(gap - static_cast<double>(periods) * period) > period / 3.0) {
        break;
      }
      run[length] = {run[length - 1].index - periods, older.time_us};
      ++length;
    }
    fit_start_ = 0;
    fit_count_ = length;
    for (std::size_t index = 0; index < length; ++index) {
      points_[index] = run[length - 1 - index];
    }
  }

  [[nodiscard]] auto fit_period() const noexcept -> Fit {
    const Point& first = point(0);
    const double count = static_cast<double>(fit_count_);
    double index_sum = 0.0;
    double time_sum = 0.0;
    for (std::size_t offset = 0; offset < fit_count_; ++offset) {
      index_sum += static_cast<double>(point(offset).index - first.index);
      time_sum += static_cast<double>(point(offset).time_us - first.time_us);
    }
    const double index_mean = index_sum / count;
    const double time_mean = time_sum / count;
    double index_spread = 0.0;
    double covariance = 0.0;
    for (std::size_t offset = 0; offset < fit_count_; ++offset) {
      const double index = static_cast<double>(point(offset).index - first.index) - index_mean;
      const double time = static_cast<double>(point(offset).time_us - first.time_us) - time_mean;
      index_spread += index * index;
      covariance += index * time;
    }
    const double period = covariance / index_spread;
    double residuals = 0.0;
    for (std::size_t offset = 0; offset < fit_count_; ++offset) {
      const double index = static_cast<double>(point(offset).index - first.index) - index_mean;
      const double time = static_cast<double>(point(offset).time_us - first.time_us) - time_mean;
      const double residual = time - period * index;
      residuals += residual * residual;
    }
    const double residual_variance = residuals / (count - 2.0);
    return {period, std::sqrt(residual_variance / index_spread), std::sqrt(residual_variance)};
  }

  // Half a second at 60 fps.
  static constexpr std::size_t kRecentIntervals = 30;
  // Half a second at 60 fps: enough frames that the fitted period, rather
  // than recent_period(), numbers the next frame.
  static constexpr std::size_t kEstablishedFitFrames = 30;
  // Consecutive intervals of one repeated gap that make a new, slower cadence.
  static constexpr std::size_t kHarmonicIntervals = 12;
  // The largest residual RMS, as a share of the period, that a fit may have
  // and still decide: capture jitter leaves far less.
  static constexpr double kMaximumResidualShare = 0.2;
  // Refits that number a young fit by its own period, until it holds.
  static constexpr std::size_t kRenumberPasses = 4;
  // About four seconds at 60 fps.
  static constexpr std::size_t kFitCapacity = 256;
  // A recent_period() this far from the fitted period is a new cadence.
  static constexpr double kCadenceChange = 0.15;
  // About a second at 60 fps: enough residuals for the fit's own error.
  static constexpr std::size_t kReliableFitFrames = 60;
  // The largest residual RMS, as a share of the period, from which a fit
  // that is not yet established declares a measured rate: capture jitter
  // leaves less, and frames numbered by a wrong period leave more.
  static constexpr double kMeasuredResidualShare = 0.08;
  // Residual shares this close count as equally small.
  static constexpr double kResidualTie = 0.005;
  // The window over which the delivered share of the fitted periods is
  // measured, and how long a delivered share or rate must hold to act on it.
  static constexpr std::uint64_t kDeliveryWindowUs = 1'000'000;
  static constexpr std::uint64_t kDeliveryHoldUs = 1'000'000;
  // Below this delivered share the label is the delivered rate.
  static constexpr double kEnterDeliveredShare = 0.75;
  // Above this delivered share the label is the cadence again.
  static constexpr double kLeaveDeliveredShare = 0.9;
  // How far the delivered rate may move from its label before it changes.
  static constexpr double kDeliveredBand = 0.05;
  static constexpr std::uint64_t kNotHeld = ~std::uint64_t{0};

  std::array<std::uint64_t, kRecentIntervals> intervals_{};
  std::size_t next_interval_ = 0;
  std::size_t interval_count_ = 0;
  std::array<Point, kFitCapacity> points_{};
  std::size_t fit_start_ = 0;
  std::size_t fit_count_ = 0;
  bool has_previous_ = false;
  std::uint64_t previous_us_ = 0;
  FrameRate declared_ = kInitialFrameRate;
  double fitted_period_ = 0.0;
  bool fit_consistent_ = false;
  FrameRate shown_ = kInitialFrameRate;
  bool delivering_ = false;
  FrameRate delivered_{};
  std::uint64_t delivered_since_us_ = 0;
  std::size_t delivered_frames_ = 0;
  std::uint64_t sparse_since_us_ = kNotHeld;
  std::uint64_t dense_since_us_ = kNotHeld;
  std::uint64_t off_since_us_ = kNotHeld;
};

}  // namespace noisefactor::sync::ndi
