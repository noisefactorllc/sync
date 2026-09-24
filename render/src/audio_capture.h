#pragma once

#include <QString>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <runtime/audio_state.h>

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
// fails or is unplugged later, is unavailable while the capture looks for it
// again with the same selection rule, and capture resumes when it is back.
//
// Devices are opened and closed only on the capture's own audio thread; the
// render thread only reads. Opening a device takes longer than a frame, and
// a driver may tie a stream to the thread that made it: WASAPI initialises
// COM in RtAudio's constructor and uninitialises it in the destructor, which
// must not happen on a thread Qt's COM state lives on.
class AudioCapture {
 public:
  // A change in what the capture hears, for the event log.
  struct Change {
    bool live = false;     // capturing `source`; else waiting for the wanted source
    audio::Source source;  // the source now heard, when live
    QString reason;        // why it is waiting
  };

  explicit AudioCapture(QString wanted);
  AudioCapture(QString wanted, std::unique_ptr<audio::InputBackend> backend);
  ~AudioCapture();
  AudioCapture(const AudioCapture&) = delete;
  auto operator=(const AudioCapture&) -> AudioCapture& = delete;

  // Starts the audio thread and waits for its first attempt to open the
  // wanted source; if that fails, the thread keeps looking.
  void start();
  // Samples captured since the last call, interleaved float32, at most
  // audio::kMaximumPacketFrames of them (read again for the rest); empty
  // when nothing new has arrived or the source is unavailable. Never throws.
  [[nodiscard]] auto read() -> audio::Packet;
  // The changes since the last call, oldest first.
  [[nodiscard]] auto take_changes() -> std::vector<Change>;

 private:
  using Clock = std::chrono::steady_clock;
  struct Opened {
    std::unique_ptr<audio::Capture> capture;
    audio::Source source;
  };
  struct Retired {
    std::unique_ptr<audio::Capture> capture;
    Clock::time_point close_at;
  };
  [[nodiscard]] auto open(QString& error) -> std::optional<Opened>;
  void run();
  void take_found();
  void retire(QString reason);

  QString wanted_;
  std::unique_ptr<audio::InputBackend> backend_;  // the audio thread's, once started
  // Render thread only.
  std::unique_ptr<audio::Capture> capture_;
  std::vector<Change> changes_;
  std::thread thread_;
  // Shared with the audio thread.
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable first_attempt_;
  bool stopping_ = false;
  bool attempted_ = false;
  bool searching_ = true;
  Clock::time_point search_at_{};
  QString error_;
  std::optional<Opened> found_;
  std::vector<Retired> retired_;
};

// The engine's name for a source: audio(name: "...") resolves against it.
[[nodiscard]] auto audio_device_for(const audio::Source& source) -> nm::AudioDevice;

// One rendered frame's audio. Every sample captured since the last frame
// goes to the engine's input, as the default input and as the source's own
// device.
// While the source is unavailable both are disconnected, so levels read zero
// rather than holding the last sound heard. Returns the frame's changes, for
// the event log; `device` follows the source being heard.
[[nodiscard]] auto feed_audio(AudioCapture& capture, nm::AudioInput& input,
                              nm::AudioDevice& device) -> std::vector<AudioCapture::Change>;

}  // namespace noisefactor::sync::render_helper
