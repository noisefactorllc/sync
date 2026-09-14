#include <sync/audio_capture.hpp>
#include <RtAudio.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>

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
    driver_->setErrorCallback([this](RtAudioErrorType error, const std::string &) {
      if (error > RTAUDIO_WARNING) failed_.store(true, std::memory_order_relaxed);
    });
    RtAudio::StreamParameters input{device.device_id, channels_, 0};
    unsigned frames = 256;
    const auto opened = driver_->openStream(nullptr, &input, RTAUDIO_FLOAT32,
        device.source.sample_rate, &frames,
        [](void *, void *input, unsigned count, double, RtAudioStreamStatus status, void *context) {
          auto &capture = *static_cast<NativeCapture *>(context);
          if (!input || (status & RTAUDIO_INPUT_OVERFLOW)) {
            // Never report a silently discontinuous stream as healthy CV.
            capture.failed_.store(true, std::memory_order_relaxed);
            return 2;
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
    if (failed_.load(std::memory_order_relaxed) || !driver_->isStreamRunning())
      throw std::runtime_error("Native audio capture stopped");
    return buffer_.read();
  }
private:
  CaptureBuffer buffer_;
  unsigned channels_;
  std::atomic<bool> failed_{false};
  std::unique_ptr<RtAudio> driver_;
};

class NativeBackend final : public InputBackend {
public:
  std::vector<Source> sources() override {
    std::lock_guard lock(mutex_);
    std::vector<Source> sources;
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
