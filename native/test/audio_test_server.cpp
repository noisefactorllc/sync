#include <sync/audio_capture.hpp>
#include <sync/server.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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
  TestCapture(unsigned channels, std::atomic<unsigned> &active,
              std::atomic<unsigned> &wrong_thread_reads,
              std::atomic<unsigned> &wrong_thread_closes,
              unsigned max_reads = 0,
              std::string gate_path = "")
      : channels_(channels), active_(active),
        wrong_thread_reads_(wrong_thread_reads), wrong_thread_closes_(wrong_thread_closes),
        max_reads_(max_reads), gate_path_(std::move(gate_path)),
        owner_thread_(std::this_thread::get_id()), buffer_(48000, channels, 16384),
        producer_([this](std::stop_token stop) {
          std::vector<float> samples(240 * channels_);
          for (unsigned frame = 0; frame < 240; ++frame)
            for (unsigned channel = 0; channel < channels_; ++channel)
              samples[frame * channels_ + channel] = static_cast<float>(channel + 1) / 32;
          auto next_time = std::chrono::steady_clock::now();
          while (!stop.stop_requested()) {
            next_time += std::chrono::milliseconds(5);
            std::this_thread::sleep_until(next_time);
            buffer_.push(samples);
          }
        }) { ++active_; }
  ~TestCapture() override {
    if (std::this_thread::get_id() != owner_thread_) ++wrong_thread_closes_;
    producer_.request_stop();
    producer_.join();
    --active_;
  }
  sync_audio::Packet read() override {
    if (std::this_thread::get_id() != owner_thread_) ++wrong_thread_reads_;
    if (!gate_path_.empty() && !std::filesystem::exists(gate_path_)) {
      throw std::runtime_error("Simulated hardware disconnection (hot unplug)");
    }
    if (max_reads_ > 0 && ++read_count_ > max_reads_) {
      throw std::runtime_error("Simulated hardware stream stoppage or sample-rate switch");
    }
    return buffer_.read();
  }
private:
  unsigned channels_;
  std::atomic<unsigned> &active_;
  std::atomic<unsigned> &wrong_thread_reads_;
  std::atomic<unsigned> &wrong_thread_closes_;
  unsigned max_reads_ = 0;
  unsigned read_count_ = 0;
  std::string gate_path_;
  std::thread::id owner_thread_;
  sync_audio::CaptureBuffer buffer_;
  std::jthread producer_;
};

class TestBackend final : public sync_audio::InputBackend {
public:
  std::vector<sync_audio::Source> sources() override {
    std::vector<sync_audio::Source> list = {
      {"audio_32", "32 channel fixture", 32, 48000},
      {"audio_8", "8 channel fixture", 8, 48000},
      {"audio_2", "2 channel fixture", 2, 48000},
      {"audio_1", "1 channel fixture", 1, 48000},
      {"audio_active", std::to_string(active_.load()), 1, 48000},
      {"audio_wrong_thread_reads", std::to_string(wrong_thread_reads_.load()), 1, 48000},
      {"audio_wrong_thread_closes", std::to_string(wrong_thread_closes_.load()), 1, 48000},
      {"audio_slow_started", std::to_string(slow_started_.load()), 1, 48000},
      {"audio_slow_completed", std::to_string(slow_completed_.load()), 1, 48000},
      {"audio_blocked_completed", std::to_string(blocked_completed_.load()), 1, 48000},
      {"audio_fail_after_2", "Transient failure fixture", 2, 48000},
      {"audio_busy", "Contended busy device", 2, 48000},
      {"audio_permission_denied", "Restricted device", 2, 48000}
    };
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const auto *hotplug_gate = std::getenv("SYNC_AUDIO_HOTPLUG_GATE");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (hotplug_gate && std::filesystem::exists(hotplug_gate)) {
      list.push_back({"audio_hotplug", "Hotplug USB interface", 2, 48000});
    }
    return list;
  }
  std::unique_ptr<sync_audio::Capture> open(const std::string &id) override {
    if (id == "audio_busy") {
      throw std::runtime_error("Audio device is busy or held in exclusive mode");
    }
    if (id == "audio_permission_denied") {
      throw std::runtime_error("Audio permission denied");
    }
    if (id == "audio_hotplug") {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
      const auto *hotplug_gate = std::getenv("SYNC_AUDIO_HOTPLUG_GATE");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
      if (!hotplug_gate || !std::filesystem::exists(hotplug_gate)) {
        throw std::runtime_error("Hotplug audio interface not connected");
      }
      return std::make_unique<TestCapture>(2, active_, wrong_thread_reads_,
                                           wrong_thread_closes_, 0, hotplug_gate);
    }
    if (id == "audio_fail_after_2") {
      return std::make_unique<TestCapture>(2, active_, wrong_thread_reads_,
                                           wrong_thread_closes_, 2);
    }
    if (id == "audio_slow") {
      ++slow_started_;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (id == "audio_blocked") {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
      const auto *gate = std::getenv("SYNC_AUDIO_TEST_GATE");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
      if (!gate) throw std::runtime_error("Missing test gate");
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!std::filesystem::exists(gate)) {
        if (std::chrono::steady_clock::now() >= deadline)
          throw std::runtime_error("Audio test gate timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    for (const unsigned count : {32u, 8u, 2u, 1u})
      if (id == "audio_" + std::to_string(count) || id == "audio_slow" || id == "audio_blocked") {
        auto capture = std::make_unique<TestCapture>(count, active_,
                                                     wrong_thread_reads_, wrong_thread_closes_);
        if (id == "audio_slow") ++slow_completed_;
        if (id == "audio_blocked") ++blocked_completed_;
        return capture;
      }
    throw std::runtime_error("Missing test source");
  }
  bool shutdown_ok() const {
    return active_ == 0 && wrong_thread_reads_ == 0 && wrong_thread_closes_ == 0 &&
           slow_started_ == slow_completed_;
  }
private:
  std::atomic<unsigned> active_{0};
  std::atomic<unsigned> wrong_thread_reads_{0};
  std::atomic<unsigned> wrong_thread_closes_{0};
  std::atomic<unsigned> slow_started_{0};
  std::atomic<unsigned> slow_completed_{0};
  std::atomic<unsigned> blocked_completed_{0};
};
}

int main(int argc, char** argv) {
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
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--port" && i + 1 < argc) {
      options.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
    } else if (arg == "--test-origin" && i + 1 < argc) {
      options.allowed_origin = argv[++i];
    } else if (arg == "--test-token" && i + 1 < argc) {
      options.test_token = argv[++i];
    } else if (arg == "--test-receiver") {
      options.test_receiver = true;
    }
  }
  options.audio_backend = &backend;
  options.providers[0] = {"test", noisefactor::sync::ProviderDirection::Send, true, true};
  options.providers[1] = {"audio", noisefactor::sync::ProviderDirection::Receive, true, true};
  options.provider_count = 2;
  const auto result = noisefactor::sync::run_server(options);
  return result != 0 ? result : (backend.shutdown_ok() ? 0 : 2);
}
