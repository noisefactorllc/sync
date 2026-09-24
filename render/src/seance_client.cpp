#include "seance_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace noisefactor::sync::render_helper {

namespace {

// Close codes after which reconnecting cannot succeed or is not allowed
// (protocol.md section 7). 4401 is a kick: the protocol says a client must not
// rejoin on its own.
[[nodiscard]] auto terminal(int code) -> bool {
  switch (code) {
    case 4400:  // protocol violation
    case 4401:  // kicked
    case 4403:  // banned, guests off, unauthorized
    case 4404:  // unknown session
    case 4409:  // dialect mismatch
    case 4423:  // locked
      return true;
    default:
      return false;
  }
}

[[nodiscard]] auto valid_session_id(const QString& id) -> bool {
  static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9]{1,64}$"));
  return pattern.match(id).hasMatch();
}

}  // namespace

auto parse_session_id(const QString& input) -> std::optional<QString> {
  const QString trimmed = input.trimmed();
  if (valid_session_id(trimmed)) return trimmed;
  const QUrl url(trimmed, QUrl::StrictMode);
  if (!url.isValid() || url.scheme().isEmpty()) return std::nullopt;
  const QString id = QUrlQuery(url).queryItemValue(QStringLiteral("seance"));
  if (valid_session_id(id)) return id;
  return std::nullopt;
}

auto session_socket_url(const QUrl& server, const QString& session_id) -> QUrl {
  QUrl url(server);
  url.setScheme(server.scheme() == QStringLiteral("http") ? QStringLiteral("ws")
                                                          : QStringLiteral("wss"));
  url.setPath(QStringLiteral("/v1/sessions/%1/ws").arg(session_id));
  url.setQuery(QString());
  url.setFragment(QString());
  return url;
}

SeanceClient::SeanceClient(Options options, QObject* parent)
    : QObject(parent), options_(std::move(options)), socket_(options_.origin) {
  reconnect_timer_.setSingleShot(true);
  connect(&reconnect_timer_, &QTimer::timeout, this, &SeanceClient::open_socket);
  ping_timer_.setInterval(options_.ping_interval_ms);
  connect(&ping_timer_, &QTimer::timeout, this, [this] {
    check_silence();
    if (socket_.state() == QAbstractSocket::ConnectedState) socket_.ping();
  });
  connect(&socket_, &QWebSocket::connected, this, &SeanceClient::on_connected);
  connect(&socket_, &QWebSocket::disconnected, this, &SeanceClient::on_disconnected);
  connect(&socket_, &QWebSocket::textMessageReceived, this, &SeanceClient::on_text);
  connect(&socket_, &QWebSocket::pong, this, [this] { last_heard_.restart(); });
  // A binary frame is a protocol violation from either side; ignore it rather
  // than guess at its meaning.
}

SeanceClient::~SeanceClient() {
  stopping_ = true;
  socket_.abort();
}

void SeanceClient::start() {
  stopping_ = false;
  attempt_ = 0;
  open_socket();
}

void SeanceClient::stop() {
  stopping_ = true;
  reconnect_timer_.stop();
  ping_timer_.stop();
  // Abort rather than close: a graceful close defers the server-observed
  // leave by the transport teardown window (protocol.md section 11).
  socket_.abort();
  set_status(Status::Stopped, QStringLiteral("stopped"));
}

void SeanceClient::open_socket() {
  if (stopping_) return;
  set_status(Status::Connecting, QString());
  resync_pending_ = false;
  QNetworkRequest request(session_socket_url(options_.server, options_.session_id));
  request.setRawHeader("User-Agent", "sync-render");
  socket_.open(request);
}

void SeanceClient::on_connected() {
  last_heard_.restart();
  ping_timer_.start();
  QJsonObject hello{{QStringLiteral("type"), QStringLiteral("hello")},
                    {QStringLiteral("protocol"), 1},
                    {QStringLiteral("dialects"), QJsonArray{QStringLiteral("noisemaker-dsl")}}};
  if (!options_.anon_token.isEmpty()) {
    hello.insert(QStringLiteral("anon_token"), options_.anon_token);
  }
  send(hello);
}

