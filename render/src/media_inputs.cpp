#include "media_inputs.h"

#include <QCamera>
#include <QCameraDevice>
#include <QCoreApplication>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QMutexLocker>
#include <QPermissions>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

#include <algorithm>
#include <utility>

#include <compiler/effect_registry.h>
#include <runtime/backend.h>
#include <runtime/graph.h>

namespace noisefactor::sync::render_helper {

struct MediaInputs::Source {
  MediaSpec spec;
  QImage still;  // a still-image file, uploaded once per texture
  std::unique_ptr<QVideoSink> sink;
  std::unique_ptr<QMediaPlayer> player;

  // Camera. Empty while the source looks for its device or waits for it to
  // come back.
  std::unique_ptr<CameraStream> stream;
  CameraDevice device;
  // Permission was granted, so the search may open the device when it can.
  bool permitted = false;
  // The event channel carries the waiting state once per loss, not once per
  // search: every half-second retry of the same absence is quiet.
  bool waiting_announced = false;

  // Written by the sink or the stream's frame callback, which may call from a
  // media thread; read by apply() on the render thread.
  QMutex mutex;
  QVideoFrame frame;
  quint64 serial = 0;

  // The newest frame as RGBA8, converted once however many steps share it.
  QImage converted;
  quint64 converted_serial = 0;

  [[nodiscard]] auto index_in(const std::vector<std::unique_ptr<Source>>& sources) const -> int {
    return static_cast<int>(std::find_if(sources.begin(), sources.end(),
                                         [&](const auto& s) { return s.get() == this; }) -
                            sources.begin());
  }
  [[nodiscard]] auto live() const -> bool { return stream != nullptr; }
};

auto parse_media_spec(const QString& text) -> std::optional<MediaSpec> {
  if (text == QStringLiteral("camera")) return MediaSpec{MediaSpec::Kind::Camera, {}};
  if (text.startsWith(QStringLiteral("camera:")) && text.size() > 7) {
    return MediaSpec{MediaSpec::Kind::Camera, text.mid(7)};
  }
  if (text.startsWith(QStringLiteral("file:")) && text.size() > 5) {
    return MediaSpec{MediaSpec::Kind::File, text.mid(5)};
  }
  return std::nullopt;
}

auto bind_media(const QStringList& external_texture_ids, int source_count)
    -> std::vector<MediaBinding> {
  static const QRegularExpression media_id(QStringLiteral("^imageTex_step_(\\d+)$"));
  std::vector<MediaBinding> bindings;
  if (source_count <= 0) return bindings;
  for (const QString& id : external_texture_ids) {
    const auto match = media_id.match(id);
    if (!match.hasMatch()) continue;
    const int step = match.captured(1).toInt();
    if (std::any_of(bindings.begin(), bindings.end(),
                    [&](const MediaBinding& b) { return b.texture_id == id; })) {
      continue;
    }
    bindings.push_back({id, step, 0});
  }
  std::sort(bindings.begin(), bindings.end(),
            [](const MediaBinding& a, const MediaBinding& b) { return a.step_index < b.step_index; });
  for (std::size_t i = 0; i < bindings.size(); ++i) {
    bindings[i].source_index = std::min(static_cast<int>(i), source_count - 1);
  }
  return bindings;
}

auto choose_camera_device(const QList<CameraDevice>& devices, const QString& wanted)
    -> std::optional<CameraDevice> {
  if (wanted.isEmpty()) return std::nullopt;  // unnamed steps take the backend's default
  bool numeric = false;
  const int position = wanted.toInt(&numeric);
  if (numeric && position >= 0 && position < devices.size()) return devices.at(position);
  for (const CameraDevice& candidate : devices) {
    if (candidate.description.contains(wanted, Qt::CaseInsensitive)) return candidate;
  }
  return std::nullopt;
}

// The camera stack over Qt's multimedia backend, what the helper runs with.
namespace {

// One camera over Qt's capture stack. The sink and the callbacks it holds
// live with the stream, so a frame or an error is never delivered after the
// stream is gone.
class QMediaCameraStream final : public CameraStream {
 public:
  QMediaCameraStream(const QCameraDevice& device,
                     std::function<void(const QVideoFrame&)> on_frame,
                     std::function<void(const QString&)> on_error)
      : camera_(device) {
    QObject::connect(sink_.get(), &QVideoSink::videoFrameChanged, sink_.get(),
            [on_frame = std::move(on_frame)](const QVideoFrame& frame) { on_frame(frame); },
            Qt::DirectConnection);
    // Errors arrive queued: the handler retires the stream, and destroying a
    // QCamera from inside its own errorOccurred emission is not safe.
    QObject::connect(&camera_, &QCamera::errorOccurred, sink_.get(),
                     [on_error = std::move(on_error)](QCamera::Error, const QString& message) {
                       on_error(message);
                     }, Qt::QueuedConnection);
    session_.setCamera(&camera_);
    session_.setVideoSink(sink_.get());
    camera_.start();
  }
  void stop() override { camera_.stop(); }

