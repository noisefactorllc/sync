#pragma once

#include <QString>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
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
//
// Audio never stops the picture. A source that is missing at start, or that
// fails or is unplugged later, is silence while a background thread looks for
// it again with the same selection rule, and capture resumes when it is back.
// Opening a device takes longer than a frame, so the render thread never does
// it after start().
class AudioCapture {
 public:
  // A change in what the capture hears, for the event log.
  struct Change {
    bool live = false;  // capturing source(); else waiting for the wanted source
    QString reason;     // why it is waiting
  };

  explicit AudioCapture(QString wanted);
  AudioCapture(QString wanted, std::unique_ptr<audio::InputBackend> backend);
  ~AudioCapture();
  AudioCapture(const AudioCapture&) = delete;
  auto operator=(const AudioCapture&) -> AudioCapture& = delete;

  // Opens the wanted source if it is there, else starts waiting for it.
  void start();
  // Samples captured since the last call, interleaved float32; empty when
  // nothing new has arrived or the source is unavailable. Never throws.
  [[nodiscard]] auto read() -> audio::Packet;
  // The change since the last call, if any.
  [[nodiscard]] auto take_change() -> std::optional<Change>;
  // The source being captured, or the last one while waiting.
  [[nodiscard]] auto source() const -> const audio::Source& { return source_; }

 private:
  struct Opened {
    std::unique_ptr<audio::Capture> capture;
    audio::Source source;
  };
  [[nodiscard]] auto open(QString& error) -> std::optional<Opened>;
  void wait_for_source(std::unique_ptr<audio::Capture> failed, QString reason);
  void search(std::unique_ptr<audio::Capture> failed);
  void take_found();

  QString wanted_;
  std::unique_ptr<audio::InputBackend> backend_;
  // Render thread only.
  std::unique_ptr<audio::Capture> capture_;
  audio::Source source_;
  std::optional<Change> change_;
  std::thread searcher_;
  // Shared with the searcher.
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::optional<Opened> found_;
};

}  // namespace noisefactor::sync::render_helper