void SeanceClient::on_disconnected() {
  ping_timer_.stop();
  if (stopping_) return;
  const int code = static_cast<int>(socket_.closeCode());
  const QString reason = socket_.closeReason();
  if (terminal(code)) {
    stopping_ = true;
    set_status(Status::Stopped, QStringLiteral("closed %1 %2").arg(code).arg(reason));
    emit stopped(code, reason);
    return;
  }
  schedule_reconnect(QStringLiteral("closed %1 %2").arg(code).arg(reason));
}

void SeanceClient::schedule_reconnect(const QString& detail) {
  // Exponential backoff with jitter so a server restart does not bring every
  // helper back in the same instant.
  const int shift = std::min(attempt_, 16);
  const qint64 base = std::min<qint64>(static_cast<qint64>(options_.reconnect_base_ms) << shift,
                                       options_.reconnect_max_ms);
  const double jitter = 0.75 + 0.5 * QRandomGenerator::global()->generateDouble();
  ++attempt_;
  set_status(Status::Offline, detail);
  reconnect_timer_.start(static_cast<int>(static_cast<double>(base) * jitter));
}

void SeanceClient::check_silence() {
  if (socket_.state() != QAbstractSocket::ConnectedState) return;
  if (last_heard_.isValid() && last_heard_.elapsed() > options_.silence_timeout_ms) {
    socket_.abort();
  }
}

void SeanceClient::on_text(const QString& message) {
  last_heard_.restart();
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    emit server_error(QStringLiteral("client_bad_frame"), error.errorString());
    return;
  }
  handle(document.object());
}

void SeanceClient::handle(const QJsonObject& frame) {
  const QString type = frame.value(QStringLiteral("type")).toString();
  bool text_may_have_changed = false;

  if (type == QStringLiteral("welcome")) {
    const QString token = frame.value(QStringLiteral("anon_token")).toString();
    if (!token.isEmpty() && token != options_.anon_token) {
      options_.anon_token = token;
      emit anon_token_changed(token);
    }
  } else if (type == QStringLiteral("session-snapshot")) {
    resync_pending_ = false;
    attempt_ = 0;
    document_.adopt(frame.value(QStringLiteral("docs")).toArray());
    text_may_have_changed = true;
    set_status(Status::Online, document_.has_document()
                                   ? QStringLiteral("document %1").arg(document_.doc_id())
                                   : QStringLiteral("no program document"));
  } else if (type == QStringLiteral("doc-snapshot")) {
    document_.adopt(frame.value(QStringLiteral("docs")).toArray());
    text_may_have_changed = true;
  } else if (type == QStringLiteral("doc-edit")) {
    switch (document_.apply(frame)) {
      case SeanceDocument::Edit::Applied:
        text_may_have_changed = true;
        break;
      case SeanceDocument::Edit::Ignored:
        break;
      case SeanceDocument::Edit::NeedsResync:
        request_resync();
        break;
    }
  } else if (type == QStringLiteral("error")) {
    emit server_error(frame.value(QStringLiteral("code")).toString(),
                      frame.value(QStringLiteral("detail")).toString());
  }
  // Everything else (chat, cursors, roster, poly-*, state/data lanes) is
  // presence or content this helper does not render.

  if (text_may_have_changed && !resync_pending_) {
    const qint64 rev = document_.rev();
    const QString& text = document_.text();
    if (rev != last_emitted_rev_ || text != last_emitted_text_) {
      last_emitted_rev_ = rev;
      last_emitted_text_ = text;
      emit program_changed(text, rev);
    }
  }
}

void SeanceClient::request_resync() {
  if (resync_pending_) return;
  resync_pending_ = true;
  send(QJsonObject{{QStringLiteral("type"), QStringLiteral("session-state")}});
}

void SeanceClient::send(const QJsonObject& frame) {
  socket_.sendTextMessage(QString::fromUtf8(QJsonDocument(frame).toJson(QJsonDocument::Compact)));
}

void SeanceClient::set_status(Status status, const QString& detail) {
  if (status == status_ && detail.isEmpty()) return;
  status_ = status;
  emit status_changed(status, detail);
}

}  // namespace noisefactor::sync::render_helper
