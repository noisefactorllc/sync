#pragma once

#include <QString>

#include <memory>
#include <optional>
#include <vector>

#include <sync/audio_capture.hpp>

namespace noisefactor::sync::render_helper {

// Chooses the capture source for audio(): an exact source id, else the first
// source whose name contains the text (case-insensitive), and "default" for
// the first source the backend lists.
[[nodiscard]] auto choose_audio_source(const std::vector<audio::Source>& sources,
                                       const QString& wanted) -> std::optional<audio::Source>;

// Captures from one native input with Sync's own audio stack (the RtAudio
// backend syncd serves browsers with), so the render helper hears exactly the
// devices Sync already supports and qualifies. Analysis is the engine's
// (nm::AudioInput); this class only moves samples.
class AudioCapture {
 public:
  explicit AudioCapture(QString wanted);

  [[nodiscard]] auto start(QString& error) -> bool;
  // Samples captured since the last call, interleaved float32; empty when
  // nothing new has arrived.
  [[nodiscard]] auto read() -> audio::Packet;
  [[nodiscard]] auto source() const -> const audio::Source& { return source_; }

 private:
  QString wanted_;
  std::unique_ptr<audio::InputBackend> backend_;
  std::unique_ptr<audio::Capture> capture_;
  audio::Source source_;
};

}  // namespace noisefactor::sync::render_helper
