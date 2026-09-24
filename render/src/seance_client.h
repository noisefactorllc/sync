#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <optional>

#include "seance_document.h"

namespace noisefactor::sync::render_helper {

// Accepts a bare session id ("Ab12Cd") or a share link that carries one in
// its ?seance= parameter, the form every controller hands out.
[[nodiscard]] auto parse_session_id(const QString& input) -> std::optional<QString>;

// GET /v1/sessions/{id}/ws on the server's own origin, ws/wss matching
// http/https (seance protocol.md section 1).
[[nodiscard]] auto session_socket_url(const QUrl& server, const QString& session_id) -> QUrl;

// A read-only seance participant. It joins, adopts the session snapshot,
// follows the program document's edits, and never sends a write: the only
// frames it originates are hello and session-state (a resync request), both
// allowed to read-only connections (protocol.md section 3, "all").
class SeanceClient final : public QObject {
  Q_OBJECT

 public:
  enum class Status { Offline, Connecting, Online, Stopped };
  Q_ENUM(Status)

  struct Options {
    QUrl server;
    QString session_id;
    // The Origin header. Seance allowlists origins exactly, so this must be
    // one the server was configured to accept.
    QString origin;
    // Reused across reconnects so the helper keeps one identity in the
    // roster (protocol.md section 11). Empty lets the server mint one.
    QString anon_token;
    int reconnect_base_ms = 500;
    int reconnect_max_ms = 8000;
    // A socket that has delivered nothing, not even a pong, for this long is
    // presumed half-open and dropped so the reconnect path can recover it.
    int silence_timeout_ms = 60000;
    int ping_interval_ms = 20000;
  };

  explicit SeanceClient(Options options, QObject* parent = nullptr);
  ~SeanceClient() override;

  void start();
  void stop();

  [[nodiscard]] auto status() const -> Status { return status_; }
  [[nodiscard]] auto document() const -> const SeanceDocument& { return document_; }
  [[nodiscard]] auto anon_token() const -> const QString& { return options_.anon_token; }

 signals:
  // The program text changed: a snapshot, a resync, or an applied edit.
  void program_changed(const QString& text, qint64 rev);
  void status_changed(noisefactor::sync::render_helper::SeanceClient::Status status,
                      const QString& detail);
  // The session refused or ended the connection for a reason reconnecting
  // cannot fix (kicked, banned, locked, unknown, dialect). No retry follows.
  void stopped(int close_code, const QString& reason);
  // A seance error frame, for diagnostics only.
  void server_error(const QString& code, const QString& detail);
  // A fresh identity the caller may persist for the next run.
  void anon_token_changed(const QString& token);

 private:
  void open_socket();
  void on_connected();
  void on_disconnected();
  void on_text(const QString& message);
  void handle(const QJsonObject& frame);
  void send(const QJsonObject& frame);
  void request_resync();
  void schedule_reconnect(const QString& detail);
  void set_status(Status status, const QString& detail);
  void check_silence();

  Options options_;
  QWebSocket socket_;
  SeanceDocument document_;
  Status status_ = Status::Offline;
  QTimer reconnect_timer_;
  QTimer ping_timer_;
  QElapsedTimer last_heard_;
  int attempt_ = 0;
  bool stopping_ = false;
  bool resync_pending_ = false;
  qint64 last_emitted_rev_ = -2;
  QString last_emitted_text_;
};

}  // namespace noisefactor::sync::render_helper