 private:
  QCamera camera_;
  QMediaCaptureSession session_;
  std::unique_ptr<QVideoSink> sink_ = std::make_unique<QVideoSink>();
};

class QMediaCameraBackend final : public CameraBackend {
 public:
  auto inputs() const -> QList<CameraDevice> override {
    QList<CameraDevice> devices;
    for (const QCameraDevice& device : QMediaDevices::videoInputs()) {
      devices.push_back({device.id(), device.description()});
    }
    return devices;
  }
  auto default_device() const -> std::optional<CameraDevice> override {
    const QCameraDevice device = QMediaDevices::defaultVideoInput();
    if (device.isNull()) return std::nullopt;
    return CameraDevice{device.id(), device.description()};
  }
  void watch(QObject* receiver, std::function<void()> on_change) override {
    // One QMediaDevices per watch: it tracks the OS device list and turns its
    // changes into the callback. Parented to the receiver, so it lives only
    // as long as the watching MediaInputs.
    auto* devices = new QMediaDevices(receiver);
    QObject::connect(devices, &QMediaDevices::videoInputsChanged, receiver,
                     [on_change = std::move(on_change)] { on_change(); });
  }
  auto open(const CameraDevice& device, const std::function<void(const QVideoFrame&)>& on_frame,
            const std::function<void(const QString&)>& on_error)
      -> std::unique_ptr<CameraStream> override {
    // The device was listed by inputs() a moment ago; match it again by id,
    // because a QCameraDevice cannot be carried across the seam.
    for (const QCameraDevice& candidate : QMediaDevices::videoInputs()) {
      if (candidate.id() != device.id) continue;
      return std::make_unique<QMediaCameraStream>(candidate, on_frame, on_error);
    }
    return nullptr;
  }
  auto check_permission() const -> CameraPermission::Status override {
    switch (qApp->checkPermission(QCameraPermission())) {
      case Qt::PermissionStatus::Granted: return CameraPermission::Status::Granted;
      case Qt::PermissionStatus::Denied: return CameraPermission::Status::Denied;
      case Qt::PermissionStatus::Undetermined: return CameraPermission::Status::Undetermined;
    }
    return CameraPermission::Status::Undetermined;
  }
  void request_permission(QObject* receiver,
                          std::function<void(CameraPermission::Status)> on_result) override {
    qApp->requestPermission(QCameraPermission(), receiver,
                            [on_result = std::move(on_result)](const QPermission& result) {
                              on_result(result.status() == Qt::PermissionStatus::Granted
                                            ? CameraPermission::Status::Granted
                                            : CameraPermission::Status::Denied);
                            });
  }
};

// How often a source without a camera looks again. The same half second the
// audio capture waits, so a camera the OS never announced (or whose loss was
// never announced) is still heard within half a second of arriving.
constexpr int kSearchIntervalMs = 500;

}  // namespace

MediaInputs::MediaInputs(QList<MediaSpec> specs, QObject* parent,
                         std::unique_ptr<CameraBackend> backend)
    : QObject(parent), specs_(std::move(specs)), backend_(std::move(backend)) {
  if (!backend_ && std::any_of(specs_.begin(), specs_.end(), [](const MediaSpec& spec) {
        return spec.kind == MediaSpec::Kind::Camera;
      })) {
    backend_ = std::make_unique<QMediaCameraBackend>();
  }
}

MediaInputs::~MediaInputs() {
  for (auto& source : sources_) {
    if (source->stream) source->stream->stop();
    if (source->player) source->player->stop();
  }
}

auto MediaInputs::start(QString& error) -> bool {
  bool watching = false;
  for (const MediaSpec& spec : specs_) {
    auto source = std::make_unique<Source>();
    source->spec = spec;
    Source& s = *source;
    const int index = static_cast<int>(sources_.size());
    sources_.push_back(std::move(source));

    if (spec.kind == MediaSpec::Kind::File) {
      const QFileInfo info(spec.value);
      if (!info.isFile()) {
        error = QStringLiteral("media file not found: %1").arg(spec.value);
        return false;
      }
      // A still image is read once. Anything Qt's image readers do not
      // recognise is handed to the media player as a video.
      QImageReader reader(spec.value);
      if (reader.canRead()) {
        s.still = reader.read().convertToFormat(QImage::Format_RGBA8888);
        if (s.still.isNull()) {
          error = QStringLiteral("cannot decode image %1: %2").arg(spec.value, reader.errorString());
          return false;
        }
        emit status(index, QStringLiteral("image"), spec.value);
        continue;
      }
      s.sink = std::make_unique<QVideoSink>();
      s.player = std::make_unique<QMediaPlayer>();
      s.player->setVideoSink(s.sink.get());
      s.player->setLoops(QMediaPlayer::Infinite);
      connect(s.sink.get(), &QVideoSink::videoFrameChanged, this,
              [this, &s](const QVideoFrame& frame) { accept_frame(s, frame); },
              Qt::DirectConnection);
      connect(s.player.get(), &QMediaPlayer::errorOccurred, this,
              [this, index](QMediaPlayer::Error, const QString& message) {
                emit status(index, QStringLiteral("error"), message);
              });
      s.player->setSource(QUrl::fromLocalFile(info.absoluteFilePath()));
      s.player->play();
      emit status(index, QStringLiteral("video"), spec.value);
      continue;
    }

    // Camera: capture needs the user's permission. Qt asks the OS; on macOS
    // the executable must carry a camera usage description for the request
    // to be shown at all.
    switch (backend_->check_permission()) {
      case CameraPermission::Status::Granted:
        s.permitted = true;
        open_camera(s);
        break;
      case CameraPermission::Status::Denied:
        emit status(index, QStringLiteral("denied"), QStringLiteral("camera permission denied"));
        break;
      case CameraPermission::Status::Undetermined:
        emit status(index, QStringLiteral("waiting"), QStringLiteral("camera permission requested"));
        backend_->request_permission(this, [this, &s](CameraPermission::Status granted) {
          if (granted == CameraPermission::Status::Granted) {
            s.permitted = true;
            open_camera(s);
          } else {
            emit status(s.index_in(sources_), QStringLiteral("denied"),
                        QStringLiteral("camera permission denied"));
          }
        });
        break;
    }
    if (!watching) {
      watching = true;
      backend_->watch(this, [this] { reopen_missing_cameras(); });
      // The timer keeps the search alive where the OS never announces a
      // device change; the announcement, when it comes, is the fast path.
      auto* search = new QTimer(this);
      search->setInterval(kSearchIntervalMs);
      connect(search, &QTimer::timeout, this, [this] { reopen_missing_cameras(); });
      search->start();
    }
  }
  return true;
}

void MediaInputs::open_camera(Source& source) {
  if (!source.permitted) return;  // the search does not outrun the permission
  const int index = source.index_in(sources_);
  // An unnamed step opens the platform's default camera, exactly as before
  // the reopen fix; a named step searches the list by position, then
  // description.
  const auto device = source.spec.value.isEmpty() ? backend_->default_device()
                                                  : choose_camera_device(backend_->inputs(),
                                                                         source.spec.value);
  if (!device.has_value()) {
    // Not a failure the helper reports once: the source waits, and the
    // search re-runs the same selection when a camera may have arrived.
    announce_waiting(source, index,
                     source.spec.value.isEmpty() ? QStringLiteral("no camera")
                                                 : QStringLiteral("no camera matches %1").arg(
                                                       source.spec.value));
    return;
  }
  auto on_frame = [this, &source](const QVideoFrame& frame) { accept_frame(source, frame); };
  auto on_error = [this, &source](const QString& message) {
    // A camera that reports an error may still be listed; retire it and let
    // the search decide whether the same device can serve again.
    close_camera(source, message);
  };
  auto stream = backend_->open(*device, std::move(on_frame), std::move(on_error));
  if (!stream) {
    announce_waiting(source, index, QStringLiteral("the camera did not open"));
    return;
  }
  source.device = *device;
  source.stream = std::move(stream);
  source.waiting_announced = false;
  emit status(index, QStringLiteral("camera"), device->description);
}

void MediaInputs::announce_waiting(Source& source, int index, const QString& reason) {
  if (source.waiting_announced) return;
  source.waiting_announced = true;
  emit status(index, QStringLiteral("waiting"), reason);
}

void MediaInputs::close_camera(Source& source, const QString& reason) {
  if (source.stream) {
    source.stream->stop();
    source.stream.reset();
  }
  announce_waiting(source, source.index_in(sources_), reason);
}

void MediaInputs::reopen_missing_cameras() {
  const QList<CameraDevice> devices = backend_->inputs();
  for (auto& source : sources_) {
    if (source->spec.kind != MediaSpec::Kind::Camera) continue;
    if (source->live()) {
      // The device list is the ground truth: a camera that no longer
      // appears on it is gone even if the stream has not reported an error
      // (macOS can retire a device without a QCamera error).
      const bool still_listed = std::any_of(devices.begin(), devices.end(),
                                            [&](const CameraDevice& device) {
                                              return device.id == source->device.id;
                                            });
      if (still_listed) continue;
      close_camera(*source, QStringLiteral("the camera left"));
    }
    // Frame callbacks capture `source`, which outlives the stream: the old
    // stream is already destroyed above, so the reopen cannot double-feed.
    open_camera(*source);
  }
}

void MediaInputs::accept_frame(Source& source, const QVideoFrame& frame) {
  if (!frame.isValid()) return;
  QMutexLocker lock(&source.mutex);
  source.frame = frame;
  ++source.serial;
}

void MediaInputs::set_shared_images(QHash<int, QImage> images) {
  shared_images_ = std::move(images);
}

void MediaInputs::apply(nm::Backend& backend, nm::Graph& graph, quint64 generation,
                        const nm::EffectRegistry& registry) {
  const auto upload = [&](const MediaBinding& binding, const QImage& image, int source_index,
                          quint64 serial) {
    Uploaded& last = uploaded_[binding.texture_id];
    if (last.source_index != source_index || last.serial != serial) {
      nm::ExternalTextureOptions options;
      options.flipY = false;
      backend.updateTextureFromSource(binding.texture_id, image, options);
      last.source_index = source_index;
      last.serial = serial;
    }
    if (last.size == image.size() && last.generation == generation) return;
    const QJsonObject values{
        {QStringLiteral("step_%1").arg(binding.step_index),
         QJsonObject{{QStringLiteral("imageSize"), QJsonArray{image.width(), image.height()}}}}};
    backend.applyStepParameterValues(graph, registry, values);
    last.size = image.size();
    last.generation = generation;
  };
  QStringList local_textures;
  for (const auto& binding : bind_media(nm::Backend::externalTextureIds(graph), 1)) {
    const auto image = shared_images_.constFind(binding.step_index);
    if (image != shared_images_.constEnd()) {
      upload(binding, *image, -2, static_cast<quint64>(image->cacheKey()));
    } else {
      local_textures.push_back(binding.texture_id);
      if (uploaded_.value(binding.texture_id).source_index == -2) {
        QImage transparent(1, 1, QImage::Format_RGBA8888);
        transparent.fill(Qt::transparent);
        upload(binding, transparent, -3, 1);
      }
    }
  }
  const std::vector<MediaBinding> bindings =
      bind_media(local_textures, static_cast<int>(sources_.size()));
  for (const MediaBinding& binding : bindings) {
    Source& source = *sources_.at(static_cast<std::size_t>(binding.source_index));
    const QImage* image = nullptr;
    quint64 serial = 0;
    if (!source.still.isNull()) {
      image = &source.still;
      serial = 1;
    } else {
      QVideoFrame frame;
      {
        QMutexLocker lock(&source.mutex);
        frame = source.frame;
        serial = source.serial;
      }
      if (serial == 0) continue;  // nothing captured yet
      if (source.converted_serial != serial) {
        source.converted = frame.toImage().convertToFormat(QImage::Format_RGBA8888);
        source.converted_serial = serial;
      }
      if (source.converted.isNull()) continue;
      image = &source.converted;
    }

    // The program text may carry the controller's own imageSize (Noisedeck
    // stores it in the step); the local source's real size wins, and has to
    // be re-applied to every newly compiled graph.
    upload(binding, *image, binding.source_index, serial);
  }
}

}  // namespace noisefactor::sync::render_helper
