#include "test_harness.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QWebSocket>
#include <QWebSocketServer>

#include <functional>
#include <memory>
#include <vector>

#include "seance_client.h"

namespace {

using noisefactor::sync::render_helper::SeanceClient;

// Runs the event loop until pred holds. A condition wait, not a sleep: it
// returns the moment the condition is met and fails the test only if it
// never is.
[[nodiscard]] auto wait_until(const std::function<bool()>& pred, int timeout_ms = 5000) -> bool {
  QElapsedTimer timer;
  timer.start();
  while (!pred()) {
    if (timer.elapsed() > timeout_ms) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
  return true;
}

// The server half of the seance protocol, reduced to what a read-only
// participant exercises.
class FakeSeance {
 public:
  struct Peer {
    std::unique_ptr<QWebSocket> socket;
    QString origin;
    QString path;
    std::vector<QJsonObject> received;
  };

  FakeSeance() : server_(QStringLiteral("fake-seance"), QWebSocketServer::NonSecureMode) {
    SYNC_REQUIRE(server_.listen(QHostAddress::LocalHost, 0));
    QObject::connect(&server_, &QWebSocketServer::newConnection, [this] {
      while (QWebSocket* socket = server_.nextPendingConnection()) {
        auto peer = std::make_unique<Peer>();
        peer->origin = socket->origin();
        peer->path = socket->requestUrl().path();
        peer->socket.reset(socket);
        Peer* raw = peer.get();
        QObject::connect(socket, &QWebSocket::textMessageReceived, [raw](const QString& text) {
          raw->received.push_back(QJsonDocument::fromJson(text.toUtf8()).object());
        });
        peers.push_back(std::move(peer));
      }
    });
  }

  [[nodiscard]] auto url() const -> QUrl {
    return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort()));
  }

  void send(std::size_t peer, const QJsonObject& frame) {
    peers.at(peer)->socket->sendTextMessage(
        QString::fromUtf8(QJsonDocument(frame).toJson(QJsonDocument::Compact)));
  }

  void welcome_and_snapshot(std::size_t peer, const QString& text, int rev,
                            const QString& token = QStringLiteral("anon-1")) {
    send(peer, {{QStringLiteral("type"), QStringLiteral("welcome")},
                {QStringLiteral("protocol"), 1},
                {QStringLiteral("anon_token"), token}});
    send(peer, {{QStringLiteral("type"), QStringLiteral("session-snapshot")},
                {QStringLiteral("docs"),
                 QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("main")},
                                        {QStringLiteral("kind"), QStringLiteral("noisemaker-dsl")},
                                        {QStringLiteral("rev"), rev},
                                        {QStringLiteral("text"), text},
                                        {QStringLiteral("default"), true}}}}});
  }

  std::vector<std::unique_ptr<Peer>> peers;

 private:
  QWebSocketServer server_;
};

[[nodiscard]] auto doc_edit(int rev, int start, int end, const QString& text) -> QJsonObject {
  return {{QStringLiteral("type"), QStringLiteral("doc-edit")},
          {QStringLiteral("docId"), QStringLiteral("main")},
          {QStringLiteral("rev"), rev},
          {QStringLiteral("authorSeq"), 3},
          {QStringLiteral("edit"), QJsonObject{{QStringLiteral("start"), start},
                                               {QStringLiteral("end"), end},
                                               {QStringLiteral("text"), text}}}};
}

struct Harness {
  explicit Harness(FakeSeance& server) {
    SeanceClient::Options options;
    options.server = server.url();
    options.session_id = QStringLiteral("Ab12Cd");
    options.origin = QStringLiteral("https://sync.noisedeck.app");
    options.reconnect_base_ms = 20;
    options.reconnect_max_ms = 40;
    client = std::make_unique<SeanceClient>(options);
    QObject::connect(client.get(), &SeanceClient::program_changed,
                     [this](const QString& text, qint64 rev) {
                       programs.push_back(text);
                       revs.push_back(rev);
                     });
    QObject::connect(client.get(), &SeanceClient::stopped,
                     [this](int code, const QString&) { stopped_code = code; });
    QObject::connect(client.get(), &SeanceClient::anon_token_changed,
                     [this](const QString& token) { tokens.push_back(token); });
  }
  std::unique_ptr<SeanceClient> client;
  std::vector<QString> programs;
  std::vector<qint64> revs;
  std::vector<QString> tokens;
  int stopped_code = 0;
};

SYNC_TEST(joining_presents_the_origin_and_a_read_only_hello) {
  FakeSeance server;
  Harness harness(server);
  harness.client->start();
  SYNC_REQUIRE(wait_until([&] { return !server.peers.empty() && !server.peers[0]->received.empty(); }));
  const auto& peer = *server.peers[0];
  SYNC_REQUIRE(peer.origin == QStringLiteral("https://sync.noisedeck.app"));
  SYNC_REQUIRE(peer.path == QStringLiteral("/v1/sessions/Ab12Cd/ws"));
  const QJsonObject& hello = peer.received[0];
  SYNC_REQUIRE(hello.value(QStringLiteral("type")).toString() == QStringLiteral("hello"));
  SYNC_REQUIRE(hello.value(QStringLiteral("protocol")).toInt() == 1);
  SYNC_REQUIRE(hello.value(QStringLiteral("dialects")).toArray() ==
               QJsonArray{QStringLiteral("noisemaker-dsl")});
  SYNC_REQUIRE(!hello.contains(QStringLiteral("anon_token")));
}

