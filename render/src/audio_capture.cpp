#include "audio_capture.h"

#include <chrono>
#include <exception>
#include <utility>

namespace noisefactor::sync::render_helper {

namespace {

// How often a missing source is looked for. Listing the sources costs about a
// millisecond and a half, on the search thread; a returning device is heard
// within half a second.
constexpr auto kSearchInterval = std::chrono::milliseconds(500);

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

AudioCapture::AudioCapture(QString wanted, std::unique_ptr<audio::InputBackend> backend)
    : wanted_(std::move(wanted)), backend_(std::move(backend)) {}

AudioCapture::~AudioCapture() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (searcher_.joinable()) searcher_.join();
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
  QString error;
  if (auto opened = open(error)) {
    capture_ = std::move(opened->capture);
    source_ = std::move(opened->source);
    change_ = Change{true, {}};
    return;
  }
  wait_for_source(nullptr, error);
}

auto AudioCapture::read() -> audio::Packet {
  if (!capture_) take_found();
  if (!capture_) return {};
  try {
    return capture_->read();
  } catch (const std::exception& e) {
    wait_for_source(std::move(capture_), QString::fromUtf8(e.what()));
  } catch (...) {
    wait_for_source(std::move(capture_), QStringLiteral("audio capture failed"));
  }
  return {};
}

auto AudioCapture::take_change() -> std::optional<Change> {
  return std::exchange(change_, std::nullopt);
}

void AudioCapture::wait_for_source(std::unique_ptr<audio::Capture> failed, QString reason) {
  change_ = Change{false, std::move(reason)};
  searcher_ = std::thread([this, failed = std::move(failed)]() mutable { search(std::move(failed)); });
}

void AudioCapture::search(std::unique_ptr<audio::Capture> failed) {
  for (;;) {
    {
      std::unique_lock lock(mutex_);
      if (wake_.wait_for(lock, kSearchInterval, [this] { return stopping_; })) return;
    }
    // A failed stream is closed here, a whole interval after it failed:
    // RtAudio reports an unplugged device from inside its own close, on the
    // HAL's thread, and finishes that close after the report returns.
    failed.reset();
    QString error;
    auto opened = open(error);
    if (!opened) continue;
    std::lock_guard lock(mutex_);
    found_ = std::move(opened);
    return;
  }
}

void AudioCapture::take_found() {
  std::optional<Opened> found;
  {
    std::lock_guard lock(mutex_);
    found = std::exchange(found_, std::nullopt);
  }
  if (!found) return;
  // The search ends as it hands over a source, so this join does not wait.
  searcher_.join();
  capture_ = std::move(found->capture);
  source_ = std::move(found->source);
  change_ = Change{true, {}};
}

}  // namespace noisefactor::sync::render_helper
