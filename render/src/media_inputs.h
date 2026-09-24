#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

class QCamera;
class QMediaCaptureSession;
class QMediaPlayer;
class QVideoFrame;
class QVideoSink;

namespace nm {
class Backend;
class EffectRegistry;
struct Graph;
}  // namespace nm

namespace noisefactor::sync::render_helper {

// Where a media() step's pixels come from. Chosen on the machine that
// renders, because a controller's camera ids and file paths mean nothing on
// another machine (or even in another origin's sandbox on the same one).
struct MediaSpec {
  enum class Kind { Camera, File };
  Kind kind = Kind::Camera;
  // Camera: empty for the system default, else a case-insensitive substring
  // of the device description or a zero-based device index. File: a path to
  // a still image or a video, which loops.
  QString value;
};

// "camera", "camera:<name-or-index>", "file:<path>".
[[nodiscard]] auto parse_media_spec(const QString& text) -> std::optional<MediaSpec>;

// The media() steps a program reads, in step order, each with the source that
// feeds it: the i-th step takes the i-th source, and steps past the last
// source share the last one. No sources, no bindings.
struct MediaBinding {
  QString texture_id;  // "imageTex_step_N"
  int step_index = 0;
  int source_index = 0;
};
[[nodiscard]] auto bind_media(const QStringList& external_texture_ids, int source_count)
    -> std::vector<MediaBinding>;

// Owns the capture and playback objects and feeds their newest frames to the
// backend before each render, the way Noisedeck's media input manager does:
// upload with flipY false (the media shader flips v itself) and set the
// step's imageSize to the real frame size, which every coordinate in the
// media shader depends on.
class MediaInputs final : public QObject {
  Q_OBJECT

 public:
  explicit MediaInputs(QList<MediaSpec> specs, QObject* parent = nullptr);
  ~MediaInputs() override;

  // Opens every source. A camera waits for permission first, so frames may
  // start after this returns. error explains a false return.
  [[nodiscard]] auto start(QString& error) -> bool;

  // Uploads new frames for the graph's media steps. The backend's GL context
  // must be current (called from the render engine's tick). generation
  // changes whenever a new program is swapped in.
  void apply(nm::Backend& backend, nm::Graph& graph, quint64 generation,
             const nm::EffectRegistry& registry);

  [[nodiscard]] auto source_count() const -> int { return static_cast<int>(sources_.size()); }

 signals:
  // Human-readable state per source, for the helper's event channel.
  void status(int source_index, const QString& state, const QString& detail);

 private:
  struct Source;
  void start_camera(Source& source);
  void accept_frame(Source& source, const QVideoFrame& frame);

  QList<MediaSpec> specs_;
  std::vector<std::unique_ptr<Source>> sources_;
  // What each texture id last received, so an unchanged frame is not
  // uploaded again and a program swap re-applies imageSize.
  struct Uploaded {
    int source_index = -1;
    quint64 serial = 0;
    QSize size;
    quint64 generation = 0;
  };
  QHash<QString, Uploaded> uploaded_;
};

}  // namespace noisefactor::sync::render_helper
