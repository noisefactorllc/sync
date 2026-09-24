// sync-render: joins a seance session read-only, compiles the program
// document with noisemaker-for-qt, renders it headless on the GPU, and writes
// each frame into the render ring for syncd to publish. See event_log.h for
// the line protocol it reports on stdout.

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <thread>

#include "event_log.h"
#include "media_inputs.h"
#include "program_compiler.h"
#include "render_engine.h"
#include "seance_client.h"

#include <sync/render/render_ring.hpp>

namespace {

using namespace noisefactor::sync;
using namespace noisefactor::sync::render_helper;

constexpr int kExitUsage = 2;
constexpr int kExitStartup = 3;
constexpr int kExitSessionEnded = 4;

std::atomic<bool> g_stop_requested{false};

// The helper's stderr is syncd's log. Qt's media backends can repeat one
// warning per decoded frame (macOS: "cannot create texture, Metal texture
// cache was released?" for every video frame, harmless there), which would
// bury everything else. Each distinct line prints once, then at most once
// every ten seconds with the number of repeats it stands for.
void rate_limited_message(QtMsgType type, const QMessageLogContext& context,
                          const QString& message) {
  static QMutex mutex;
  struct Seen {
    qint64 last_printed_ms = 0;
    quint64 suppressed = 0;
  };
  static QHash<QString, Seen> seen;
  static QElapsedTimer clock;
  QMutexLocker lock(&mutex);
  if (!clock.isValid()) clock.start();
  const qint64 now = clock.elapsed();
  const auto it = seen.find(message);
  if (it != seen.end() && now - it->last_printed_ms < 10'000) {
    ++it->suppressed;
    return;
  }
  const quint64 repeats = it != seen.end() ? it->suppressed : 0;
  if (seen.size() > 1024) seen.clear();  // bounded, whatever a backend emits
  seen.insert(message, Seen{now, 0});
  const QString line = qFormatLogMessage(type, context, message);
  if (repeats > 0) {
    std::fprintf(stderr, "%s (repeated %llu times)\n", qPrintable(line),
                 static_cast<unsigned long long>(repeats));
  } else {
    std::fprintf(stderr, "%s\n", qPrintable(line));
  }
  if (type == QtFatalMsg) std::abort();
}

extern "C" void request_stop(int) { g_stop_requested.store(true); }

// Search order: the flag, the environment, the tree the build copies next to
// the binary, and the macOS bundle's Resources.
[[nodiscard]] auto resolve_data_root(const QString& flag) -> QString {
  QStringList candidates;
  if (!flag.isEmpty()) candidates << flag;
  const QString env = qEnvironmentVariable("SYNC_RENDER_DATA_ROOT");
  if (!env.isEmpty()) candidates << env;
  const QString app_dir = QCoreApplication::applicationDirPath();
  candidates << app_dir + QStringLiteral("/noisemaker")
             << app_dir + QStringLiteral("/../Resources/noisemaker");
  for (const QString& candidate : candidates) {
    if (QFileInfo(candidate + QStringLiteral("/effects")).isDir() &&
        QFileInfo(candidate + QStringLiteral("/shaders")).isDir()) {
      return QDir(candidate).canonicalPath();
    }
  }
  return {};
}

[[nodiscard]] auto read_token(const QString& path) -> QString {
  QFile file(path);
  if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) return {};
  return QString::fromUtf8(file.readAll()).trimmed();
}

void write_token(const QString& path, const QString& token) {
  if (path.isEmpty()) return;
  QFile file(path);
  // Owner-only before the token is written, so it is never readable by
  // anyone else even for an instant.
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
  file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  file.write(token.toUtf8());
}

[[nodiscard]] auto status_name(SeanceClient::Status status) -> QString {
  switch (status) {
    case SeanceClient::Status::Offline: return QStringLiteral("offline");
    case SeanceClient::Status::Connecting: return QStringLiteral("connecting");
    case SeanceClient::Status::Online: return QStringLiteral("online");
    case SeanceClient::Status::Stopped: return QStringLiteral("stopped");
  }
  return QStringLiteral("unknown");
}

