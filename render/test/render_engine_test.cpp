#include "test_harness.hpp"

#include <QCoreApplication>
#include <QImage>

#include <cstring>
#include <filesystem>
#include <string>

#include "program_compiler.h"
#include "render_engine.h"

#include <sync/render/render_ring.hpp>

namespace {

using namespace noisefactor::sync;
using namespace noisefactor::sync::render_helper;

// Rotated, so the image is neither vertically nor horizontally symmetric: a
// flipped or mirrored frame cannot match the reference readback by accident.
const QString kGradient = QStringLiteral("search synth\ngradient(seed: 1, rotation: 45).write(o0)\nrender(o0)\n");
const QString kPerlin = QStringLiteral("search synth\nperlin().write(o0)\nrender(o0)\n");

[[nodiscard]] auto data_root() -> QString { return QStringLiteral(SYNC_RENDER_TEST_DATA_ROOT); }

[[nodiscard]] auto ring_name(const char* tag) -> std::string {
#if defined(_WIN32)
  return "SyncRenderEngineTest-" + std::to_string(QCoreApplication::applicationPid()) + "-" + tag;
#else
  return (std::filesystem::temp_directory_path() /
          ("sync-render-engine-test-" + std::to_string(QCoreApplication::applicationPid()) + "-" +
           tag))
      .string();
#endif
}

[[nodiscard]] auto engine_options(const char* tag) -> RenderEngine::Options {
  RenderEngine::Options options;
  options.size = QSize(64, 48);
  options.data_root = data_root();
  options.ring_name = ring_name(tag);
  return options;
}

SYNC_TEST(the_compiler_loads_the_catalogue_and_reports_errors_as_data) {
  ProgramCompiler compiler(data_root());
  SYNC_REQUIRE(compiler.effect_count() > 200);
  const auto good = compiler.compile(kGradient);
  SYNC_REQUIRE(good.graph != nullptr);
  SYNC_REQUIRE(good.error.isEmpty());
  const auto syntax = compiler.compile(QStringLiteral("gradient(.write(o0)"));
  SYNC_REQUIRE(syntax.graph == nullptr);
  SYNC_REQUIRE(!syntax.error.isEmpty());
  const auto unknown = compiler.compile(QStringLiteral("search synth\nnotAnEffect().write(o0)\nrender(o0)\n"));
  SYNC_REQUIRE(unknown.graph == nullptr);
  SYNC_REQUIRE(!unknown.error.isEmpty());
}

SYNC_TEST(shared_media_references_compile_without_becoming_shader_parameters) {
  ProgramCompiler compiler(data_root());
  const QString id(64, QLatin1Char('a'));
  const auto result = compiler.compile(QStringLiteral(
      "search synth\nmedia(url: \"image:%1\", scaleAmt: 50).write(o0)\nrender(o0)\n").arg(id));
  SYNC_REQUIRE(result.graph != nullptr);
  SYNC_REQUIRE(result.error.isEmpty());
  SYNC_REQUIRE(result.shared_images.value(0) == id);
}

SYNC_TEST(shared_media_binding_uses_validated_steps_for_inline_chains) {
  ProgramCompiler compiler(data_root());
  const QString first(64, QLatin1Char('a')), second(64, QLatin1Char('b'));
  const auto result = compiler.compile(QStringLiteral(
      "search synth, mixer\n"
      "gradient().blendMode(tex: media(url: 'image:%1', scaleAmt: 50), mode: mix).write(o0)\n"
      "media(scaleAmt: 30, url: \"image:%2\").write(o1)\nrender(o0)\n").arg(first, second));
  SYNC_REQUIRE(result.graph != nullptr);
  SYNC_REQUIRE(result.shared_images.size() == 2);
  SYNC_REQUIRE(result.shared_images.value(1) == first);
  SYNC_REQUIRE(result.shared_images.value(4) == second);
}

SYNC_TEST(shared_media_urls_are_literal_image_references_and_nonmedia_errors_survive) {
  ProgramCompiler compiler(data_root());
  for (const QString& value : {QStringLiteral("https://remote.test/image.png"),
                               QStringLiteral("data:image/png;base64,AAAA"),
                               QStringLiteral("image:short"), QStringLiteral("video:abc")}) {
    const auto result = compiler.compile(QStringLiteral(
        "search synth\nmedia(url: \"%1\").write(o0)\nrender(o0)\n").arg(value));
    SYNC_REQUIRE(result.graph == nullptr);
    SYNC_REQUIRE(!result.error.isEmpty());
  }
  const auto unrelated = compiler.compile(QStringLiteral(
      "search synth\ngradient(url: \"image:%1\").write(o0)\nrender(o0)\n")
      .arg(QString(64, QLatin1Char('a'))));
  SYNC_REQUIRE(unrelated.graph == nullptr);
  const auto comment = compiler.compile(QStringLiteral(
      "search synth\n// media(url: \"invalid\")\ngradient().write(o0)\nrender(o0)\n"));
  SYNC_REQUIRE(comment.graph != nullptr);
  SYNC_REQUIRE(comment.shared_images.isEmpty());
}

SYNC_TEST(shared_media_stripping_preserves_explicit_other_namespace_errors) {
  ProgramCompiler compiler(data_root());
  const auto result = compiler.compile(QStringLiteral(
      "search synth\nmedia(url: \"image:%1\").write(o0)\n"
      "from(filter, media(url: \"genuine parameter\")).write(o1)\nrender(o0)\n")
      .arg(QString(64, QLatin1Char('a'))));
  SYNC_REQUIRE(result.graph == nullptr);
  SYNC_REQUIRE(result.error.contains(QStringLiteral("Unknown effect")));
}

SYNC_TEST(shared_media_stripping_uses_the_outermost_namespace_override) {
  ProgramCompiler compiler(data_root());
  const auto id = QString(64, QLatin1Char('a'));
  const auto result = compiler.compile(QStringLiteral(
      "search synth\nfrom(synth, from(filter, media(url: \"image:%1\"))).write(o0)\nrender(o0)\n")
      .arg(id));
  SYNC_REQUIRE(result.graph != nullptr);
  SYNC_REQUIRE(result.shared_images.size() == 1);
  SYNC_REQUIRE(result.shared_images.value(0) == id);
}

SYNC_TEST(a_frame_in_the_ring_is_the_backends_own_top_down_readback) {
  ProgramCompiler compiler(data_root());
  RenderEngine engine(engine_options("readback"));
  QString error;
  SYNC_REQUIRE(engine.start(error));
  engine.set_program(compiler.compile(kGradient).graph);
  engine.freeze_time(0.25);
  engine.tick();
  engine.drain();
  engine.tick();  // delivers the readback the drain completed, if poll had not already
  SYNC_REQUIRE(engine.frames_written() >= 1);

  std::string ring_error;
  auto section = render::RenderRingSection::open(engine.options().ring_name, ring_error);
  SYNC_REQUIRE(section.has_value());
  render::RenderRingReader reader(section->bytes());
  SYNC_REQUIRE(reader.valid());
  SYNC_REQUIRE(reader.geometry().width == 64);
  SYNC_REQUIRE(reader.geometry().height == 48);
  SYNC_REQUIRE(reader.geometry().alpha_mode == render::kRenderAlphaPremultiplied);
  render::RenderFrameLease lease = reader.acquire();
  SYNC_REQUIRE(lease.held());

  // The backend's readSurface() is documented top-down RGBA8 (backend.h).
  // The gradient is opaque, so premultiplication leaves it unchanged and the
  // ring must match it byte for byte.
  const QImage reference = engine.read_surface().convertToFormat(QImage::Format_RGBA8888);
  SYNC_REQUIRE(reference.width() == 64 && reference.height() == 48);
  const auto payload = lease.payload();
  bool opaque = true;
  bool identical = true;
  bool varied = false;
  const auto first = payload[0];
  for (int y = 0; y < 48; ++y) {
    const auto* ref = reference.constScanLine(y);
    const auto* got = reinterpret_cast<const unsigned char*>(payload.data()) + y * 64 * 4;
    if (std::memcmp(ref, got, 64 * 4) != 0) identical = false;
    for (int x = 0; x < 64; ++x) {
      if (ref[x * 4 + 3] != 255) opaque = false;
      if (static_cast<std::byte>(got[x * 4]) != first) varied = true;
    }
  }
  SYNC_REQUIRE(opaque);
  SYNC_REQUIRE(varied);
  SYNC_REQUIRE(identical);
}

SYNC_TEST(swapping_programs_changes_the_output) {
  ProgramCompiler compiler(data_root());
  RenderEngine engine(engine_options("swap"));
  QString error;
  SYNC_REQUIRE(engine.start(error));
  engine.freeze_time(0.5);
  engine.set_program(compiler.compile(kGradient).graph);
  engine.tick();
  const QImage gradient = engine.read_surface();
  engine.set_program(compiler.compile(kPerlin).graph);
  engine.tick();
  const QImage perlin = engine.read_surface();
  SYNC_REQUIRE(gradient != perlin);
}

SYNC_TEST(an_overlay_trace_does_not_stop_the_picture) {
  // fibers traces its overlay on the CPU: about 0.4 s at this size, seconds
  // at 1080p. The tick that meets the new program returns while the trace
  // still runs, and a later tick shows the finished overlay.
  ProgramCompiler compiler(data_root());
  RenderEngine::Options options = engine_options("overlay");
  options.size = QSize(640, 360);
  RenderEngine engine(options);
  QString error;
  SYNC_REQUIRE(engine.start(error));
  engine.freeze_time(0.5);
  const auto fibers = compiler.compile(QStringLiteral(
      "search classicNoisedeck, filter\nnoise(seed: 1).fibers().write(o0)\nrender(o0)\n"));
  SYNC_REQUIRE(fibers.graph != nullptr);
  engine.set_program(fibers.graph);
  engine.tick();
  SYNC_REQUIRE(engine.overlay_traces_pending());
  const QImage before = engine.read_surface();
  engine.wait_for_overlay_traces();
  engine.tick();
  SYNC_REQUIRE(!engine.overlay_traces_pending());
  SYNC_REQUIRE(engine.read_surface() != before);
}

SYNC_TEST(no_program_writes_nothing_but_keeps_the_ring_alive) {
  RenderEngine engine(engine_options("idle"));
  QString error;
  SYNC_REQUIRE(engine.start(error));
  engine.tick();
  engine.tick();
  SYNC_REQUIRE(engine.frames_written() == 0);
  std::string ring_error;
  auto section = render::RenderRingSection::open(engine.options().ring_name, ring_error);
  SYNC_REQUIRE(section.has_value());
  render::RenderRingReader reader(section->bytes());
  SYNC_REQUIRE(reader.valid());
  SYNC_REQUIRE(reader.newest_frame() == 0);
  SYNC_REQUIRE(reader.writer_alive(render::render_clock_us(), 1'000'000));
}

SYNC_TEST(stopping_closes_the_ring_for_the_reader) {
  ProgramCompiler compiler(data_root());
  RenderEngine engine(engine_options("stop"));
  QString error;
  SYNC_REQUIRE(engine.start(error));
  std::string ring_error;
  auto section = render::RenderRingSection::open(engine.options().ring_name, ring_error);
  SYNC_REQUIRE(section.has_value());
  render::RenderRingReader reader(section->bytes());
  SYNC_REQUIRE(!reader.writer_closed());
  engine.stop();
  SYNC_REQUIRE(reader.writer_closed());
}

SYNC_TEST(an_out_of_range_size_is_a_startup_error) {
  RenderEngine::Options options = engine_options("size");
  options.size = QSize(8192, 64);
  RenderEngine engine(options);
  QString error;
  SYNC_REQUIRE(!engine.start(error));
  SYNC_REQUIRE(!error.isEmpty());
}

}  // namespace
