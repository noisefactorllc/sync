#include "test_harness.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QThread>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <runtime/audio_state.h>
#include <runtime/backend.h>

#include "audio_capture.h"
#include "program_compiler.h"
#include "render_engine.h"

namespace {

using namespace noisefactor::sync;
using namespace noisefactor::sync::render_helper;

// The gradient's rotation follows the bass band (band 0). audio() yields a
// unit level that the engine scales into rotation's -180..180 range, so
// silence and a saturated band are the same angle; the tone below is quiet
// enough to leave the band part-way up (0.82 in the reference).
const QString kAudioRotation = QStringLiteral(
    "search synth\n"
    "let angle = audio(band: 0)\n"
    "gradient(seed: 1, rotation: angle).write(o0)\n"
    "render(o0)\n");

[[nodiscard]] auto resolved_rotation(const nm::Backend& backend, const nm::Graph& graph) -> double {
  for (const nm::Pass& pass : graph.passes) {
    if (!pass.uniforms.contains(QStringLiteral("rotation"))) continue;
    return backend
        .resolveUniformValue(pass.uniforms.value(QStringLiteral("rotation")), 0.0,
                             pass.uniformSpecs.value(QStringLiteral("rotation")).toObject())
        .toDouble();
  }
  return std::nan("");
}

[[nodiscard]] auto data_root() -> QString { return QStringLiteral(SYNC_RENDER_TEST_DATA_ROOT); }

// A two-channel interface that can be unplugged, broken and plugged back in
// from the test, standing in for RtAudio.
struct Interface {
  std::atomic<bool> present{true};
  std::atomic<bool> broken{false};
  std::atomic<int> opens{0};
  std::atomic<bool> closed{false};
  std::thread::id closed_on;
  std::chrono::steady_clock::time_point closed_at;
};

class InterfaceCapture final : public audio::Capture {
 public:
  explicit InterfaceCapture(std::shared_ptr<Interface> device) : device_(std::move(device)) {}
  ~InterfaceCapture() override {
    device_->closed_on = std::this_thread::get_id();
    device_->closed_at = std::chrono::steady_clock::now();
    device_->closed = true;
  }
  auto read() -> audio::Packet override {
    if (device_->broken) {
      throw std::runtime_error(
          "Native audio capture stopped: driver error (5): the stream device was disconnected");
    }
    return {48000, 2, 0, 0, {0.25f, -0.25f, 0.5f, -0.5f}};
  }

 private:
  std::shared_ptr<Interface> device_;
};

class InterfaceBackend final : public audio::InputBackend {
 public:
  explicit InterfaceBackend(std::shared_ptr<Interface> device) : device_(std::move(device)) {}
  auto sources() -> std::vector<audio::Source> override {
    if (!device_->present) return {};
    return {{"core:9", "Stage Interface", 2, 48000}};
  }
  auto open(const std::string& id) -> std::unique_ptr<audio::Capture> override {
    if (!device_->present || id != "core:9") throw std::runtime_error("no such source");
    ++device_->opens;
    return std::make_unique<InterfaceCapture>(device_);
  }

