#pragma once

// The synthetic fixture producer shared by the fixture captures in
// native/src/audio_native.cpp and native/test/audio_test_server.cpp, and by
// the device-free wall-clock regression in native/test/audio_fixture_test.cpp.
//
// The fixture emulates a continuous 48 kHz capture device: every 5 ms
// wall-clock slot produces its 240 frames, and the waveform is a pure
// function of the absolute frame index so the stream's content follows the
// published cursor. A producer wake that missed slots (descheduling,
// suspend) produces the whole missed window, so the source's production
// always tracks the wall clock. The capture ring is the retention bound: a
// reader that was away sees the overflow counted as dropped frames, and the
// acceptance collector's wall-clock production ledger sees the shortfall.
// Contiguous underproduction -- a cursor that never jumps while the source
// produced fewer than 48,000 frames per wall-clock second -- is the defect
// this unit exists to prevent (the E14-E16 soaks lost ~2% of production to
// it with no cursor gap). Catch-up cost is proportional to the missed window
// (about a hundredth of it in practice), so a long suspend spends a bounded
// catch-up rather than losing the window.

#include <sync/audio_capture.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace noisefactor::sync::audio::fixture {

enum class WaveformPattern {
  LinearRamp,
  OrthogonalTones,
  SteppedPulse,
};

inline constexpr unsigned kFixtureSampleRate = 48'000;
inline constexpr unsigned kFramesPerSlot = 240;
inline constexpr std::int64_t kSlotIntervalUs = 5'000;
inline constexpr auto kSlotInterval = std::chrono::microseconds(kSlotIntervalUs);
inline constexpr std::size_t kFixtureRingFrames = 65'536;

// The real clock: sleeps the calling thread. The regression substitutes a
// controlled clock whose sleep_until advances virtual time without pausing.
struct SteadyClock {
  std::chrono::steady_clock::time_point now() {
    return std::chrono::steady_clock::now();
  }
  void sleep_until(std::chrono::steady_clock::time_point deadline) {
    std::this_thread::sleep_until(deadline);
  }
};

// Produces the fixture stream into `buffer` until `stop` is set. A wake that
// missed wall-clock slots produces every missed block, so the published
// stream stays contiguous and its production covers the full wall-clock
// window; the ring reports whatever a stalled reader could not retain as
// dropped frames instead of the source silently underproducing.
template <typename Clock>
void produce(CaptureBuffer &buffer, unsigned channels, WaveformPattern pattern,
             std::atomic<bool> &stop, Clock &clock) {
  std::vector<float> samples(kFramesPerSlot * channels);
  const auto fill_samples = [&samples, channels, pattern](std::uint64_t frame_offset) {
    if (pattern == WaveformPattern::LinearRamp) {
      for (unsigned frame = 0; frame < kFramesPerSlot; ++frame)
        for (unsigned channel = 0; channel < channels; ++channel)
          samples[frame * channels + channel] = static_cast<float>(channel + 1) / 32.0f;
    } else if (pattern == WaveformPattern::OrthogonalTones) {
      constexpr double kPi = 3.14159265358979323846;
      for (unsigned frame = 0; frame < kFramesPerSlot; ++frame) {
        const double t = static_cast<double>(frame_offset + frame) / 48000.0;
        for (unsigned channel = 0; channel < channels; ++channel) {
          const double freq = 100.0 * (channel + 1);
          samples[frame * channels + channel] = static_cast<float>(std::sin(2.0 * kPi * freq * t));
        }
      }
    } else if (pattern == WaveformPattern::SteppedPulse) {
      for (unsigned frame = 0; frame < kFramesPerSlot; ++frame) {
        const unsigned active_ch = static_cast<unsigned>(((frame_offset + frame) / 4800) % channels);
        for (unsigned channel = 0; channel < channels; ++channel) {
          samples[frame * channels + channel] = (channel == active_ch) ? 1.0f : 0.0f;
        }
      }
    }
  };
  std::uint64_t total_frames = 0;
  fill_samples(total_frames);
  buffer.push(samples);
  total_frames += kFramesPerSlot;
  auto next_time = clock.now();
  while (!stop.load(std::memory_order_acquire)) {
    next_time += kSlotInterval;
    clock.sleep_until(next_time);
    const auto now = clock.now();
    std::uint64_t blocks = 1;
    if (now > next_time) {
      const auto late =
          std::chrono::duration_cast<std::chrono::microseconds>(now - next_time).count();
      blocks += static_cast<std::uint64_t>(late) / static_cast<std::uint64_t>(kSlotIntervalUs);
    }
    for (std::uint64_t block = 0; block < blocks; ++block) {
      fill_samples(total_frames);
      total_frames += kFramesPerSlot;
      buffer.push(samples);
    }
    // The missed slots are fully accounted, so the schedule stays on the
    // original 5 ms grid: the next wake owes only the slot after the last one
    // accounted here.
    next_time += kSlotInterval * static_cast<std::int64_t>(blocks - 1);
  }
}

}  // namespace noisefactor::sync::audio::fixture
