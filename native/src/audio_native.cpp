#include <sync/audio_capture.hpp>
#include <RtAudio.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace noisefactor::sync::audio {
#if defined(__APPLE__)
bool request_audio_permission();
#endif
namespace {
void ignore_probe_error(RtAudioErrorType, const std::string &) {}

std::string source_id(RtAudio::Api api, const std::string &name) {
  const auto identity = RtAudio::getApiName(api) + ":" + name;
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char *>(identity.data()), identity.size(), digest.data());
  std::string id = "audio_";
  constexpr char hex[] = "0123456789abcdef";
  for (const auto value : digest) { id += hex[value >> 4]; id += hex[value & 15]; }
  return id;
}

struct Device {
  Source source;
  unsigned device_id;
};

std::vector<Device> devices(RtAudio &driver, RtAudio::Api api) {
  std::vector<RtAudio::DeviceInfo> infos;
  std::map<std::string, unsigned> names;
  for (const auto id : driver.getDeviceIds()) {
    auto info = driver.getDeviceInfo(id);
    if (!info.inputChannels) continue;
    ++names[info.name];
    infos.push_back(std::move(info));
  }
  std::vector<Device> result;
  for (const auto &info : infos) {
    // RtAudio does not expose a persistent hardware UID. Ambiguous names are
    // omitted rather than routing a saved source to a different interface.
    if (names[info.name] != 1) continue;
    const auto rate = info.currentSampleRate ? info.currentSampleRate : info.preferredSampleRate;
    if (rate < 8000 || rate > 384000) continue;
    auto name = info.name + " (" + RtAudio::getApiDisplayName(api) + ")";
    if (name.size() > 256) continue;
    result.push_back({{source_id(api, info.name), std::move(name),
                      std::min(info.inputChannels, kMaximumChannels), rate}, info.ID});
  }
  return result;
}

class NativeCapture final : public Capture {
public:
  NativeCapture(std::unique_ptr<RtAudio> driver, const Device &device)
      : buffer_(device.source.sample_rate, device.source.channels),
        channels_(device.source.channels), driver_(std::move(driver)) {
#if defined(__APPLE__)
    if (!request_audio_permission()) throw std::runtime_error("Audio permission denied");
#endif
    driver_->setErrorCallback([this](RtAudioErrorType error, const std::string &message) {
      if (error > RTAUDIO_WARNING) {
        record_failure("driver error (" + std::to_string(static_cast<int>(error)) + "): " + message);
      }
    });
    RtAudio::StreamParameters input{device.device_id, channels_, 0};
    unsigned frames = 256;
    const auto opened = driver_->openStream(nullptr, &input, RTAUDIO_FLOAT32,
        device.source.sample_rate, &frames,
        [](void *, void *input, unsigned count, double, RtAudioStreamStatus status, void *context) {
          auto &capture = *static_cast<NativeCapture *>(context);
          if (!input) {
            capture.record_failure("null input buffer");
            return 2;
          }
          if (status & RTAUDIO_INPUT_OVERFLOW) {
            // RtAudio WASAPI maps AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY to RTAUDIO_INPUT_OVERFLOW.
            // On stream start or initialization, initial discontinuity is standard across Windows WASAPI.
            if (capture.first_buffer_.exchange(false, std::memory_order_relaxed)) {
              // Ignore initial startup discontinuity
            } else {
              // Never report a silently discontinuous stream as healthy CV.
              capture.record_failure("input buffer overflow (status " + std::to_string(status) + ")");
              return 2;
            }
          } else {
            capture.first_buffer_.store(false, std::memory_order_relaxed);
          }
          capture.buffer_.push({static_cast<const float *>(input),
                                static_cast<std::size_t>(count) * capture.channels_});
          return 0;
        }, this);
    if (opened != RTAUDIO_NO_ERROR || driver_->startStream() != RTAUDIO_NO_ERROR)
      throw std::runtime_error("Native audio capture could not start");
  }
  ~NativeCapture() override { if (driver_->isStreamOpen()) driver_->closeStream(); }
  Packet read() override {
    if (failed_.load(std::memory_order_relaxed)) {
      std::lock_guard lock(failure_mutex_);
      throw std::runtime_error("Native audio capture stopped: " +
                               (failure_reason_.empty() ? "unspecified failure" : failure_reason_));
    }
    if (!driver_->isStreamRunning())
      throw std::runtime_error("Native audio capture stopped: stream not running");
    return buffer_.read();
  }
private:
  void record_failure(std::string message) {
    std::lock_guard lock(failure_mutex_);
    if (failure_reason_.empty()) {
      failure_reason_ = std::move(message);
    }
    failed_.store(true, std::memory_order_relaxed);
  }

  CaptureBuffer buffer_;
  unsigned channels_;
  std::atomic<bool> failed_{false};
  std::atomic<bool> first_buffer_{true};
  std::mutex failure_mutex_;
  std::string failure_reason_;
  std::unique_ptr<RtAudio> driver_;
};

enum class WaveformPattern {
  LinearRamp,
  OrthogonalTones,
  SteppedPulse,
};

