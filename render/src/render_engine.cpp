#include "render_engine.h"

#include <QCoreApplication>
#include <QThread>

#include <cmath>
#include <exception>
#include <span>
#include <utility>

namespace noisefactor::sync::render_helper {

namespace {

[[nodiscard]] auto current_pid() -> std::uint32_t {
  return static_cast<std::uint32_t>(QCoreApplication::applicationPid());
}

}  // namespace

RenderEngine::RenderEngine(Options options, QObject* parent)
    : QObject(parent), options_(std::move(options)) {
  timer_.setSingleShot(true);
  timer_.setTimerType(Qt::PreciseTimer);
  connect(&timer_, &QTimer::timeout, this, [this] {
    tick();
    schedule_next();
  });
  readback_timer_.setTimerType(Qt::PreciseTimer);
  connect(&readback_timer_, &QTimer::timeout, this, [this] {
    if (started_ && queue_) queue_->poll();
  });
}

RenderEngine::~RenderEngine() { stop(); }

auto RenderEngine::start(QString& error) -> bool {
  if (started_) return true;
  if (options_.fps <= 0.0 || options_.loop_seconds <= 0.0) {
    error = QStringLiteral("fps and loop length must be positive");
    return false;
  }
  const render::RenderRingGeometry geometry{
      .width = static_cast<std::uint32_t>(options_.size.width()),
      .height = static_cast<std::uint32_t>(options_.size.height()),
      .color_space = render::kRenderColorSpaceSrgb,
      // nm::Backend configures its sinks with the reference's default
      // descriptor, whose alpha mode is premultiplied, and the readback
      // shader premultiplies accordingly. The ring says what the pixels are.
      .alpha_mode = render::kRenderAlphaPremultiplied,
  };
  const auto bytes = render::render_ring_bytes(geometry);
  if (!bytes.has_value()) {
    error = QStringLiteral("size %1x%2 is outside 1..%3")
                .arg(options_.size.width())
                .arg(options_.size.height())
                .arg(render::kRenderRingMaxDimension);
    return false;
  }

  try {
    backend_.setup(nullptr, options_.data_root, options_.size);
    // A live host. The overlays of fibers, scratches and strayHair are
    // traced on the backend's worker thread, and a node keeps its previous
    // overlay until the new trace is done; traced in render() they stopped
    // the picture, 1.5 s at 1080p on an M2 for a switch to all three.
    backend_.setOverlayTraceMode(nm::OverlayTraceMode::Background);
    nm::FrameExportOptions export_options;
    export_options.slotCount = 3;
    export_options.onFrame = [this](const nm::ExportFrame& frame, double timestamp_ms) {
      on_readback(frame, timestamp_ms);
    };
    export_options.onError = [this](std::exception_ptr failure) {
      try {
        if (failure) std::rethrow_exception(failure);
      } catch (const std::exception& e) {
        ++stats_.ring_failures;
        emit render_failed(QStringLiteral("readback: %1").arg(QString::fromUtf8(e.what())));
      }
    };
    queue_ = backend_.createFrameExportQueue(std::move(export_options));
    remove_sink_ = backend_.addSink(queue_);
  } catch (const std::exception& e) {
    error = QStringLiteral("GPU setup failed: %1").arg(QString::fromUtf8(e.what()));
    return false;
  }

  std::string ring_error;
  section_ = render::RenderRingSection::create(options_.ring_name, *bytes, ring_error);
  if (!section_.has_value()) {
    error = QStringLiteral("render ring %1: %2")
                .arg(QString::fromStdString(options_.ring_name), QString::fromStdString(ring_error));
    return false;
  }
  writer_.emplace(section_->bytes(), geometry, current_pid());
  if (!writer_->valid()) {
    error = QStringLiteral("render ring could not be initialized");
    return false;
  }

  interval_ns_ = static_cast<qint64>(std::llround(1e9 / options_.fps));
  clock_.start();
  started_ = true;
  return true;
}

void RenderEngine::run() {
  if (!started_) return;
  next_tick_ns_ = clock_.nsecsElapsed();
  timer_.start(0);
  // A frame goes into the ring when the GPU finishes it, not at the next
  // tick. Polled only at ticks, a readback that finished just after one
  // waited a whole tick and then landed together with the next frame, and
  // syncd's reader, which keeps the newest frame, never saw the first: a
  // heavier program lost about 2% of its frames (68 of 3,604 in a minute) to
  // that bunching on 2026-09-24.
  readback_timer_.start(2);
}

void RenderEngine::stop() {
  timer_.stop();
  readback_timer_.stop();
  if (!started_) return;
  started_ = false;
  drain();
  if (writer_.has_value()) writer_->close();
  if (remove_sink_) remove_sink_();
  remove_sink_ = {};
  queue_.reset();
  writer_.reset();
  section_.reset();
}

void RenderEngine::set_program(std::shared_ptr<nm::Graph> graph) {
  graph_ = std::move(graph);
  ++generation_;
  reported_render_error_ = false;
}

void RenderEngine::freeze_time(double normalized) { frozen_time_ = normalized; }

auto RenderEngine::normalized_time() const -> double {
  if (frozen_time_.has_value()) return *frozen_time_;
  const double elapsed = static_cast<double>(clock_.nsecsElapsed()) / 1e9;
  return std::fmod(elapsed, options_.loop_seconds) / options_.loop_seconds;
}

void RenderEngine::tick() {
  if (!started_) return;
  ++stats_.ticks;
  // Finished readbacks first, so a frame reaches the ring as soon as the GPU
  // is done with it rather than a whole tick later.
  queue_->poll();
  const std::uint64_t now_us = render::render_clock_us();
  writer_->heartbeat(now_us);

  if (!graph_) return;
  if (backend_.shouldDeferRender()) {
    // Every readback slot is still in flight. Skipping the draw lets the GPU
    // catch up; time still advances, as the reference renderer does.
    ++stats_.deferred;
    return;
  }
  try {
    if (before_render_) before_render_(backend_, *graph_, generation_);
    backend_.render(*graph_, normalized_time(), static_cast<double>(now_us) / 1000.0);
    ++stats_.rendered;
  } catch (const std::exception& e) {
    ++stats_.render_errors;
    if (!reported_render_error_) {
      reported_render_error_ = true;
      emit render_failed(QString::fromUtf8(e.what()));
    }
  }
}

auto RenderEngine::overlay_traces_pending() const -> bool {
  return backend_.overlayTracesPending();
}

void RenderEngine::wait_for_overlay_traces() { backend_.waitForOverlayTraces(); }

void RenderEngine::drain() {
  if (!queue_) return;
  // Bounded: three slots, each a fence the driver will signal. The loop
  // yields rather than sleeping a fixed time, and gives up after a second so
  // a lost context cannot hang shutdown.
  QElapsedTimer waited;
  waited.start();
  for (;;) {
    const nm::FrameExportStats pending = queue_->stats();
    if (pending.accepted <= pending.completed + pending.failed) break;
    queue_->poll();
    if (waited.elapsed() > 1000) break;
    QThread::yieldCurrentThread();
  }
}

void RenderEngine::on_readback(const nm::ExportFrame& frame, double timestamp_ms) {
  if (!writer_.has_value()) return;
  if (frame.width != options_.size.width() || frame.height != options_.size.height()) {
    ++stats_.ring_failures;
    return;
  }
  const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(frame.data.constData()),
                                         static_cast<std::size_t>(frame.data.size()));
  const auto presentation_us = static_cast<std::uint64_t>(std::llround(timestamp_ms * 1000.0));
  if (writer_->write(bytes, static_cast<std::size_t>(frame.rowStride), presentation_us)) {
    ++stats_.ring_writes;
  } else {
    ++stats_.ring_failures;
  }
}

auto RenderEngine::read_surface() const -> QImage { return backend_.readSurface(); }

auto RenderEngine::reader_attached() const -> bool {
  return writer_.has_value() && writer_->reader_alive(render::render_clock_us(), 1'000'000);
}

void RenderEngine::schedule_next() {
  if (!started_) return;
  next_tick_ns_ += interval_ns_;
  const qint64 now = clock_.nsecsElapsed();
  if (now - next_tick_ns_ > interval_ns_) {
    // More than a frame behind: drop the missed ticks instead of rendering a
    // burst to catch up. Output stays at the configured rate.
    ++stats_.late_ticks;
    next_tick_ns_ = now;
  }
  const qint64 wait_ns = std::max<qint64>(0, next_tick_ns_ - now);
  timer_.start(static_cast<int>(wait_ns / 1'000'000));
}

}  // namespace noisefactor::sync::render_helper
