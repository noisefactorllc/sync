#include "test_harness.hpp"

#include "../src/audio_fixture_producer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace audio = noisefactor::sync::audio;
namespace fixture = noisefactor::sync::audio::fixture;
using namespace std::chrono_literals;

namespace {

// A clock the regression controls: sleep_until advances virtual time without
// pausing, a pending stall models a producer wake that was descheduled, a
// second stall can be armed for a later deadline, and the clock ends the run
// once the steady-state window closes. produce() is driven synchronously, so
// every expectation below is exact.
struct ControlledClock {
  std::chrono::steady_clock::time_point now_{};
  std::chrono::steady_clock::time_point end_{};
  std::atomic<bool> *stop_ = nullptr;
  bool end_run_ = false;
  std::chrono::microseconds pending_stall_{0};
  bool stall_pending_ = false;
  std::chrono::steady_clock::time_point later_stall_at_{};
  std::chrono::microseconds later_stall_{0};
  bool later_stall_armed_ = false;

  std::chrono::steady_clock::time_point now() const { return now_; }
  void sleep_until(std::chrono::steady_clock::time_point deadline) {
    if (now_ < deadline) now_ = deadline;
    if (stall_pending_) {
      now_ += pending_stall_;
      stall_pending_ = false;
    }
    if (later_stall_armed_ && deadline >= later_stall_at_) {
      later_stall_armed_ = false;
      now_ += later_stall_;
    }
    if (end_run_ && now_ >= end_) stop_->store(true, std::memory_order_release);
  }
  void stall_next_wake(std::chrono::microseconds stall) {
    pending_stall_ = stall;
    stall_pending_ = true;
  }
  void stall_wake_at(std::chrono::steady_clock::time_point deadline,
                     std::chrono::microseconds stall) {
    later_stall_at_ = deadline;
    later_stall_ = stall;
    later_stall_armed_ = true;
  }
};

struct Summary {
  std::uint64_t delivered_frames = 0;
  std::uint64_t final_cursor = 0;
  std::uint64_t first_delivered_frame = 0;
  std::uint64_t dropped_frames = 0;
  std::uint64_t cursor_jumps = 0;
  std::uint64_t first_packet_first_frame = 0;
  std::vector<float> first_packet_samples;
};

Summary drain(audio::CaptureBuffer &buffer) {
  Summary summary;
  bool have_previous = false;
  std::uint64_t previous_end = 0;
  for (;;) {
    const audio::Packet packet = buffer.read();
    const auto frames = packet.samples.size() / packet.channels;
    if (frames == 0) break;
    if (have_previous && packet.first_frame != previous_end) ++summary.cursor_jumps;
    if (!have_previous) {
      summary.first_delivered_frame = packet.first_frame;
      summary.first_packet_first_frame = packet.first_frame;
      summary.first_packet_samples = packet.samples;
    }
    have_previous = true;
    previous_end = packet.first_frame + frames;
    summary.delivered_frames += frames;
    summary.dropped_frames = std::max(summary.dropped_frames, packet.dropped_frames);
  }
  summary.final_cursor = previous_end;
  return summary;
}

void require_equal(std::uint64_t actual, std::uint64_t expected, const char *label) {
  if (actual != expected) {
    throw std::runtime_error(std::string(label) + ": expected " + std::to_string(expected) +
                             ", got " + std::to_string(actual));
  }
}

Summary run_window(std::chrono::milliseconds window, std::chrono::microseconds stall,
                   fixture::WaveformPattern pattern = fixture::WaveformPattern::LinearRamp) {
  audio::CaptureBuffer buffer(fixture::kFixtureSampleRate, 32, fixture::kFixtureRingFrames);
  std::atomic<bool> stop{false};
  ControlledClock clock;
  clock.end_ = clock.now_ + window;
  clock.stop_ = &stop;
  clock.end_run_ = true;
  if (stall.count() > 0) clock.stall_next_wake(stall);
  fixture::produce(buffer, 32, pattern, stop, clock);
  return drain(buffer);
}

}  // namespace

