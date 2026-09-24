#include "test_harness.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QImage>

#include <string>
#include <vector>

#include <runtime/midi_state.h>

#include "midi_input.h"
#include "program_compiler.h"
#include "render_engine.h"

namespace {

using namespace noisefactor::sync::render_helper;

// The gradient's rotation follows CC 7 on channel 1 over a quarter turn
// (mode 5 is 7-bit control change; the default mode reads note velocity).
// Not -180..180: those ends are the same angle, which only float rounding
// on some GPUs would tell apart.
const QString kMidiRotation = QStringLiteral(
    "search synth\n"
    "let angle = midi(channel: 1, mode: 5, cc: 7, min: 0, max: 90)\n"
    "gradient(seed: 1, rotation: angle).write(o0)\n"
    "render(o0)\n");

[[nodiscard]] auto data_root() -> QString { return QStringLiteral(SYNC_RENDER_TEST_DATA_ROOT); }

SYNC_TEST(midi_port_ids_stay_unique_and_stable) {
  const auto ports = midi_port_ids({QStringLiteral("IAC Bus 1"), QStringLiteral("nanoKONTROL2"),
                                    QStringLiteral("IAC Bus 1")});
  SYNC_REQUIRE(ports.size() == 3);
  SYNC_REQUIRE(ports[0].id == QStringLiteral("IAC Bus 1"));
  SYNC_REQUIRE(ports[1].id == QStringLiteral("nanoKONTROL2"));
  SYNC_REQUIRE(ports[2].id == QStringLiteral("IAC Bus 1 #2"));
  SYNC_REQUIRE(ports[2].name == QStringLiteral("IAC Bus 1"));
}

SYNC_TEST(midi_filters_match_names_case_insensitively) {
  SYNC_REQUIRE(midi_port_wanted(QStringLiteral("nanoKONTROL2"), {}));
  SYNC_REQUIRE(midi_port_wanted(QStringLiteral("nanoKONTROL2"), {QStringLiteral("kontrol")}));
  SYNC_REQUIRE(!midi_port_wanted(QStringLiteral("IAC Bus 1"), {QStringLiteral("kontrol")}));
}

SYNC_TEST(a_control_change_moves_a_midi_driven_parameter) {
  ProgramCompiler compiler(data_root());
  const auto compiled = compiler.compile(kMidiRotation);
  SYNC_REQUIRE(compiled.graph != nullptr);

  RenderEngine::Options options;
  options.size = QSize(64, 48);
  options.data_root = data_root();
  options.ring_name = QDir(QDir::tempPath())
                          .filePath(QStringLiteral("sync-render-midi-test-%1.frames")
                                        .arg(QCoreApplication::applicationPid()))
                          .toStdString();
  RenderEngine engine(options);
  QString error;
  SYNC_REQUIRE(engine.start(error));

  nm::MidiState state;
  std::vector<MidiInput::Message> pending;
  engine.set_before_render([&](nm::Backend& backend, nm::Graph&, quint64) {
    (void)feed_midi(state, pending);
    pending.clear();
    backend.setMidiState(state.snapshot());
  });
  engine.set_program(compiled.graph);
  engine.freeze_time(0.0);

  pending.push_back({{0xB0, 7, 0}, QStringLiteral("test"), QStringLiteral("test")});
  engine.tick();
  const QImage low = engine.read_surface();
  pending.push_back({{0xB0, 7, 127}, QStringLiteral("test"), QStringLiteral("test")});
  engine.tick();
  const QImage high = engine.read_surface();
  SYNC_REQUIRE(low != high);

  // A message on another channel leaves the parameter where it was.
  pending.push_back({{0xB1, 7, 0}, QStringLiteral("test"), QStringLiteral("test")});
  engine.tick();
  SYNC_REQUIRE(engine.read_surface() == high);
}

SYNC_TEST(open_ports_reach_the_engine_port_registry) {
  nm::MidiState state;
  sync_midi_ports(state, {{QStringLiteral("a"), QStringLiteral("Pad")},
                          {QStringLiteral("b"), QStringLiteral("Keys")}});
  const auto ports = state.ports();
  SYNC_REQUIRE(ports.size() == 2);
  sync_midi_ports(state, {{QStringLiteral("b"), QStringLiteral("Keys")}});
  int connected = 0;
  for (const auto& port : state.ports()) connected += port.connected ? 1 : 0;
  SYNC_REQUIRE(connected == 1);
}

}  // namespace
