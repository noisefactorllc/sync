#include <sync/audio_capture.hpp>
#include <sync/server.hpp>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif

namespace sync_audio = noisefactor::sync::audio;
namespace {
class TestCapture final : public sync_audio::Capture {
public:
  TestCapture(unsigned channels, std::atomic<unsigned> &active)
      : channels_(channels), active_(active), buffer_(48000, channels),
        producer_([this](std::stop_token stop) {
          std::vector<float> samples(240 * channels_);
          for (unsigned frame = 0; frame < 240; ++frame)
            for (unsigned channel = 0; channel < channels_; ++channel)
              samples[frame * channels_ + channel] = static_cast<float>(channel + 1) / 32;
          while (!stop.stop_requested()) {
            buffer_.push(samples);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
        }) { ++active_; }
  ~TestCapture() override { producer_.request_stop(); producer_.join(); --active_; }
  sync_audio::Packet read() override { return buffer_.read(); }
private:
  unsigned channels_;
  std::atomic<unsigned> &active_;
  sync_audio::CaptureBuffer buffer_;
  std::jthread producer_;
};

class TestBackend final : public sync_audio::InputBackend {
public:
  std::vector<sync_audio::Source> sources() override {
    return {{"audio_32", "32 channel fixture", 32, 48000},
            {"audio_8", "8 channel fixture", 8, 48000},
            {"audio_2", "2 channel fixture", 2, 48000},
            {"audio_1", "1 channel fixture", 1, 48000},
            {"audio_active", std::to_string(active_.load()), 1, 48000},
            {"audio_slow_completed", std::to_string(slow_completed_.load()), 1, 48000}};
  }
  std::unique_ptr<sync_audio::Capture> open(const std::string &id) override {
    if (id == "audio_slow") std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (const unsigned count : {32u, 8u, 2u, 1u})
      if (id == "audio_" + std::to_string(count) || id == "audio_slow") {
        auto capture = std::make_unique<TestCapture>(count, active_);
        if (id == "audio_slow") ++slow_completed_;
        return capture;
      }
    throw std::runtime_error("Missing test source");
  }
private:
  std::atomic<unsigned> active_{0};
  std::atomic<unsigned> slow_completed_{0};
};
}

int main() {
#if defined(_WIN32)
  // Match syncd's platform-independent readiness and diagnostic byte format.
  ::_setmode(::_fileno(stdout), _O_BINARY);
  ::_setmode(::_fileno(stderr), _O_BINARY);
#endif
  TestBackend backend;
  noisefactor::sync::ServerOptions options;
  options.allowed_origin = "http://127.0.0.1:8000";
  options.test_token = "audio-test-token";
  options.test_receiver = true;
  options.audio_backend = &backend;
  options.providers[0] = {"audio", noisefactor::sync::ProviderDirection::Receive, true, true};
  options.provider_count = 1;
  return noisefactor::sync::run_server(options);
}
