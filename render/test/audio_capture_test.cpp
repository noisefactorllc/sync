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
// from the test, standing in for RtAudio. It plays a quiet 60 Hz tone.
struct Interface {
  std::atomic<bool> present{true};
  std::atomic<bool> broken{false};
  std::atomic<int> opens{0};
  std::atomic<bool> closed{false};
  std::thread::id opened_on;
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
  // One 60 fps frame of audio, then drained: the next read finds nothing,
  // and the one after it the next frame's worth.
  auto read() -> audio::Packet override {
    if (device_->broken) {
      throw std::runtime_error(
          "Native audio capture stopped: driver error (5): the stream device was disconnected");
    }
    drained_ = !drained_;
    if (!drained_) return {48000, 2, frame_, 0, {}};
    audio::Packet packet{48000, 2, frame_, 0, std::vector<float>(1600)};
    for (std::size_t i = 0; i < 800; ++i) {
      const double t = static_cast<double>(frame_ + i) / 48000.0;
      const auto sample = static_cast<float>(0.05 * std::sin(2.0 * 3.14159265358979323846 * 60.0 * t));
      packet.samples[2 * i] = sample;
      packet.samples[2 * i + 1] = sample;
    }
    frame_ += 800;
    return packet;
  }

 private:
  std::shared_ptr<Interface> device_;
  std::uint64_t frame_ = 0;
  bool drained_ = false;
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
    device_->opened_on = std::this_thread::get_id();
    return std::make_unique<InterfaceCapture>(device_);
  }

 private:
  std::shared_ptr<Interface> device_;
};

// A capture over Sync's own capture ring, which hands out at most
// kMaximumPacketFrames a read, filled by the test as a driver would fill it.
class RingCapture final : public audio::Capture {
 public:
  explicit RingCapture(std::shared_ptr<audio::CaptureBuffer> ring) : ring_(std::move(ring)) {}
  auto read() -> audio::Packet override { return ring_->read(); }

 private:
  std::shared_ptr<audio::CaptureBuffer> ring_;
};

class RingBackend final : public audio::InputBackend {
 public:
  explicit RingBackend(std::shared_ptr<audio::CaptureBuffer> ring) : ring_(std::move(ring)) {}
  auto sources() -> std::vector<audio::Source> override {
    return {{"core:3", "Driver Ring", 1, 48000}};
  }
  auto open(const std::string&) -> std::unique_ptr<audio::Capture> override {
    return std::make_unique<RingCapture>(ring_);
  }

 private:
  std::shared_ptr<audio::CaptureBuffer> ring_;
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
  const auto started = capture.take_changes();
  SYNC_REQUIRE(started.size() == 1 && started[0].live);
  SYNC_REQUIRE(started[0].source.name == "Stage Interface");
  SYNC_REQUIRE(capture.read().samples.size() == 1600);

  // The driver reports the unplug through the next read. That read is
  // silence, not an exception that would stop the picture.
  device->broken = true;
  device->present = false;
  const auto unplugged = std::chrono::steady_clock::now();
  SYNC_REQUIRE(capture.read().samples.empty());
  const auto lost = capture.take_changes();
  SYNC_REQUIRE(lost.size() == 1 && !lost[0].live);
  SYNC_REQUIRE(lost[0].reason.contains(QStringLiteral("disconnected")));

  // While it is gone, every frame reads silence and nothing more is reported.
  QElapsedTimer gone;
  gone.start();
  while (!device->closed && gone.elapsed() < 3000) {
    SYNC_REQUIRE(capture.read().samples.empty());
    QThread::msleep(16);
  }
  SYNC_REQUIRE(capture.take_changes().empty());
  // The dead stream was closed where it was opened, on the capture's own
  // thread, and not at once: RtAudio is still finishing its own close when it
  // reports an unplug.
  SYNC_REQUIRE(device->closed);
  SYNC_REQUIRE(device->closed_on == device->opened_on);
  SYNC_REQUIRE(device->closed_on != std::this_thread::get_id());
  SYNC_REQUIRE(device->closed_at - unplugged >= std::chrono::milliseconds(400));