[[nodiscard]] auto stats_json(const RenderEngine& engine) -> QJsonObject {
  const RenderEngine::Stats& s = engine.stats();
  return {{QStringLiteral("reader_attached"), engine.reader_attached()},
          {QStringLiteral("ticks"), static_cast<double>(s.ticks)},
          {QStringLiteral("rendered"), static_cast<double>(s.rendered)},
          {QStringLiteral("deferred"), static_cast<double>(s.deferred)},
          {QStringLiteral("late_ticks"), static_cast<double>(s.late_ticks)},
          {QStringLiteral("render_errors"), static_cast<double>(s.render_errors)},
          {QStringLiteral("ring_writes"), static_cast<double>(s.ring_writes)},
          {QStringLiteral("ring_failures"), static_cast<double>(s.ring_failures)}};
}

}  // namespace

int main(int argc, char** argv) {
#if defined(Q_OS_MACOS)
  // A background renderer, not an app: keep it out of the Dock and the
  // app switcher when Qt's platform plugin loads.
  if (qEnvironmentVariableIsEmpty("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM")) {
    qputenv("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM", "1");
  }
#endif
  qInstallMessageHandler(rate_limited_message);
  // QOffscreenSurface, which the backend renders through, needs a GUI
  // application object even though nothing is ever shown.
  QGuiApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("sync-render"));

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("Render a seance session's noisemaker program into Sync's render ring."));
  parser.addHelpOption();
  // Not "--session": QGuiApplication consumes -session/--session (X11 session
  // management) from argv before this parser ever sees it.
  const QCommandLineOption session_opt(
      QStringLiteral("join"), QStringLiteral("Seance session id or share link to join."),
      QStringLiteral("id-or-url"));
  const QCommandLineOption server_opt(
      QStringLiteral("seance-url"), QStringLiteral("Seance server."), QStringLiteral("url"),
      QStringLiteral("https://seance.noisefactor.io"));
  const QCommandLineOption origin_opt(
      QStringLiteral("origin"),
      QStringLiteral("Origin header presented to seance; the server must allow it."),
      QStringLiteral("origin"), QStringLiteral("https://sync.noisedeck.app"));
  const QCommandLineOption token_opt(
      QStringLiteral("anon-token-file"),
      QStringLiteral("Keep the seance identity in this owner-only file across runs."),
      QStringLiteral("path"));
  const QCommandLineOption program_opt(
      QStringLiteral("program-file"),
      QStringLiteral("Render a program file instead of joining a session."),
      QStringLiteral("path"));
  const QCommandLineOption width_opt(QStringLiteral("width"), QStringLiteral("Frame width."),
                                     QStringLiteral("px"), QStringLiteral("1920"));
  const QCommandLineOption height_opt(QStringLiteral("height"), QStringLiteral("Frame height."),
                                      QStringLiteral("px"), QStringLiteral("1080"));
  const QCommandLineOption fps_opt(QStringLiteral("fps"), QStringLiteral("Frames per second."),
                                   QStringLiteral("rate"), QStringLiteral("60"));
  const QCommandLineOption loop_opt(QStringLiteral("loop-seconds"),
                                    QStringLiteral("Length of the program's time loop."),
                                    QStringLiteral("seconds"), QStringLiteral("10"));
  const QCommandLineOption ring_opt(QStringLiteral("ring"),
                                    QStringLiteral("Render ring section name."),
                                    QStringLiteral("name"));
  const QCommandLineOption data_opt(QStringLiteral("data-root"),
                                    QStringLiteral("noisemaker-for-qt effects/shaders tree."),
                                    QStringLiteral("path"));
  const QCommandLineOption frames_opt(QStringLiteral("frames"),
                                      QStringLiteral("Exit after writing this many frames."),
                                      QStringLiteral("count"), QStringLiteral("0"));
  const QCommandLineOption media_opt(
      QStringLiteral("media"),
      QStringLiteral("Source for media() steps, repeatable, in step order: camera, "
                     "camera:<name-or-index>, or file:<image-or-video>."),
      QStringLiteral("spec"));
  const QCommandLineOption stdin_opt(
      QStringLiteral("exit-on-stdin-eof"),
      QStringLiteral("Exit when stdin closes (the supervising process went away)."));
  parser.addOptions({session_opt, server_opt, origin_opt, token_opt, program_opt, width_opt,
                     height_opt, fps_opt, loop_opt, ring_opt, data_opt, frames_opt, media_opt,
                     stdin_opt});
  parser.process(app);

  EventLog events;
  const auto fatal = [&events](const QString& message, int code) {
    events.write(QStringLiteral("fatal"), {{QStringLiteral("message"), message}});
    std::fprintf(stderr, "sync-render: %s\n", qPrintable(message));
    return code;
  };

  bool ok_width = false, ok_height = false, ok_fps = false, ok_loop = false, ok_frames = false;
  RenderEngine::Options options;
  options.size = QSize(parser.value(width_opt).toInt(&ok_width),
                       parser.value(height_opt).toInt(&ok_height));
  options.fps = parser.value(fps_opt).toDouble(&ok_fps);
  options.loop_seconds = parser.value(loop_opt).toDouble(&ok_loop);
  const qulonglong frame_limit = parser.value(frames_opt).toULongLong(&ok_frames);
  if (!ok_width || !ok_height || !ok_fps || !ok_loop || !ok_frames) {
    return fatal(QStringLiteral("width, height, fps, loop-seconds and frames must be numbers"),
                 kExitUsage);
  }
  options.ring_name = parser.isSet(ring_opt) ? parser.value(ring_opt).toStdString()
                                             : render::default_render_ring_name();

  QList<MediaSpec> media_specs;
  for (const QString& text : parser.values(media_opt)) {
    const auto spec = parse_media_spec(text);
    if (!spec.has_value()) {
      return fatal(QStringLiteral("--media %1: expected camera, camera:<name>, or file:<path>").arg(text),
                   kExitUsage);
    }
    media_specs.push_back(*spec);
  }

  const bool from_file = parser.isSet(program_opt);
  std::optional<QString> session_id;
  if (!from_file) {
    session_id = parse_session_id(parser.value(session_opt));
    if (!session_id.has_value()) {
      return fatal(QStringLiteral("--join needs a session id or a link with ?seance="),
                   kExitUsage);
    }
  }

  options.data_root = resolve_data_root(parser.value(data_opt));
  if (options.data_root.isEmpty()) {
    return fatal(QStringLiteral("no noisemaker data root (effects/ and shaders/) found"),
                 kExitStartup);
  }
  ProgramCompiler compiler(options.data_root);
  if (compiler.effect_count() == 0) {
    return fatal(QStringLiteral("no effect definitions under %1").arg(options.data_root),
                 kExitStartup);
  }

  RenderEngine engine(options);
  QString error;
  if (!engine.start(error)) return fatal(error, kExitStartup);
  events.write(QStringLiteral("ready"),
               {{QStringLiteral("ring"), QString::fromStdString(options.ring_name)},
                {QStringLiteral("width"), options.size.width()},
                {QStringLiteral("height"), options.size.height()},
                {QStringLiteral("fps"), options.fps},
                {QStringLiteral("loop_seconds"), options.loop_seconds},
                {QStringLiteral("alpha_mode"), QStringLiteral("premultiplied")},
                {QStringLiteral("effects"), compiler.effect_count()}});

  QObject::connect(&engine, &RenderEngine::render_failed, &app, [&events](const QString& message) {
    events.write(QStringLiteral("render_error"), {{QStringLiteral("message"), message}});
  });

  MediaInputs media(media_specs);
  QObject::connect(&media, &MediaInputs::status, &app,
                   [&events](int source, const QString& state, const QString& detail) {
                     events.write(QStringLiteral("media"),
                                  {{QStringLiteral("source"), source},
                                   {QStringLiteral("state"), state},
                                   {QStringLiteral("detail"), detail}});
                   });
  if (!media.start(error)) return fatal(error, kExitStartup);
  engine.set_before_render([&media, &compiler](nm::Backend& backend, nm::Graph& graph,
                                               quint64 generation) {
    media.apply(backend, graph, generation, compiler.registry());
  });

  // Edits can arrive faster than a compile (a slider drag on the controller).
  // Only the newest text is compiled; intermediate ones are superseded.
  struct Pending {
    QString text;
    qint64 rev = -1;
    QString source;
    bool queued = false;
  } pending;
  const auto compile_now = [&] {
    pending.queued = false;
    QElapsedTimer timer;
    timer.start();
    ProgramCompiler::Result result = compiler.compile(pending.text);
    QJsonObject fields{{QStringLiteral("source"), pending.source},
                       {QStringLiteral("rev"), static_cast<double>(pending.rev)},
                       {QStringLiteral("ok"), result.graph != nullptr},
                       {QStringLiteral("compile_ms"), static_cast<double>(timer.nsecsElapsed()) / 1e6}};
    if (result.graph) {
      engine.set_program(std::move(result.graph));
    } else {
      fields.insert(QStringLiteral("error"), result.error);
      if (!result.diagnostic.isEmpty()) {
        fields.insert(QStringLiteral("diagnostic"), result.diagnostic);
      }
    }
    events.write(QStringLiteral("program"), fields);
  };
  const auto submit_program = [&](const QString& text, qint64 rev, const QString& source) {
    pending.text = text;
    pending.rev = rev;
    pending.source = source;
    if (!pending.queued) {
      pending.queued = true;
      QTimer::singleShot(0, &app, compile_now);
    }
  };

  std::unique_ptr<SeanceClient> client;
  if (from_file) {
    QFile file(parser.value(program_opt));
    if (!file.open(QIODevice::ReadOnly)) {
      return fatal(QStringLiteral("cannot read %1").arg(file.fileName()), kExitStartup);
    }
    submit_program(QString::fromUtf8(file.readAll()), 0, QStringLiteral("file"));
  } else {
    const QString token_path = parser.value(token_opt);
    SeanceClient::Options client_options;
    client_options.server = QUrl(parser.value(server_opt));
    client_options.session_id = *session_id;
    client_options.origin = parser.value(origin_opt);
    client_options.anon_token = read_token(token_path);
    client = std::make_unique<SeanceClient>(client_options);
    QObject::connect(client.get(), &SeanceClient::program_changed, &app,
                     [&](const QString& text, qint64 rev) {
                       submit_program(text, rev, QStringLiteral("seance"));
                     });
    QObject::connect(client.get(), &SeanceClient::status_changed, &app,
                     [&events](SeanceClient::Status status, const QString& detail) {
                       events.write(QStringLiteral("seance"),
                                    {{QStringLiteral("status"), status_name(status)},
                                     {QStringLiteral("detail"), detail}});
                     });
    QObject::connect(client.get(), &SeanceClient::server_error, &app,
                     [](const QString& code, const QString& detail) {
                       std::fprintf(stderr, "sync-render: seance error %s %s\n", qPrintable(code),
                                    qPrintable(detail));
                     });
    QObject::connect(client.get(), &SeanceClient::anon_token_changed, &app,
                     [token_path](const QString& token) { write_token(token_path, token); });
    QObject::connect(client.get(), &SeanceClient::stopped, &app,
                     [&events](int code, const QString& reason) {
                       events.write(QStringLiteral("stopped"),
                                    {{QStringLiteral("code"), code},
                                     {QStringLiteral("reason"), reason}});
                       QCoreApplication::exit(kExitSessionEnded);
                     });
    client->start();
  }

  QTimer stats_timer;
  QObject::connect(&stats_timer, &QTimer::timeout, &app, [&] {
    events.write(QStringLiteral("stats"), stats_json(engine));
  });
  stats_timer.start(1000);

  // Signals and --frames both end the run through the event loop, so the
  // ring is closed and the last readbacks land before the process exits.
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);
  QTimer control_timer;
  QObject::connect(&control_timer, &QTimer::timeout, &app, [&] {
    if (g_stop_requested.load()) QCoreApplication::exit(0);
    if (frame_limit != 0 && engine.frames_written() >= frame_limit) QCoreApplication::exit(0);
  });
  control_timer.start(20);

  if (parser.isSet(stdin_opt)) {
    // A supervisor that dies cannot send a signal; its end of our stdin
    // closing is the one notice that always arrives.
    std::thread([] {
      while (std::fgetc(stdin) != EOF) {
      }
      g_stop_requested.store(true);
    }).detach();
  }

  engine.run();
  const int code = app.exec();
  if (client) client->stop();
  engine.stop();
  events.write(QStringLiteral("stats"), stats_json(engine));
  return code;
}
