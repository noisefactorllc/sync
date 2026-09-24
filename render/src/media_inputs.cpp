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
  std::unique_ptr<QCamera> camera;
  std::unique_ptr<QMediaCaptureSession> session;
  std::unique_ptr<QMediaPlayer> player;

  // Written by the sink, which may call from a media thread; read by apply()
  // on the render thread.
  QMutex mutex;
  QVideoFrame frame;
  quint64 serial = 0;

  // The newest frame as RGBA8, converted once however many steps share it.
  QImage converted;
  quint64 converted_serial = 0;
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

MediaInputs::MediaInputs(QList<MediaSpec> specs, QObject* parent)
    : QObject(parent), specs_(std::move(specs)) {}

MediaInputs::~MediaInputs() {
  for (auto& source : sources_) {
    if (source->camera) source->camera->stop();
    if (source->player) source->player->stop();
  }
}

auto MediaInputs::start(QString& error) -> bool {
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
    s.sink = std::make_unique<QVideoSink>();
    connect(s.sink.get(), &QVideoSink::videoFrameChanged, this,
            [this, &s](const QVideoFrame& frame) { accept_frame(s, frame); },
            Qt::DirectConnection);
    const QCameraPermission permission;
    switch (qApp->checkPermission(permission)) {
      case Qt::PermissionStatus::Granted:
        start_camera(s);
        break;
      case Qt::PermissionStatus::Denied:
        emit status(index, QStringLiteral("denied"), QStringLiteral("camera permission denied"));
        break;
      case Qt::PermissionStatus::Undetermined:
        emit status(index, QStringLiteral("waiting"), QStringLiteral("camera permission requested"));
        qApp->requestPermission(permission, this, [this, &s, index](const QPermission& result) {
          if (result.status() == Qt::PermissionStatus::Granted) {
            start_camera(s);
          } else {
            emit status(index, QStringLiteral("denied"), QStringLiteral("camera permission denied"));
          }
        });
        break;
    }
  }
  return true;
}

void MediaInputs::start_camera(Source& source) {
  const int index = static_cast<int>(std::find_if(sources_.begin(), sources_.end(),
                                                  [&](const auto& s) { return s.get() == &source; }) -
                                     sources_.begin());
  const QList<QCameraDevice> devices = QMediaDevices::videoInputs();
  QCameraDevice device = QMediaDevices::defaultVideoInput();
  const QString wanted = source.spec.value;
  if (!wanted.isEmpty()) {
    bool numeric = false;
    const int position = wanted.toInt(&numeric);
    device = QCameraDevice();
    if (numeric && position >= 0 && position < devices.size()) {
      device = devices.at(position);
    } else {
      for (const QCameraDevice& candidate : devices) {
        if (candidate.description().contains(wanted, Qt::CaseInsensitive)) {
          device = candidate;
          break;
        }
      }
    }
  }
  if (device.isNull()) {
    emit status(index, QStringLiteral("error"),
                wanted.isEmpty() ? QStringLiteral("no camera") : QStringLiteral("no camera matches %1").arg(wanted));
    return;
  }
  source.camera = std::make_unique<QCamera>(device);
  source.session = std::make_unique<QMediaCaptureSession>();
  source.session->setCamera(source.camera.get());
  source.session->setVideoSink(source.sink.get());
  connect(source.camera.get(), &QCamera::errorOccurred, this,
          [this, index](QCamera::Error, const QString& message) {
            emit status(index, QStringLiteral("error"), message);
          });
  source.camera->start();
  emit status(index, QStringLiteral("camera"), device.description());
}

void MediaInputs::accept_frame(Source& source, const QVideoFrame& frame) {
  if (!frame.isValid()) return;
  QMutexLocker lock(&source.mutex);
  source.frame = frame;
  ++source.serial;
}

void MediaInputs::apply(nm::Backend& backend, nm::Graph& graph, quint64 generation,
                        const nm::EffectRegistry& registry) {
  if (sources_.empty()) return;
  const std::vector<MediaBinding> bindings =
      bind_media(nm::Backend::externalTextureIds(graph), static_cast<int>(sources_.size()));
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

    Uploaded& last = uploaded_[binding.texture_id];
    if (last.source_index != binding.source_index || last.serial != serial) {
      nm::ExternalTextureOptions options;
      options.flipY = false;
      backend.updateTextureFromSource(binding.texture_id, *image, options);
      last.source_index = binding.source_index;
      last.serial = serial;
    }
    if (last.size == image->size() && last.generation == generation) continue;
    // The program text may carry the controller's own imageSize (Noisedeck
    // stores it in the step); the local source's real size wins, and has to
    // be re-applied to every newly compiled graph.
    const QJsonObject values{
        {QStringLiteral("step_%1").arg(binding.step_index),
         QJsonObject{{QStringLiteral("imageSize"),
                      QJsonArray{image->width(), image->height()}}}}};
    backend.applyStepParameterValues(graph, registry, values);
    last.size = image->size();
    last.generation = generation;
  }
}

}  // namespace noisefactor::sync::render_helper