  device->broken = false;
  device->present = true;
  SYNC_REQUIRE(read_until_heard(capture, 3000).samples.size() == 1600);
  const auto back = capture.take_changes();
  SYNC_REQUIRE(back.size() == 1 && back[0].live);
  SYNC_REQUIRE(back[0].source.name == "Stage Interface");
  SYNC_REQUIRE(device->opens == 2);
}

SYNC_TEST(a_source_missing_at_start_is_waited_for) {
  auto device = std::make_shared<Interface>();
  device->present = false;
  AudioCapture capture(QStringLiteral("stage"), std::make_unique<InterfaceBackend>(device));
  capture.start();
  const auto waiting = capture.take_changes();
  SYNC_REQUIRE(waiting.size() == 1 && !waiting[0].live);
  SYNC_REQUIRE(waiting[0].reason == QStringLiteral("no audio input matches stage"));
  SYNC_REQUIRE(capture.read().samples.empty());

  device->present = true;
  SYNC_REQUIRE(read_until_heard(capture, 3000).samples.size() == 1600);
  const auto heard = capture.take_changes();
  SYNC_REQUIRE(heard.size() == 1 && heard[0].live);
}

SYNC_TEST(a_source_that_fails_at_once_reports_both_changes) {
  auto device = std::make_shared<Interface>();
  device->present = false;
  AudioCapture capture(QStringLiteral("stage"), std::make_unique<InterfaceBackend>(device));
  capture.start();
  SYNC_REQUIRE(capture.take_changes().size() == 1);
  // It comes back broken: heard, then lost on its first read.
  device->broken = true;
  device->present = true;
  std::vector<AudioCapture::Change> changes;
  QElapsedTimer waited;
  waited.start();
  while (changes.size() < 2 && waited.elapsed() < 3000) {
    SYNC_REQUIRE(capture.read().samples.empty());
    for (auto& change : capture.take_changes()) changes.push_back(std::move(change));
    QThread::msleep(16);
  }
  SYNC_REQUIRE(changes.size() >= 2);
  SYNC_REQUIRE(changes[0].live && !changes[1].live);
}

SYNC_TEST(the_live_stream_is_closed_on_the_capture_thread) {
  auto device = std::make_shared<Interface>();
  {
    AudioCapture capture(QStringLiteral("stage"), std::make_unique<InterfaceBackend>(device));
    capture.start();
    SYNC_REQUIRE(capture.read().samples.size() == 1600);
  }
  SYNC_REQUIRE(device->closed);
  SYNC_REQUIRE(device->closed_on == device->opened_on);
  SYNC_REQUIRE(device->closed_on != std::this_thread::get_id());
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
  // Not the 500 ms to its next search.
  SYNC_REQUIRE(stopped.elapsed() < 400);
}

SYNC_TEST(a_stream_unplugged_just_before_shutdown_is_closed_late) {
  auto device = std::make_shared<Interface>();
  QElapsedTimer stopped;
  std::chrono::steady_clock::time_point unplugged;
  {
    AudioCapture capture(QStringLiteral("stage"), std::make_unique<InterfaceBackend>(device));
    capture.start();
    SYNC_REQUIRE(capture.read().samples.size() == 1600);
    // Unplugged, and the helper stops before the render thread reads again.
    device->broken = true;
    unplugged = std::chrono::steady_clock::now();
    stopped.start();
  }
  SYNC_REQUIRE(device->closed);
  SYNC_REQUIRE(device->closed_on == device->opened_on);
  SYNC_REQUIRE(device->closed_at - unplugged >= std::chrono::milliseconds(400));
  SYNC_REQUIRE(stopped.elapsed() < 3000);
}

