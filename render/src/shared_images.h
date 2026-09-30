#pragma once

#include <QHash>
#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUrl>

class QNetworkReply;
class QImageReader;

namespace noisefactor::sync::render_helper {

// Stages a complete image set for one session. Callers keep their current
// graph until ready; failed or superseded requests never expose partial sets.
class SharedImages final : public QObject {
  Q_OBJECT
 public:
  struct Options { QUrl server; QString session_id; QString origin; };
  using Decoder = QImage (*)(QImageReader&);
  explicit SharedImages(Options options, QObject* parent = nullptr, Decoder decoder = nullptr);
  ~SharedImages() override;
  void request(const QStringList& ids, const QString& anon_token);
  void cancel();
  [[nodiscard]] auto images() const -> QHash<QString, QImage>;

 signals:
  void ready();
  void failed(const QString& error);

 private:
  struct Asset { QImage image; qint64 bytes = 0; };
  void next();
  void receive();
  void finish();
  void fail(const QString& error);
  Options options_;
  Decoder decoder_;
  QNetworkAccessManager network_;
  QPointer<QNetworkReply> reply_;
  QHash<QString, Asset> cache_;
  QStringList pending_;
  QString token_, current_id_;
  QByteArray download_;
  qint64 bytes_ = 0;
};
}  // namespace noisefactor::sync::render_helper