SYNC_TEST(audio_fixture_paces_every_slot_of_the_window_on_time) {
  const auto summary = run_window(1000ms, 0us);
  // 1,000 ms of 48 kHz is 48,000 frames. The fixture pre-produces one 5 ms
  // slot ahead of the wall clock, so the window plus its lead slot is
  // exactly 48,240 frames, contiguous from frame 0 with no drops.
  require_equal(summary.delivered_frames, 48'240, "delivered frames");
  require_equal(summary.first_delivered_frame, 0, "first delivered frame");
  require_equal(summary.final_cursor, 48'240, "final cursor");
  require_equal(summary.dropped_frames, 0, "dropped frames");
  require_equal(summary.cursor_jumps, 0, "cursor jumps");
}

SYNC_TEST(audio_fixture_produces_the_missed_window_after_a_late_wake) {
  // A 500 ms descheduled wake misses 100 slots: 24,000 frames of wall-clock
  // production. Producing only one block for the missed window is the E14-E16
  // defect -- contiguous underproduction with no cursor gap, which every
  // cursor-based check reads as healthy. The missed window must be produced.
  const auto summary = run_window(1000ms, 500ms);
  require_equal(summary.delivered_frames, 48'240, "delivered frames after late wake");
  require_equal(summary.first_delivered_frame, 0, "first delivered frame");
  require_equal(summary.final_cursor, 48'240, "final cursor");
  require_equal(summary.dropped_frames, 0, "dropped frames");
  require_equal(summary.cursor_jumps, 0, "cursor jumps");
}

SYNC_TEST(audio_fixture_produces_the_window_missed_by_two_late_wakes) {
  // Two descheduled wakes in one window (500 ms at the first wake, 100 ms
  // more from the 305 ms deadline) must both be caught up: the schedule
  // realigns and the window still delivers in full, contiguous, no drops.
  audio::CaptureBuffer buffer(fixture::kFixtureSampleRate, 32, fixture::kFixtureRingFrames);
  std::atomic<bool> stop{false};
  ControlledClock clock;
  clock.end_ = clock.now_ + 1000ms;
  clock.stop_ = &stop;
  clock.end_run_ = true;
  clock.stall_next_wake(500ms);
  clock.stall_wake_at(clock.now_ + 305ms, 100ms);
  fixture::produce(buffer, 32, fixture::WaveformPattern::LinearRamp, stop, clock);
  const auto summary = drain(buffer);
  require_equal(summary.delivered_frames, 48'240, "delivered frames after two late wakes");
  require_equal(summary.first_delivered_frame, 0, "first delivered frame");
  require_equal(summary.final_cursor, 48'240, "final cursor");
  require_equal(summary.dropped_frames, 0, "dropped frames");
  require_equal(summary.cursor_jumps, 0, "cursor jumps");
}

SYNC_TEST(audio_fixture_keeps_full_production_when_the_reader_was_away) {
  // A 4,000 ms wake against a 5,000 ms window still produces the whole
  // window: the source never underproduces. The ring is the retention bound,
  // so a reader that was away for the whole window receives only the newest
  // 65,536 frames and sees the rest counted as dropped frames -- the
  // shortfall is visible in the ring's own accounting and in any wall-clock
  // production ledger, never as contiguous underproduction.
  const auto summary =
      run_window(5000ms, 4000ms, fixture::WaveformPattern::OrthogonalTones);
  // 5,000 ms is 240,000 frames; with the lead slot the cursor is 240,240.
  require_equal(summary.final_cursor, 240'240, "final cursor");
  // Every accounted frame is either delivered or counted as dropped.
  require_equal(summary.final_cursor - summary.delivered_frames - summary.dropped_frames, 0,
                "frames neither delivered nor dropped");
  // Only the ring's retention is delivered, starting at its absolute
  // identity, so a receiver cannot mistake the tail for the full window.
  require_equal(summary.delivered_frames, 65'536, "delivered frames");
  require_equal(summary.first_delivered_frame, 174'704, "first delivered frame");
  require_equal(summary.dropped_frames, 174'704, "dropped frames");
  require_equal(summary.cursor_jumps, 0, "cursor jumps");
  // Content follows the absolute cursor: the tones waveform is a pure
  // function of the absolute frame index, so the first delivered sample of
  // every channel matches the fixture's deterministic value there.
  SYNC_REQUIRE(summary.first_packet_samples.size() >= 32);
  const double t = static_cast<double>(summary.first_packet_first_frame) / 48000.0;
  constexpr double kPi = 3.14159265358979323846;
  for (unsigned channel = 0; channel < 32; ++channel) {
    const double expected =
        std::sin(2.0 * kPi * 100.0 * static_cast<double>(channel + 1) * t);
    const double actual = summary.first_packet_samples[channel];
    SYNC_REQUIRE(std::fabs(actual - expected) <= 1e-6);
  }
}
