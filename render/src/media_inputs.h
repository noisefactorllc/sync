#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVideoFrame>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QCamera;
class QMediaCaptureSession;
class QMediaPlayer;
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

// One camera on this machine, as the capture backend reports it.
struct CameraDevice {
  QString id;  // the backend's stable id, the one QCameraDevice carries
  QString description;
};

// Chooses the camera for a media() step: the system default when nothing is
// named, else the zero-based position named, else the first device whose
// description contains the text (case-insensitive).
[[nodiscard]] auto choose_camera_device(const QList<CameraDevice>& devices,
                                        const QString& wanted) -> std::optional<CameraDevice>;

// One open camera. Frames arrive through the callback given at open; stop()
// ends the capture, and destruction closes it. Only the thread that opened
// the camera may stop or destroy it.
class CameraStream {
 public:
  virtual ~CameraStream() = default;
  virtual void stop() = 0;
};

// The state of the user's consent to capture, as the platform holds it.
struct CameraPermission {
  enum class Status { Granted, Denied, Undetermined };
};

// The camera sources and streams on this machine. The seam for the OS
// capture stack, the way audio::InputBackend is for capture: the helper talks
// to this, and tests stand in for it to unplug and replug a camera.
class CameraBackend {
 public:
  virtual ~CameraBackend() = default;
  // The cameras on this machine now. Cheap enough to call every search.
  [[nodiscard]] virtual auto inputs() const -> QList<CameraDevice> = 0;
  // Calls on_change whenever the machine's camera list changes, until
  // receiver is destroyed.
  virtual void watch(QObject* receiver, std::function<void()> on_change) = 0;
  // The user's consent to capture, asked through the platform when
  // Undetermined; the result reaches on_result on the receiver's thread.
  [[nodiscard]] virtual auto check_permission() const -> CameraPermission::Status = 0;
  virtual void request_permission(
      QObject* receiver, std::function<void(CameraPermission::Status)> on_result) = 0;
  // Opens a camera: frames go to on_frame, failures to on_error. nullptr when
  // the device cannot be opened. Both callbacks fire on the thread that
  // opened the stream (which is the caller's thread).
  [[nodiscard]] virtual auto open(const CameraDevice& device,
                                  const std::function<void(const QVideoFrame&)>& on_frame,
                                  const std::function<void(const QString&)>& on_error)
      -> std::unique_ptr<CameraStream> = 0;
};

// Owns the capture and playback objects and feeds their newest frames to the
// backend before each render, the way Noisedeck's media input manager does:
// upload with flipY false (the media shader flips v itself) and set the
// step's imageSize to the real frame size, which every coordinate in the
// media shader depends on.
//
// Cameras never stop the picture. A camera that is missing at start, or that
// fails or is unplugged later, is unavailable while the source looks for it
// again with the same selection rule, and frames resume when it is back.
class MediaInputs final : public QObject {
  Q_OBJECT

 public:
  explicit MediaInputs(QList<MediaSpec> specs, QObject* parent = nullptr,
                       std::unique_ptr<CameraBackend> backend = nullptr);
  ~MediaInputs() override;

  // Opens every source. A camera waits for permission first, and for its
  // device when none is present, so frames may start after this returns.
  // error explains a false return.
  [[nodiscard]] auto start(QString& error) -> bool;

  // Uploads new frames for the graph's media steps. The backend's GL context
  // must be current (called from the render engine's tick). generation
  // changes whenever a new program is swapped in.
  void apply(nm::Backend& backend, nm::Graph& graph, quint64 generation,
             const nm::EffectRegistry& registry);
  void set_shared_images(QHash<int, QImage> images);

  [[nodiscard]] auto source_count() const -> int { return static_cast<int>(sources_.size()); }

 signals:
  // Human-readable state per source, for the helper's event channel.
  void status(int source_index, const QString& state, const QString& detail);

 private:
  struct Source;
  void open_camera(Source& source);
  void announce_waiting(Source& source, int index, const QString& reason);
  void close_camera(Source& source, const QString& reason);
  void reopen_missing_cameras();
  void accept_frame(Source& source, const QVideoFrame& frame);

  QList<MediaSpec> specs_;
  std::unique_ptr<CameraBackend> backend_;
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
  QHash<int, QImage> shared_images_;
};

}  // namespace noisefactor::sync::render_helper