SYNC_TEST(the_analysers_hear_the_newest_sound_every_frame) {
  // A driver delivers 800 frames of 48 kHz audio per 60 fps frame, more than
  // one read returns. Five seconds of silence, then a loud 60 Hz tone: the
  // bass band must rise within a few frames, not once a backlog of old
  // silence has been worked through.
  auto ring = std::make_shared<audio::CaptureBuffer>(48000, 1);
  AudioCapture capture(QStringLiteral("ring"), std::make_unique<RingBackend>(ring));
  capture.start();
  nm::AudioInput input;
  nm::AudioDevice heard;
  std::uint64_t sample = 0;
  const auto frame = [&](bool tone) {
    std::vector<float> samples(800);
    for (float& value : samples) {
      const double t = static_cast<double>(sample++) / 48000.0;
      value = tone ? static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979323846 * 60.0 * t))
                   : 0.0f;
    }
    ring->push(samples);
    (void)feed_audio(capture, input, heard);
    input.update();
  };
  for (int i = 0; i < 300; ++i) frame(false);
  SYNC_REQUIRE(input.state().low == 0.0);
  SYNC_REQUIRE(ring->read().samples.empty());
  int frames_to_hear = 0;
  while (input.state().low <= 0.1 && frames_to_hear < 60) {
    frame(true);
    ++frames_to_hear;
  }
  SYNC_REQUIRE(frames_to_hear <= 5);
}

SYNC_TEST(a_full_ring_is_heard_at_its_newest_on_the_first_frame) {
  // Before the first program nothing reads the capture, so the ring is full
  // of old sound by the first frame. That frame hears the end of it.
  auto ring = std::make_shared<audio::CaptureBuffer>(48000, 1);
  AudioCapture capture(QStringLiteral("ring"), std::make_unique<RingBackend>(ring));
  capture.start();
  std::vector<float> samples(65536);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const bool tone = i + 4800 >= samples.size();  // only the last 100 ms
    samples[i] = tone ? static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979323846 * 60.0 *
                                                          static_cast<double>(i) / 48000.0))
                      : 0.0f;
  }
  ring->push(samples);
  nm::AudioInput input;
  nm::AudioDevice heard;
  (void)feed_audio(capture, input, heard);
  input.update();
  SYNC_REQUIRE(ring->read().samples.empty());
  SYNC_REQUIRE(input.state().low > 0.1);
}

SYNC_TEST(levels_fall_to_zero_while_the_source_is_gone) {
  auto device = std::make_shared<Interface>();
  AudioCapture capture(QStringLiteral("stage"), std::make_unique<InterfaceBackend>(device));
  capture.start();
  nm::AudioInput input;
  nm::AudioDevice heard;
  const auto frame = [&] {
    auto changes = feed_audio(capture, input, heard);
    input.update();
    return changes;
  };
  SYNC_REQUIRE(frame().size() == 1);
  SYNC_REQUIRE(heard.name == QStringLiteral("Stage Interface"));
  for (int i = 0; i < 30; ++i) (void)frame();
  SYNC_REQUIRE(input.state().low > 0.1);

  // Unplugged: the analysers do not hold the last sound heard.
  device->broken = true;
  device->present = false;
  const auto lost = frame();
  SYNC_REQUIRE(lost.size() == 1 && !lost[0].live);
  SYNC_REQUIRE(input.state().low == 0.0 && input.state().vol == 0.0);
  for (int i = 0; i < 10; ++i) (void)frame();
  SYNC_REQUIRE(input.state().low == 0.0 && input.state().vol == 0.0);

  device->broken = false;
  device->present = true;
  QElapsedTimer waited;
  waited.start();
  while (input.state().low <= 0.1 && waited.elapsed() < 3000) {
    (void)frame();
    QThread::msleep(16);
  }
  SYNC_REQUIRE(input.state().low > 0.1);
}


SYNC_TEST(a_fixture_source_delivers_interleaved_samples) {
  // Sync's backend serves synthetic 32-channel sources under this switch, so
  // the capture path runs with no microphone and no permission prompt.
  qputenv("SYNC_AUDIO_TEST_FIXTURE", "1");
  AudioCapture capture(QStringLiteral("audio_32_tones"));
  capture.start();
  const auto started = capture.take_changes();
  SYNC_REQUIRE(started.size() == 1 && started[0].live);
  SYNC_REQUIRE(started[0].source.channels == 32);
  SYNC_REQUIRE(started[0].source.sample_rate == 48000);
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
