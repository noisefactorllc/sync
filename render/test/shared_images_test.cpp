#include "test_harness.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>

#include <functional>

#include "shared_images.h"

namespace {
using namespace noisefactor::sync::render_helper;

auto wait_until(const std::function<bool()>& condition) -> bool {
  QElapsedTimer clock;
  clock.start();
  while (!condition()) {
    if (clock.elapsed() > 5000) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
  return true;
}

auto png(int width = 96, int height = 32) -> QByteArray {
  QImage image(width, height, QImage::Format_RGBA8888);
  image.fill(QColor(190, 30, 70, 127));
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  SYNC_REQUIRE(image.save(&buffer, "PNG"));
  return bytes;
}

auto image_id(const QByteArray& bytes) -> QString {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

class ImageServer {
 public:
  ImageServer() {
    SYNC_REQUIRE(server.listen(QHostAddress::LocalHost, 0));
    QObject::connect(&server, &QTcpServer::newConnection, [&] {
      while (auto* socket = server.nextPendingConnection()) {
        QObject::connect(socket, &QTcpSocket::readyRead, &server, [&, socket] {
          buffers[socket] += socket->readAll();
          if (!buffers[socket].contains("\r\n\r\n")) return;
          requests.push_back(buffers.take(socket));
          QObject::disconnect(socket, &QTcpSocket::readyRead, &server, nullptr);
          if (hold) held.push_back(socket);
          else respond(socket);
        });
      }
    });
  }
  auto url() const -> QUrl { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())); }
  void respond(QTcpSocket* socket) {
    socket->write("HTTP/1.1 " + QByteArray::number(status) + " response\r\nContent-Type: " + mime
        + "\r\nContent-Length: " + QByteArray::number(length >= 0 ? length : body.size())
        + "\r\nConnection: close\r\n" + headers + "\r\n" + body);
    socket->disconnectFromHost();
  }
  QTcpServer server;
  QHash<QTcpSocket*, QByteArray> buffers;
  QList<QByteArray> requests;
  QList<QTcpSocket*> held;
  QByteArray body = png(), mime = "image/png", headers;
  qint64 length = -1;
  int status = 200;
  bool hold = false;
};

struct Harness {
  ImageServer server;
  SharedImages images{{server.url(), QStringLiteral("Ab12Cd"), QStringLiteral("https://sync.noisedeck.app")}};
  int ready = 0;
  QString error;
  Harness() {
    QObject::connect(&images, &SharedImages::ready, [&] { ++ready; });
    QObject::connect(&images, &SharedImages::failed, [&](const QString& message) { error = message; });
  }
  void request(const QString& id) { images.request({id}, QStringLiteral("anon-secret")); }
};

SYNC_TEST(shared_images_authenticate_and_retain_native_pixels_without_repeat_fetches) {
  Harness h;
  const auto id = image_id(h.server.body);
  h.request(id);
  SYNC_REQUIRE(wait_until([&] { return h.ready > 0 || !h.error.isEmpty(); }));
  SYNC_REQUIRE(h.error.isEmpty());
  SYNC_REQUIRE(h.images.images().value(id).size() == QSize(96, 32));
  SYNC_REQUIRE(h.images.images().value(id).pixelColor(0, 0) == QColor(190, 30, 70, 127));
  SYNC_REQUIRE(h.server.requests.size() == 1);
  const auto request = h.server.requests.front().toLower();
  SYNC_REQUIRE(request.contains("get /v1/sessions/ab12cd/images/" + id.toLatin1() + " http/1.1"));
  SYNC_REQUIRE(request.contains("origin: https://sync.noisedeck.app"));
  SYNC_REQUIRE(request.contains("x-seance-anon: anon-secret"));
  h.request(id);
  SYNC_REQUIRE(h.ready == 2);
  SYNC_REQUIRE(h.server.requests.size() == 1);
  h.images.request({}, QStringLiteral("anon-secret"));
  SYNC_REQUIRE(h.images.images().isEmpty());
}

SYNC_TEST(shared_images_reject_hash_mime_dimensions_and_redirects) {
  for (int fault = 0; fault < 5; ++fault) {
    Harness h;
    QString id = image_id(h.server.body);
    if (fault == 0) id = QString(64, QLatin1Char('a'));
    if (fault == 1) h.server.mime = "video/mp4";
    if (fault == 2) { h.server.body = png(16385, 1); id = image_id(h.server.body); }
    if (fault == 3) { h.server.status = 302; h.server.headers = "Location: /other\r\n"; }
    if (fault == 4) h.server.length = 8 * 1024 * 1024 + 1;
    h.request(id);
    SYNC_REQUIRE(wait_until([&] { return !h.error.isEmpty(); }));
    SYNC_REQUIRE(h.ready == 0);
    SYNC_REQUIRE(h.images.images().isEmpty());
    SYNC_REQUIRE(h.server.requests.size() == 1);
  }
}

SYNC_TEST(shared_images_cancel_obsolete_requests_and_bound_manifest_count) {
  Harness h;
  h.server.hold = true;
  h.request(image_id(h.server.body));
  SYNC_REQUIRE(wait_until([&] { return !h.server.held.isEmpty(); }));
  h.images.cancel();
  h.server.respond(h.server.held.front());
  h.images.request({}, QStringLiteral("anon-secret"));
  SYNC_REQUIRE(h.ready == 1);
  SYNC_REQUIRE(h.images.images().isEmpty());
  SYNC_REQUIRE(h.error.isEmpty());
  QStringList ids;
  for (int i = 0; i < 33; ++i) ids.push_back(QStringLiteral("%1").arg(i, 64, 16, QLatin1Char('0')));
  h.images.request(ids, QStringLiteral("anon-secret"));
  SYNC_REQUIRE(!h.error.isEmpty());
  SYNC_REQUIRE(h.server.requests.size() == 1);
}

SYNC_TEST(shared_images_decode_gif_webp_and_oriented_jpeg_with_later_xmp) {
  QFile fixture(QStringLiteral(SYNC_RENDER_IMAGE_FIXTURES));
  SYNC_REQUIRE(fixture.open(QIODevice::ReadOnly));
  const auto cases = QJsonDocument::fromJson(fixture.readAll()).array();
  SYNC_REQUIRE(cases.size() == 4);
  for (const auto& item : cases) {
    Harness h;
    const auto spec = item.toObject();
    h.server.body = QByteArray::fromBase64(spec.value(QStringLiteral("base64")).toString().toLatin1());
    h.server.mime = spec.value(QStringLiteral("mimeType")).toString().toLatin1();
    const auto id = image_id(h.server.body);
    h.request(id);
    SYNC_REQUIRE(wait_until([&] { return h.ready > 0 || !h.error.isEmpty(); }));
    SYNC_REQUIRE(h.error.isEmpty());
    SYNC_REQUIRE(h.images.images().value(id).size() == QSize(spec.value(QStringLiteral("width")).toInt(),
                                                          spec.value(QStringLiteral("height")).toInt()));
  }
}

SYNC_TEST(shared_images_enforce_the_aggregate_encoded_budget_before_decoding) {
  Harness h;
  QByteArray large = png();
  large.append(QByteArray(7 * 1024 * 1024, '\0'));
  QStringList ids;
  QHash<QString, QByteArray> bodies;
  for (int i = 0; i < 5; ++i) {
    auto bytes = large + QByteArray::number(i);
    const auto id = image_id(bytes);
    ids.push_back(id); bodies.insert(id, bytes);
  }
  h.server.hold = true;
  h.images.request(ids, QStringLiteral("anon-secret"));
  for (int i = 0; i < 5; ++i) {
    SYNC_REQUIRE(wait_until([&] { return h.server.held.size() > i; }));
    const auto path = h.server.requests.at(i).split(' ').at(1);
    h.server.body = bodies.value(QString::fromLatin1(path.mid(path.lastIndexOf('/') + 1)));
    h.server.respond(h.server.held.at(i));
  }
  SYNC_REQUIRE(wait_until([&] { return !h.error.isEmpty(); }));
  SYNC_REQUIRE(h.ready == 0);
  SYNC_REQUIRE(h.error.contains(QStringLiteral("byte limit")));
}

SYNC_TEST(shared_images_limit_each_raster_independently_and_allow_full_size_qt_decoding) {
  const int previous_limit = QImageReader::allocationLimit();
  const auto restore_limit = qScopeGuard([=] { QImageReader::setAllocationLimit(previous_limit); });
  QImageReader::setAllocationLimit(128);
  ImageServer server;
  server.hold = true;
  server.mime = "image/gif";
  SharedImages images({server.url(), QStringLiteral("Ab12Cd"), QStringLiteral("https://sync.noisedeck.app")},
      nullptr, [](QImageReader& reader) {
        // Exercise real headers and quota checks without allocating the large rasters.
        if (reader.size().width() < 8000) return QImage();
        return QImage(1, 1, QImage::Format_RGBA8888);
      });
  int ready = 0;
  QString error;
  QObject::connect(&images, &SharedImages::ready, [&] { ++ready; });
  QObject::connect(&images, &SharedImages::failed, [&](const QString& message) { error = message; });
  QFile fixture(QStringLiteral(SYNC_RENDER_IMAGE_FIXTURES));
  SYNC_REQUIRE(fixture.open(QIODevice::ReadOnly));
  const auto spec = QJsonDocument::fromJson(fixture.readAll()).array().first().toObject();
  const auto gif = QByteArray::fromBase64(spec.value(QStringLiteral("base64")).toString().toLatin1());
  const auto header = [&](int width, int height) {
    auto bytes = gif;
    bytes[6] = static_cast<char>(width & 255); bytes[7] = static_cast<char>(width >> 8);
    bytes[8] = static_cast<char>(height & 255); bytes[9] = static_cast<char>(height >> 8);
    const auto descriptor = bytes.indexOf(',');
    SYNC_REQUIRE(descriptor > 0);
    bytes[descriptor + 5] = bytes[6]; bytes[descriptor + 6] = bytes[7];
    bytes[descriptor + 7] = bytes[8]; bytes[descriptor + 8] = bytes[9];
    return bytes;
  };
  QHash<QString, QByteArray> bodies;
  QStringList ids;
  for (const auto& bytes : {header(8000, 5000), header(10000, 4000), header(8000, 8000)}) {
    const auto id = image_id(bytes);
    ids.push_back(id); bodies.insert(id, bytes);
  }
  images.request(ids, QStringLiteral("anon-secret"));
  for (int i = 0; i < ids.size(); ++i) {
    SYNC_REQUIRE(wait_until([&] { return server.held.size() > i || !error.isEmpty(); }));
    SYNC_REQUIRE(error.isEmpty());
    const auto path = server.requests.at(i).split(' ').at(1);
    server.body = bodies.value(QString::fromLatin1(path.mid(path.lastIndexOf('/') + 1)));
    server.respond(server.held.at(i));
  }
  SYNC_REQUIRE(wait_until([&] { return ready > 0 || !error.isEmpty(); }));
  SYNC_REQUIRE(error.isEmpty());
  SYNC_REQUIRE(images.images().size() == 3);
  // A 16-bit RGBA PNG can decode to eight bytes per pixel before conversion.
  SYNC_REQUIRE(QImageReader::allocationLimit() == 0
      || qint64{QImageReader::allocationLimit()} * 1024 * 1024 >= qint64{64000000} * 8);

  for (const auto& bytes : {header(8000, 8001), header(16385, 1)}) {
    const int previous_requests = server.held.size();
    images.request({image_id(bytes)}, QStringLiteral("anon-secret"));
    SYNC_REQUIRE(wait_until([&] { return server.held.size() > previous_requests; }));
    server.body = bytes;
    server.respond(server.held.back());
    SYNC_REQUIRE(wait_until([&] { return !error.isEmpty(); }));
    SYNC_REQUIRE(error.contains(QStringLiteral("raster dimensions")));
    error.clear();
  }
}
}  // namespace
