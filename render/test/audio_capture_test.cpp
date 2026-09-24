#include "test_harness.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QThread>

#include <cmath>
#include <string>
#include <vector>

#include <runtime/audio_state.h>

#include "audio_capture.h"
#include "program_compiler.h"
#include "render_engine.h"

namespace {

using namespace noisefactor::sync;
using namespace noisefactor::sync::render_helper;

// The gradient's rotation follows the bass band (band 0) over a quarter
// turn. Not -180..180: those ends are the same angle.
const QString kAudioRotation = QStringLiteral(
    "search synth\n"
    "let angle = audio(band: 0, min: 0, max: 90)\n"
    "gradient(seed: 1, rotation: angle).write(o0)\n"
    "render(o0)\n");

[[nodiscard]] auto data_root() -> QString { return QStringLiteral(SYNC_RENDER_TEST_DATA_ROOT); }

SYNC_TEST(audio_sources_are_chosen_by_id_then_name_then_default) {
  const std::vector<audio::Source> sources{{"core:1", "MacBook Pro Microphone", 1, 48000},
                                           {"core:7", "Scarlett 18i20", 18, 48000}};
  SYNC_REQUIRE(choose_audio_source(sources, QStringLiteral("core:7"))->name == "Scarlett 18i20");
  SYNC_REQUIRE(choose_audio_source(sources, QStringLiteral("scarlett"))->id == "core:7");
  SYNC_REQUIRE(choose_audio_source(sources, QStringLiteral("default"))->id == "core:1");
  SYNC_REQUIRE(!choose_audio_source(sources, QStringLiteral("Loopback")).has_value());
  SYNC_REQUIRE(!choose_audio_source({}, QStringLiteral("default")).has_value());
}

SYNC_TEST(a_fixture_source_delivers_interleaved_samples) {
  // Sync's backend serves synthetic 32-channel sources under this switch, so
  // the capture path runs with no microphone and no permission prompt.
  qputenv("SYNC_AUDIO_TEST_FIXTURE", "1");
  AudioCapture capture(QStringLiteral("audio_32_tones"));
  QString error;
  SYNC_REQUIRE(capture.start(error));
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
  std::uint64_t frame_offset = 0;
  engine.set_before_render([&](nm::Backend& backend, nm::Graph&, quint64) {
    // One 60 fps frame of mono audio at 48 kHz: silence, or a full-scale
    // 60 Hz tone squarely in the bass band.
    std::vector<float> samples(800);
    for (std::size_t i = 0; i < samples.size(); ++i) {
      const double t = static_cast<double>(frame_offset + i) / 48000.0;
      samples[i] = loud ? static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 60.0 * t)) : 0.0f;
    }
    frame_offset += samples.size();
    input.pushDefault(samples.data(), static_cast<qsizetype>(samples.size()), 1);
    input.update();
    backend.setAudioState(input.snapshot());
  });
  engine.set_program(compiled.graph);
  engine.freeze_time(0.0);

  for (int i = 0; i < 30; ++i) engine.tick();
  const QImage quiet = engine.read_surface();
  loud = true;
  for (int i = 0; i < 30; ++i) engine.tick();
  const QImage bass = engine.read_surface();
  SYNC_REQUIRE(quiet != bass);
  // And back: in silence the band falls away and the frame returns to the
  // quiet one once the smoothing has decayed.
  loud = false;
  for (int i = 0; i < 120; ++i) engine.tick();
  SYNC_REQUIRE(engine.read_surface() == quiet);
}

}  // namespace