class FixtureCapture final : public Capture {
public:
  FixtureCapture(unsigned channels, WaveformPattern pattern = WaveformPattern::LinearRamp)
      : channels_(channels), pattern_(pattern),
        buffer_(48000, channels, 65536),
        producer_([this] {
          std::vector<float> samples(240 * channels_);
          auto fill_samples = [this, &samples](std::uint64_t frame_offset) {
            if (pattern_ == WaveformPattern::LinearRamp) {
              for (unsigned frame = 0; frame < 240; ++frame)
                for (unsigned channel = 0; channel < channels_; ++channel)
                  samples[frame * channels_ + channel] = static_cast<float>(channel + 1) / 32.0f;
            } else if (pattern_ == WaveformPattern::OrthogonalTones) {
              constexpr double kPi = 3.14159265358979323846;
              for (unsigned frame = 0; frame < 240; ++frame) {
                const double t = static_cast<double>(frame_offset + frame) / 48000.0;
                for (unsigned channel = 0; channel < channels_; ++channel) {
                  const double freq = 100.0 * (channel + 1);
                  samples[frame * channels_ + channel] = static_cast<float>(std::sin(2.0 * kPi * freq * t));
                }
              }
            } else if (pattern_ == WaveformPattern::SteppedPulse) {
              for (unsigned frame = 0; frame < 240; ++frame) {
                const unsigned active_ch = static_cast<unsigned>(((frame_offset + frame) / 4800) % channels_);
                for (unsigned channel = 0; channel < channels_; ++channel) {
                  samples[frame * channels_ + channel] = (channel == active_ch) ? 1.0f : 0.0f;
                }
              }
            }
          };
          std::uint64_t total_frames = 0;
          fill_samples(total_frames);
          buffer_.push(samples);
          total_frames += 240;
          auto next_time = std::chrono::steady_clock::now();
          while (!stop_.load(std::memory_order_acquire)) {
            next_time += std::chrono::milliseconds(5);
            const auto now = std::chrono::steady_clock::now();
            if (next_time < now) {
              next_time = now + std::chrono::milliseconds(5);
            }
            std::this_thread::sleep_until(next_time);
            fill_samples(total_frames);
            total_frames += 240;
            buffer_.push(samples);
          }
        }) {}
  ~FixtureCapture() override {
    stop_.store(true, std::memory_order_release);
    producer_.join();
  }
  Packet read() override {
    return buffer_.read();
  }
private:
  unsigned channels_;
  WaveformPattern pattern_{WaveformPattern::LinearRamp};
  CaptureBuffer buffer_;
  std::atomic<bool> stop_{false};
  std::thread producer_;
};

bool test_fixtures_enabled() noexcept {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  const auto *env = std::getenv("SYNC_AUDIO_TEST_FIXTURE");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  return env != nullptr && std::string_view(env) != "0" && !std::string_view(env).empty();
}

class NativeBackend final : public InputBackend {
public:
  std::vector<Source> sources() override {
    std::lock_guard lock(mutex_);
    std::vector<Source> sources;
    if (test_fixtures_enabled()) {
      sources.push_back({"audio_32", "32 channel fixture", 32, 48000});
      sources.push_back({"audio_32_tones", "32 channel orthogonal tone fixture", 32, 48000});
      sources.push_back({"audio_32_pulse", "32 channel stepped pulse fixture", 32, 48000});
    }
    for (const auto api : apis()) {
      try {
        RtAudio driver(api, ignore_probe_error);
        if (driver.getCurrentApi() != api) continue;
        for (const auto &device : devices(driver, api)) {
          if (sources.size() == 32) return sources;
          sources.push_back(device.source);
        }
      } catch (...) { /* Other compiled backends remain usable. */ }
    }
    return sources;
  }
  std::unique_ptr<Capture> open(const std::string &id) override {
    std::lock_guard lock(mutex_);
    if (test_fixtures_enabled()) {
      if (id == "audio_32") {
        return std::make_unique<FixtureCapture>(32, WaveformPattern::LinearRamp);
      }
      if (id == "audio_32_tones") {
        return std::make_unique<FixtureCapture>(32, WaveformPattern::OrthogonalTones);
      }
      if (id == "audio_32_pulse") {
        return std::make_unique<FixtureCapture>(32, WaveformPattern::SteppedPulse);
      }
    }
    for (const auto api : apis()) {
      auto driver = std::make_unique<RtAudio>(api, ignore_probe_error);
      if (driver->getCurrentApi() != api) continue;
      for (const auto &device : devices(*driver, api))
        if (device.source.id == id)
          return std::make_unique<NativeCapture>(std::move(driver), device);
    }
    throw std::runtime_error("Native audio source no longer exists");
  }
private:
  static std::vector<RtAudio::Api> apis() {
    std::vector<RtAudio::Api> result;
    RtAudio::getCompiledApi(result);
    return result;
  }
  std::mutex mutex_;
};
} // namespace

std::unique_ptr<InputBackend> make_native_input_backend() {
  return std::make_unique<NativeBackend>();
}
} // namespace noisefactor::sync::audio