SYNC_TEST(the_snapshot_and_each_edit_reach_the_renderer) {
  FakeSeance server;
  Harness harness(server);
  harness.client->start();
  SYNC_REQUIRE(wait_until([&] { return !server.peers.empty(); }));
  server.welcome_and_snapshot(0, QStringLiteral("perlin().write(o0)"), 4);
  SYNC_REQUIRE(wait_until([&] { return harness.programs.size() == 1; }));
  SYNC_REQUIRE(harness.programs[0] == QStringLiteral("perlin().write(o0)"));
  SYNC_REQUIRE(harness.revs[0] == 4);
  SYNC_REQUIRE(harness.tokens == std::vector<QString>{QStringLiteral("anon-1")});
  SYNC_REQUIRE(harness.client->status() == SeanceClient::Status::Online);

  server.send(0, doc_edit(5, 7, 7, QStringLiteral("scale: 2")));
  SYNC_REQUIRE(wait_until([&] { return harness.programs.size() == 2; }));
  SYNC_REQUIRE(harness.programs[1] == QStringLiteral("perlin(scale: 2).write(o0)"));
  SYNC_REQUIRE(harness.revs[1] == 5);
}

SYNC_TEST(a_missed_edit_is_recovered_from_a_fresh_snapshot) {
  FakeSeance server;
  Harness harness(server);
  harness.client->start();
  SYNC_REQUIRE(wait_until([&] { return !server.peers.empty(); }));
  server.welcome_and_snapshot(0, QStringLiteral("abc"), 1);
  SYNC_REQUIRE(wait_until([&] { return harness.programs.size() == 1; }));

  server.send(0, doc_edit(3, 0, 0, QStringLiteral("x")));  // rev 2 never arrived
  auto& received = server.peers[0]->received;
  SYNC_REQUIRE(wait_until([&] {
    return !received.empty() &&
           received.back().value(QStringLiteral("type")).toString() == QStringLiteral("session-state");
  }));
  // The gapped edit was not applied on top of stale text.
  SYNC_REQUIRE(harness.programs.size() == 1);
  server.send(0, {{QStringLiteral("type"), QStringLiteral("session-snapshot")},
                  {QStringLiteral("docs"),
                   QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("main")},
                                          {QStringLiteral("kind"), QStringLiteral("noisemaker-dsl")},
                                          {QStringLiteral("rev"), 3},
                                          {QStringLiteral("text"), QStringLiteral("xyabc")}}}}});
  SYNC_REQUIRE(wait_until([&] { return harness.programs.size() == 2; }));
  SYNC_REQUIRE(harness.programs[1] == QStringLiteral("xyabc"));
  SYNC_REQUIRE(harness.revs[1] == 3);
}

SYNC_TEST(a_dropped_connection_rejoins_with_the_same_identity) {
  FakeSeance server;
  Harness harness(server);
  harness.client->start();
  SYNC_REQUIRE(wait_until([&] { return !server.peers.empty(); }));
  server.welcome_and_snapshot(0, QStringLiteral("abc"), 1, QStringLiteral("anon-keep"));
  SYNC_REQUIRE(wait_until([&] { return harness.programs.size() == 1; }));
  // Roster full is transient: the client must come back.
  server.peers[0]->socket->close(static_cast<QWebSocketProtocol::CloseCode>(4429),
                                 QStringLiteral("full"));
  SYNC_REQUIRE(wait_until([&] {
    return server.peers.size() == 2 && !server.peers[1]->received.empty();
  }));
  const QJsonObject& hello = server.peers[1]->received[0];
  SYNC_REQUIRE(hello.value(QStringLiteral("anon_token")).toString() == QStringLiteral("anon-keep"));
}

SYNC_TEST(a_kick_ends_the_session_for_good) {
  FakeSeance server;
  Harness harness(server);
  harness.client->start();
  SYNC_REQUIRE(wait_until([&] { return !server.peers.empty(); }));
  server.welcome_and_snapshot(0, QStringLiteral("abc"), 1);
  SYNC_REQUIRE(wait_until([&] { return harness.programs.size() == 1; }));
  server.peers[0]->socket->close(static_cast<QWebSocketProtocol::CloseCode>(4401),
                                 QStringLiteral("kicked"));
  SYNC_REQUIRE(wait_until([&] { return harness.stopped_code == 4401; }));
  SYNC_REQUIRE(harness.client->status() == SeanceClient::Status::Stopped);
  SYNC_REQUIRE(server.peers.size() == 1);
}

SYNC_TEST(the_client_never_sends_a_write) {
  FakeSeance server;
  Harness harness(server);
  harness.client->start();
  SYNC_REQUIRE(wait_until([&] { return !server.peers.empty(); }));
  server.welcome_and_snapshot(0, QStringLiteral("abc"), 1);
  server.send(0, doc_edit(9, 0, 0, QStringLiteral("x")));
  server.send(0, {{QStringLiteral("type"), QStringLiteral("chat-message")},
                  {QStringLiteral("message"), QStringLiteral("hi")}});
  server.send(0, {{QStringLiteral("type"), QStringLiteral("error")},
                  {QStringLiteral("code"), QStringLiteral("readonly")}});
  // The gapped edit draws a resync request. Frames are handled in order, so
  // once that request arrives everything sent before the edit has been seen.
  auto& received = server.peers[0]->received;
  SYNC_REQUIRE(wait_until([&] { return received.size() >= 2; }));
  for (const QJsonObject& frame : received) {
    const QString type = frame.value(QStringLiteral("type")).toString();
    SYNC_REQUIRE(type == QStringLiteral("hello") || type == QStringLiteral("session-state"));
  }
}

}  // namespace