 private:
  std::shared_ptr<Interface> device_;
};

// Reads as the render thread does, once a frame, until samples arrive.
[[nodiscard]] auto read_until_heard(AudioCapture& capture, int milliseconds) -> audio::Packet {
  QElapsedTimer waited;
  waited.start();
  for (;;) {
    audio::Packet packet = capture.read();
    if (!packet.samples.empty() || waited.elapsed() > milliseconds) return packet;
    QThread::msleep(16);
  }
}

SYNC_TEST(audio_sources_are_chosen_by_id_then_name_then_default) {
  const std::vector<audio::Source> sources{{"core:1", "MacBook Pro Microphone", 1, 48000},
                                           {"core:7", "Scarlett 18i20", 18, 48000}};
  SYNC_REQUIRE(choose_audio_source(sources, QStringLiteral("core:7"))->name == "Scarlett 18i20");
  SYNC_REQUIRE(choose_audio_source(sources, QStringLiteral("scarlett"))->id == "core:7");
  SYNC_REQUIRE(choose_audio_source(sources, QStringLiteral("default"))->id == "core:1");
  SYNC_REQUIRE(!choose_audio_source(sources, QStringLiteral("Loopback")).has_value());
  SYNC_REQUIRE(!choose_audio_source({}, QStringLiteral("default")).has_value());
}

SYNC_TEST(an_unplugged_source_is_silence_until_it_is_plugged_back_in) {
  auto device = std::make_shared<Interface>();
  AudioCapture capture(QStringLiteral("stage"), std::make_unique<InterfaceBackend>(device));
  capture.start();
  const auto started = capture.take_change();
  SYNC_REQUIRE(started.has_value() && started->live);
  SYNC_REQUIRE(capture.read().samples.size() == 4);

  // The driver reports the unplug through the next read. That read is
  // silence, not an exception that would stop the picture.
  device->broken = true;
  device->present = false;
  const auto unplugged = std::chrono::steady_clock::now();
  SYNC_REQUIRE(capture.read().samples.empty());
  const auto lost = capture.take_change();
  SYNC_REQUIRE(lost.has_value() && !lost->live);
  SYNC_REQUIRE(lost->reason.contains(QStringLiteral("disconnected")));

  // While it is gone, every frame reads silence and nothing more is reported.
  QElapsedTimer gone;
  gone.start();
  while (!device->closed && gone.elapsed() < 3000) {
    SYNC_REQUIRE(capture.read().samples.empty());
    QThread::msleep(16);
  }
  SYNC_REQUIRE(!capture.take_change().has_value());
  // The dead stream was closed off the render thread, and not at once:
  // RtAudio is still finishing its own close when it reports an unplug.
  SYNC_REQUIRE(device->closed);
  SYNC_REQUIRE(device->closed_on != std::this_thread::get_id());
  SYNC_REQUIRE(device->closed_at - unplugged >= std::chrono::milliseconds(400));

  device->broken = false;
  device->present = true;
  SYNC_REQUIRE(read_until_heard(capture, 3000).samples.size() == 4);
  const auto back = capture.take_change();
  SYNC_REQUIRE(back.has_value() && back->live);
  SYNC_REQUIRE(capture.source().name == "Stage Interface");
  SYNC_REQUIRE(device->opens == 2);
}

SYNC_TEST(a_source_missing_at_start_is_waited_for) {
  auto device = std::make_shared<Interface>();
  device->present = false;
  AudioCapture capture(QStringLiteral("stage"), std::make_unique<InterfaceBackend>(device));
  capture.start();
  const auto waiting = capture.take_change();
  SYNC_REQUIRE(waiting.has_value() && !waiting->live);
  SYNC_REQUIRE(waiting->reason == QStringLiteral("no audio input matches stage"));
  SYNC_REQUIRE(capture.read().samples.empty());

  device->present = true;
  SYNC_REQUIRE(read_until_heard(capture, 3000).samples.size() == 4);
  const auto heard = capture.take_change();
  SYNC_REQUIRE(heard.has_value() && heard->live);
}

SYNC_TEST(a_capture_waiting_for_its_source_stops_promptly) {
  auto device = std::make_shared<Interface>();
  device->present = false;
  QElapsedTimer stopped;
  {
    AudioCapture capture(QStringLiteral("stage"), std::make_unique<InterfaceBackend>(device));
    capture.start();
    QThread::msleep(50);
    stopped.start();
  }
  SYNC_REQUIRE(stopped.elapsed() < 250);
}

SYNC_TEST(a_fixture_source_delivers_interleaved_samples) {
  // Sync's backend serves synthetic 32-channel sources under this switch, so
  // the capture path runs with no microphone and no permission prompt.
  qputenv("SYNC_AUDIO_TEST_FIXTURE", "1");
  AudioCapture capture(QStringLiteral("audio_32_tones"));
  capture.start();
  const auto started = capture.take_change();
  SYNC_REQUIRE(started.has_value() && started->live);
  SYNC_REQUIRE(capture.source().channels == 32);
  SYNC_REQUIRE(capture.source().sample_rate == 48000);
  audio::Packet packet;
  QElapsedTimer waited;
  waited.start();
  while (packet.samples.empty() && waited.elapsed() < 5000) {
    packet = capture.read();
    if (packet.samples.empty()) QThread::yieldCurrentThread();
  }
  qunsetenv("SYNC_AUDIO_TEST_FIXTURE");
  SYNC_REQUIRE(!packet.samples.empty());
  SYNC_REQUIRE(packet.channels == 32);
  SYNC_REQUIRE(packet.samples.size() % 32 == 0);
  float peak = 0;
  for (const float sample : packet.samples) peak = std::max(peak, std::abs(sample));
  SYNC_REQUIRE(peak > 0.5f);
}

SYNC_TEST(a_bass_tone_moves_an_audio_driven_parameter) {
  ProgramCompiler compiler(data_root());
  const auto compiled = compiler.compile(kAudioRotation);
  SYNC_REQUIRE(compiled.graph != nullptr);

  RenderEngine::Options options;
  options.size = QSize(64, 48);
  options.data_root = data_root();
  options.ring_name = QDir(QDir::tempPath())
                          .filePath(QStringLiteral("sync-render-audio-test-%1.frames")
                                        .arg(QCoreApplication::applicationPid()))
                          .toStdString();
  RenderEngine engine(options);
  QString error;
  SYNC_REQUIRE(engine.start(error));

  nm::AudioInput input;
  bool loud = false;
  double rotation = 0;
  std::uint64_t frame_offset = 0;
  engine.set_before_render([&](nm::Backend& backend, nm::Graph& graph, quint64) {
    // One 60 fps frame of mono audio at 48 kHz: silence, or a quiet 60 Hz
    // tone squarely in the bass band.
    std::vector<float> samples(800);
    for (std::size_t i = 0; i < samples.size(); ++i) {
      const double t = static_cast<double>(frame_offset + i) / 48000.0;
      samples[i] = loud ? static_cast<float>(0.05 * std::sin(2.0 * 3.14159265358979323846 * 60.0 * t)) : 0.0f;
    }
    frame_offset += samples.size();
    input.pushDefault(samples.data(), static_cast<qsizetype>(samples.size()), 1);
    input.update();
    backend.setAudioState(input.snapshot());
    rotation = resolved_rotation(backend, graph);
  });
  engine.set_program(compiled.graph);
  engine.freeze_time(0.0);

  for (int i = 0; i < 30; ++i) engine.tick();
  SYNC_REQUIRE(rotation == -180.0);
  const QImage quiet = engine.read_surface();
  loud = true;
  for (int i = 0; i < 30; ++i) engine.tick();
  // Part-way up the band: a real change of angle, short of the +180 that
  // would coincide with the quiet -180.
  SYNC_REQUIRE(rotation > 0.0 && rotation < 170.0);
  const QImage bass = engine.read_surface();
  SYNC_REQUIRE(quiet != bass);
  // And back: in silence the band falls away and the frame returns to the
  // quiet one once the smoothing has decayed.
  loud = false;
  for (int i = 0; i < 120; ++i) engine.tick();
  SYNC_REQUIRE(rotation == -180.0);
  SYNC_REQUIRE(engine.read_surface() == quiet);
}

}  // namespace
