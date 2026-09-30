#include "shared_images.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QImageReader>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>

#include <utility>

namespace noisefactor::sync::render_helper {
namespace {
constexpr qint64 kImageBytes = 8 * 1024 * 1024;
constexpr qint64 kSessionBytes = 32 * 1024 * 1024;
constexpr qint64 kRasterPixels = 64000000;
constexpr int kAxis = 16384;
}

SharedImages::SharedImages(Options options, QObject* parent, Decoder decoder)
    : QObject(parent), options_(std::move(options)), decoder_(decoder), network_(this) {
  // The format may decode to 16-bit RGBA before conversion. Qt's default
  // allocation ceiling is smaller than Seance's 64M-pixel per-image limit.
  const int allocation_limit = QImageReader::allocationLimit();
  if (allocation_limit > 0 && allocation_limit < 512) QImageReader::setAllocationLimit(512);
}

SharedImages::~SharedImages() { cancel(); }

void SharedImages::cancel() {
  if (reply_) {
    auto* reply = reply_.data();
    reply_ = nullptr;
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }
  pending_.clear();
  download_.clear();
  current_id_.clear();
}

auto SharedImages::images() const -> QHash<QString, QImage> {
  QHash<QString, QImage> result;
  for (auto it = cache_.constBegin(); it != cache_.constEnd(); ++it) result.insert(it.key(), it->image);
  return result;
}

void SharedImages::request(const QStringList& ids, const QString& anon_token) {
  cancel();
  const QSet<QString> unique(ids.begin(), ids.end());
  static const QRegularExpression image_pattern(QStringLiteral("^[a-f0-9]{64}$"));
  static const QRegularExpression session_pattern(QStringLiteral("^[A-Za-z0-9]{1,64}$"));
  if (unique.size() > 32) { fail(QStringLiteral("shared images exceed the 32 image limit")); return; }
  for (const auto& id : unique) {
    if (!image_pattern.match(id).hasMatch()) { fail(QStringLiteral("invalid shared image reference")); return; }
  }
  if (!unique.isEmpty() && (anon_token.isEmpty() || !session_pattern.match(options_.session_id).hasMatch()
      || options_.origin.isEmpty() || options_.server.host().isEmpty()
      || (options_.server.scheme() != QStringLiteral("http") && options_.server.scheme() != QStringLiteral("https")))) {
    fail(QStringLiteral("shared images require an authenticated Seance session"));
    return;
  }
  bytes_ = 0;
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (!unique.contains(it.key())) { it = cache_.erase(it); continue; }
    bytes_ += it->bytes;
    ++it;
  }
  for (const auto& id : unique) if (!cache_.contains(id)) pending_.push_back(id);
  pending_.sort();
  token_ = anon_token;
  next();
}

void SharedImages::next() {
  if (pending_.isEmpty()) { emit ready(); return; }
  current_id_ = pending_.takeFirst();
  download_.clear();
  QUrl url = options_.server;
  url.setPath(QStringLiteral("/v1/sessions/%1/images/%2").arg(options_.session_id, current_id_));
  url.setQuery(QString());
  url.setFragment(QString());
  url.setUserInfo(QString());
  QNetworkRequest request(url);
  request.setRawHeader("Origin", options_.origin.toUtf8());
  request.setRawHeader("X-Seance-Anon", token_.toUtf8());
  request.setRawHeader("User-Agent", "sync-render");
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  request.setTransferTimeout(15000);
  reply_ = network_.get(request);
  reply_->setReadBufferSize(kImageBytes + 1);
  connect(reply_, &QNetworkReply::metaDataChanged, this, &SharedImages::receive);
  connect(reply_, &QNetworkReply::readyRead, this, &SharedImages::receive);
  connect(reply_, &QNetworkReply::finished, this, &SharedImages::finish);
}

void SharedImages::receive() {
  if (!reply_) return;
  const qint64 length = reply_->header(QNetworkRequest::ContentLengthHeader).toLongLong();
  if (length > kImageBytes || bytes_ + length > kSessionBytes) {
    fail(QStringLiteral("shared images exceed the encoded byte limit"));
    return;
  }
  download_ += reply_->read(kImageBytes - download_.size() + 1);
  if (download_.size() > kImageBytes || bytes_ + download_.size() > kSessionBytes) {
    fail(QStringLiteral("shared images exceed the encoded byte limit"));
  }
}

void SharedImages::finish() {
  if (!reply_) return;
  receive();
  if (!reply_) return;
  const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (reply_->error() != QNetworkReply::NoError || status != 200) {
    fail(QStringLiteral("shared image request failed (HTTP %1)").arg(status));
    return;
  }
  const QString mime = reply_->header(QNetworkRequest::ContentTypeHeader).toString()
      .section(QLatin1Char(';'), 0, 0).trimmed().toLower();
  const QHash<QString, QByteArray> formats{{QStringLiteral("image/png"), "png"},
      {QStringLiteral("image/jpeg"), "jpeg"}, {QStringLiteral("image/gif"), "gif"},
      {QStringLiteral("image/webp"), "webp"}};
  if (!formats.contains(mime)) { fail(QStringLiteral("unsupported shared image MIME type")); return; }
  const auto hash = QString::fromLatin1(QCryptographicHash::hash(download_, QCryptographicHash::Sha256).toHex());
  if (hash != current_id_) { fail(QStringLiteral("shared image hash does not match its reference")); return; }
  QBuffer buffer(&download_);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer);
  reader.setDecideFormatFromContent(true);
  reader.setAutoTransform(true);
  const QSize size = reader.size();
  const qint64 pixels = static_cast<qint64>(size.width()) * size.height();
  if (reader.format() != formats.value(mime) || size.width() < 1 || size.height() < 1
      || size.width() > kAxis || size.height() > kAxis || pixels > kRasterPixels) {
    fail(QStringLiteral("shared image format or raster dimensions are invalid"));
    return;
  }
  QImage image = (decoder_ ? decoder_(reader) : reader.read()).convertToFormat(QImage::Format_RGBA8888);
  if (image.isNull()) { fail(QStringLiteral("shared image decoding failed")); return; }
  cache_.insert(current_id_, {image, download_.size()});
  bytes_ += download_.size();
  auto* complete = reply_.data();
  reply_ = nullptr;
  complete->deleteLater();
  download_.clear();
  next();
}

void SharedImages::fail(const QString& error) {
  cancel();
  emit failed(error);
}
}  // namespace noisefactor::sync::render_helper
