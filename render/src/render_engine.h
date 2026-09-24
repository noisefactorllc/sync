#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <runtime/backend.h>
#include <runtime/frame_export.h>
#include <runtime/graph.h>
#include <sync/render/render_ring.hpp>

namespace noisefactor::sync::render_helper {

// Renders the current program headless at a fixed rate and writes every
// finished frame into the render ring.
//
// One GL context, owned by nm::Backend, on the thread that owns this object.
// Frames leave the GPU through the engine's own asynchronous readback queue,
// which returns top-down RGBA rows, the ring's layout.
class RenderEngine final : public QObject {
  Q_OBJECT

 public:
  struct Options {
    QSize size{1920, 1080};
    double fps = 60.0;
    // The reference renderer's default loop (noisemaker canvas.js): time is
    // (elapsed mod loop) / loop, a 0..1 value every program is written for.
    double loop_seconds = 10.0;
    QString data_root;
    std::string ring_name;
  };

  struct Stats {
    quint64 ticks = 0;
    quint64 rendered = 0;
    quint64 deferred = 0;       // skipped because readback was behind; time still advanced
    quint64 late_ticks = 0;     // ticks that started more than one frame late
    quint64 render_errors = 0;
    quint64 ring_writes = 0;
    quint64 ring_failures = 0;
  };

  explicit RenderEngine(Options options, QObject* parent = nullptr);
  ~RenderEngine() override;

  // Creates the context, the ring and the readback queue. Does not start the
  // clock; call run() for paced rendering or tick() to step by hand.
  [[nodiscard]] auto start(QString& error) -> bool;
  void run();
  void stop();

  // Swaps the program. Surfaces are keyed by texture id in the backend, so a
  // recompiled program with the same structure keeps its feedback state.
  void set_program(std::shared_ptr<nm::Graph> graph);

  // Runs before each render with the GL context current: the place to feed
  // external inputs (media frames, live parameters) into the program. The
  // generation changes with every set_program().
  using BeforeRender = std::function<void(nm::Backend&, nm::Graph&, quint64 generation)>;
  void set_before_render(BeforeRender hook) { before_render_ = std::move(hook); }

  // One frame at the current clock time, then any readbacks that finished.
  void tick();
  // Blocks until every readback in flight has reached the ring. For tests
  // and for a clean shutdown.
  void drain();

  [[nodiscard]] auto stats() const -> const Stats& { return stats_; }
  [[nodiscard]] auto frames_written() const -> quint64 { return stats_.ring_writes; }
  // Whether syncd has read the ring within the last second.
  [[nodiscard]] auto reader_attached() const -> bool;
  [[nodiscard]] auto options() const -> const Options& { return options_; }
  // The rendered surface of the last frame, top-down, straight from the
  // backend: the reference the ring's contents are checked against.
  [[nodiscard]] auto read_surface() const -> QImage;
  // Pins the clock: every tick renders at this normalized time. For tests.
  void freeze_time(double normalized);
  // Whether an overlay trace is running or waiting for the next tick; and a
  // wait for the running ones. For tests.
  [[nodiscard]] auto overlay_traces_pending() const -> bool;
  void wait_for_overlay_traces();

 signals:
  void render_failed(const QString& message);

 private:
  void schedule_next();
  void on_readback(const nm::ExportFrame& frame, double timestamp_ms);
  [[nodiscard]] auto normalized_time() const -> double;

  Options options_;
  // Declared before the queue so it is destroyed after it: the queue holds
  // the backend's GL function table.
  nm::Backend backend_;
  std::shared_ptr<nm::FrameExportQueue> queue_;
  std::function<void()> remove_sink_;
  std::optional<render::RenderRingSection> section_;
  std::optional<render::RenderRingWriter> writer_;
  std::shared_ptr<nm::Graph> graph_;
  quint64 generation_ = 0;
  BeforeRender before_render_;
  bool reported_render_error_ = false;
  bool started_ = false;
  std::optional<double> frozen_time_;

  QTimer timer_;
  // Collects finished readbacks between ticks; see RenderEngine::run().
  QTimer readback_timer_;
  QElapsedTimer clock_;
  qint64 next_tick_ns_ = 0;
  qint64 interval_ns_ = 0;
  Stats stats_;
};

}  // namespace noisefactor::sync::render_helper
