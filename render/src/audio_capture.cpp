#include "audio_capture.h"

#include <exception>
#include <utility>

namespace noisefactor::sync::render_helper {

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

AudioCapture::AudioCapture(QString wanted) : wanted_(std::move(wanted)) {}

auto AudioCapture::start(QString& error) -> bool {
  try {
    backend_ = audio::make_native_input_backend();
    const auto chosen = choose_audio_source(backend_->sources(), wanted_);
    if (!chosen.has_value()) {
      error = QStringLiteral("no audio input matches %1").arg(wanted_);
      return false;
    }
    source_ = *chosen;
    capture_ = backend_->open(source_.id);
  } catch (const std::exception& e) {
    error = QStringLiteral("audio input %1: %2").arg(wanted_, QString::fromUtf8(e.what()));
    return false;
  }
  return capture_ != nullptr;
}

auto AudioCapture::read() -> audio::Packet {
  if (!capture_) return {};
  return capture_->read();
}

}  // namespace noisefactor::sync::render_helper
