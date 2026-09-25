#include "audio_capture.h"

#include <algorithm>
#include <deque>
#include <exception>
#include <utility>

namespace noisefactor::sync::render_helper {

namespace {


// A 60 fps frame brings 735 to 800 frames of audio, and one read returns at
// most kMaximumPacketFrames (480, the browser protocol's packet). Reading
// once a frame let the backlog fill the capture ring within seconds, after
// which the analysers heard sound 1.4 s late. The bound only guarantees the
// loop ends: 256 reads is more than a full ring.
constexpr int kMaximumReadsPerFrame = 256;

// The newest packets a frame hands the engine. Its analysers read the newest
// 256 samples and 128-frame quanta, as a browser AnalyserNode does, so older
// sound is past. Before the first program, or after a stall, the whole ring
// (65,536 frames) is waiting; pushing all of it cost 21.5 ms at 32 channels.
// Ten packets is 100 ms at 48 kHz.
constexpr std::size_t kNewestPackets = 10;

}  // namespace

auto choose_audio_source(const std::vector<audio::Source>& sources, const QString& wanted)
    -> std::optional<audio::Source> {
  if (sources.empty()) return std::nullopt;
  if (wanted == QStringLiteral("default")) return sources.front();
  const std::string id = wanted.toStdString();
  for (const audio::Source& source : sources) {
    if (source.id == id) return source;
  }
  for (const audio::Source& source : sources) {
    if (QString::fromStdString(source.name).contains(wanted, Qt::CaseInsensitive)) return source;
  }
  return std::nullopt;
}

AudioCapture::AudioCapture(QString wanted)
    : AudioCapture(std::move(wanted), audio::make_native_input_backend()) {}

AudioCapture::AudioCapture(QString wanted, std::unique_ptr<audio::InputBackend> backend,
                           std::chrono::milliseconds search_interval)
    : wanted_(std::move(wanted)), backend_(std::move(backend)), search_interval_(search_interval) {}

AudioCapture::~AudioCapture() {
  // The live stream is closed where it was opened: at once, unless it has
  // just failed and RtAudio may still be inside its own close.
  auto close_at = Clock::now();
  if (capture_) {
    try {
      (void)capture_->read();
    } catch (...) {
      close_at += search_interval_;
    }
  }
  {
    std::lock_guard lock(mutex_);
    if (capture_) retired_.push_back({std::move(capture_), close_at});
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

auto AudioCapture::open(QString& error) -> std::optional<Opened> {
  try {
    const auto chosen = choose_audio_source(backend_->sources(), wanted_);
    if (!chosen.has_value()) {
      error = QStringLiteral("no audio input matches %1").arg(wanted_);
      return std::nullopt;
    }
    auto capture = backend_->open(chosen->id);
    if (capture) return Opened{std::move(capture), *chosen};
    error = QStringLiteral("audio input %1 did not open").arg(wanted_);
  } catch (const std::exception& e) {
    error = QStringLiteral("audio input %1: %2").arg(wanted_, QString::fromUtf8(e.what()));
  } catch (...) {
    error = QStringLiteral("audio input %1 failed to open").arg(wanted_);
  }
  return std::nullopt;
}

void AudioCapture::start() {
  thread_ = std::thread([this] { run(); });
  std::unique_lock lock(mutex_);
  first_attempt_.wait(lock, [this] { return attempted_; });
  if (!found_) changes_.push_back({false, {}, error_});
  lock.unlock();
  take_found();
}

void AudioCapture::run() {
  std::unique_lock lock(mutex_);
  for (;;) {
    // Closing and opening happen without the lock, so a render-thread read
    // never waits on a driver.
    const auto now = Clock::now();
    const auto due = std::find_if(retired_.begin(), retired_.end(),
                                  [&](const Retired& r) { return r.close_at <= now; });
    if (due != retired_.end()) {
      auto closing = std::move(due->capture);
      retired_.erase(due);
      lock.unlock();
      closing.reset();
      lock.lock();
      continue;
    }
    if (stopping_ && retired_.empty()) break;
    if (!stopping_ && searching_ && search_at_ <= now) {
      lock.unlock();
      QString error;
      auto opened = open(error);
      lock.lock();
      if (opened) {
        found_ = std::move(opened);
        searching_ = false;
      } else {
        search_at_ = Clock::now() + search_interval_;
      }
      if (!attempted_) {
        attempted_ = true;
        error_ = error;
        first_attempt_.notify_all();
      }
      continue;
    }
    auto wake_at = Clock::time_point::max();
    for (const Retired& r : retired_) wake_at = std::min(wake_at, r.close_at);
    if (!stopping_ && searching_) wake_at = std::min(wake_at, search_at_);
    if (wake_at == Clock::time_point::max()) {
      wake_.wait(lock);
    } else {
      wake_.wait_until(lock, wake_at);
    }
  }
  // A source found but never read is closed here too.
  auto unclaimed = std::move(found_);
  found_.reset();
  lock.unlock();
}

auto AudioCapture::read() -> audio::Packet {
  if (!capture_) take_found();
  if (!capture_) return {};
  try {
    return capture_->read();
  } catch (const std::exception& e) {
    retire(QString::fromUtf8(e.what()));
  } catch (...) {
    retire(QStringLiteral("audio capture failed"));
  }
  return {};
}

auto AudioCapture::take_changes() -> std::vector<Change> { return std::exchange(changes_, {}); }

void AudioCapture::take_found() {
  std::lock_guard lock(mutex_);
  if (!found_) return;
  capture_ = std::move(found_->capture);
  changes_.push_back({true, std::move(found_->source), {}});
  found_.reset();
}

void AudioCapture::retire(QString reason) {
  changes_.push_back({false, {}, std::move(reason)});
  {
    std::lock_guard lock(mutex_);
    const auto close_at = Clock::now() + search_interval_;
    retired_.push_back({std::move(capture_), close_at});
    // Look again once the failed stream is closed, not beside it.
    searching_ = true;
    search_at_ = close_at;
  }
  wake_.notify_all();
}

auto audio_device_for(const audio::Source& source) -> nm::AudioDevice {
  return nm::AudioDevice{QString::fromStdString(source.id), QString::fromStdString(source.name),
                         static_cast<int>(source.channels), true};
}

auto feed_audio(AudioCapture& capture, nm::AudioInput& input, nm::AudioDevice& device)
    -> std::vector<AudioCapture::Change> {
  std::vector<AudioCapture::Change> changes;
  std::deque<audio::Packet> newest;
  const auto push_newest = [&] {
    for (const audio::Packet& packet : newest) {
      const auto frames = static_cast<qsizetype>(packet.samples.size() / packet.channels);
      input.pushDefault(packet.samples.data(), frames, static_cast<int>(packet.channels));
      input.pushDevice(device, packet.samples.data(), frames);
    }
    newest.clear();
  };
  for (int reads = 0; reads < kMaximumReadsPerFrame; ++reads) {
    audio::Packet packet = capture.read();
    // In the order they happened: a source heard again (before its first
    // samples, and it may not be the one lost), or a source lost (after
    // its last). The samples before a change go to the device they came
    // from.
    std::vector<AudioCapture::Change> read_changes = capture.take_changes();
    if (!read_changes.empty()) push_newest();
    for (AudioCapture::Change& change : read_changes) {
      if (change.live) {
        device = audio_device_for(change.source);
      } else {
        input.disconnectDefault();
        if (!device.id.isEmpty()) input.disconnectDevice(device.id);
      }
      changes.push_back(std::move(change));
    }
    if (packet.channels == 0 || packet.samples.empty()) break;
    newest.push_back(std::move(packet));
    if (newest.size() > kNewestPackets) newest.pop_front();
  }
  push_newest();
  return changes;
}

}  // namespace noisefactor::sync::render_helper
