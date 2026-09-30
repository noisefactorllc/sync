#include "test_harness.hpp"

#include <QColor>
#include <QFile>
#include <QCoreApplication>
#include <QDir>
#include <QImage>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "media_inputs.h"
#include "program_compiler.h"
#include "render_engine.h"

namespace {

using namespace noisefactor::sync::render_helper;

const QString kMedia = QStringLiteral("search synth\nmedia().write(o0)\nrender(o0)\n");

[[nodiscard]] auto data_root() -> QString { return QStringLiteral(SYNC_RENDER_TEST_DATA_ROOT); }

[[nodiscard]] auto temp_path(const char* name) -> QString {
  return QDir(QDir::tempPath())
      .filePath(QStringLiteral("sync-render-media-test-%1-%2")
                    .arg(QCoreApplication::applicationPid())
                    .arg(QString::fromLatin1(name)));
}

SYNC_TEST(media_specs_name_a_camera_or_a_file) {
  const auto camera = parse_media_spec(QStringLiteral("camera"));
  SYNC_REQUIRE(camera.has_value() && camera->kind == MediaSpec::Kind::Camera && camera->value.isEmpty());
  const auto named = parse_media_spec(QStringLiteral("camera:FaceTime"));
  SYNC_REQUIRE(named.has_value() && named->value == QStringLiteral("FaceTime"));
  const auto file = parse_media_spec(QStringLiteral("file:/tmp/a b.mov"));
  SYNC_REQUIRE(file.has_value() && file->kind == MediaSpec::Kind::File &&
               file->value == QStringLiteral("/tmp/a b.mov"));
  SYNC_REQUIRE(!parse_media_spec(QStringLiteral("camera:")).has_value());
  SYNC_REQUIRE(!parse_media_spec(QStringLiteral("file:")).has_value());
  SYNC_REQUIRE(!parse_media_spec(QStringLiteral("/tmp/a.png")).has_value());
}

SYNC_TEST(media_steps_take_sources_in_step_order_and_share_the_last) {
  const QStringList ids{QStringLiteral("textTex_step_1"), QStringLiteral("imageTex_step_4"),
                        QStringLiteral("imageTex_step_2"), QStringLiteral("imageTex_step_7"),
                        QStringLiteral("imageTex_step_2")};
  const auto one = bind_media(ids, 1);
  SYNC_REQUIRE(one.size() == 3);
  SYNC_REQUIRE(one[0].texture_id == QStringLiteral("imageTex_step_2") && one[0].step_index == 2);
  SYNC_REQUIRE(one[1].step_index == 4 && one[2].step_index == 7);
  for (const auto& b : one) SYNC_REQUIRE(b.source_index == 0);
  const auto two = bind_media(ids, 2);
  SYNC_REQUIRE(two[0].source_index == 0 && two[1].source_index == 1 && two[2].source_index == 1);
  SYNC_REQUIRE(bind_media(ids, 0).empty());
}

SYNC_TEST(a_missing_media_file_is_a_startup_error) {
  MediaInputs inputs({MediaSpec{MediaSpec::Kind::File, temp_path("missing.png")}});
  QString error;
  SYNC_REQUIRE(!inputs.start(error));
  SYNC_REQUIRE(error.contains(QStringLiteral("not found")));
}

SYNC_TEST(an_image_file_reaches_the_media_step_at_its_real_size) {
  // A wide image, so a wrong imageSize (the 1024x1024 default) would distort
  // the placement and leave the centre on the background instead.
  const QString png = temp_path("wide.png");
  QImage image(96, 32, QImage::Format_RGBA8888);
  image.fill(QColor(200, 40, 30));
  SYNC_REQUIRE(image.save(png));

  ProgramCompiler compiler(data_root());
  RenderEngine::Options options;
  options.size = QSize(64, 48);
  options.data_root = data_root();
  options.ring_name = temp_path("media.frames").toStdString();
  RenderEngine engine(options);
  QString error;
  SYNC_REQUIRE(engine.start(error));

  MediaInputs inputs({MediaSpec{MediaSpec::Kind::File, png}});
  SYNC_REQUIRE(inputs.start(error));
  engine.set_before_render([&](nm::Backend& backend, nm::Graph& graph, quint64 generation) {
    inputs.apply(backend, graph, generation, compiler.registry());
  });

  // Without a source the media step samples transparent black.
  RenderEngine bare(RenderEngine::Options{options.size, 60.0, 10.0, data_root(),
                                          temp_path("bare.frames").toStdString()});
  SYNC_REQUIRE(bare.start(error));
  bare.set_program(compiler.compile(kMedia).graph);
  bare.freeze_time(0.0);
  bare.tick();
  const QColor dark = bare.read_surface().pixelColor(32, 24);
  SYNC_REQUIRE(dark.red() < 20 && dark.green() < 20 && dark.blue() < 20);

  auto graph = compiler.compile(kMedia).graph;
  SYNC_REQUIRE(graph != nullptr);
  engine.set_program(graph);
  engine.freeze_time(0.0);
  engine.tick();
  const QImage out = engine.read_surface();
  const QColor centre = out.pixelColor(32, 24);
  SYNC_REQUIRE(std::abs(centre.red() - 200) <= 2);
  SYNC_REQUIRE(std::abs(centre.green() - 40) <= 2);
  SYNC_REQUIRE(std::abs(centre.blue() - 30) <= 2);

  // A recompiled program (the controller edited something) gets the image
  // and its size again, not the definition's default.
  engine.set_program(compiler.compile(kMedia).graph);
  engine.tick();
  const QColor again = engine.read_surface().pixelColor(32, 24);
  SYNC_REQUIRE(std::abs(again.red() - 200) <= 2);

  QFile::remove(png);
}

SYNC_TEST(shared_images_render_without_local_sources_and_survive_recompilation) {
  ProgramCompiler compiler(data_root());
  RenderEngine::Options options{QSize(64, 48), 60.0, 10.0, data_root(),
                               temp_path("shared.frames").toStdString()};
  RenderEngine engine(options);
  QString error;
  SYNC_REQUIRE(engine.start(error));
  MediaInputs inputs({});
  SYNC_REQUIRE(inputs.start(error));
  engine.set_before_render([&](nm::Backend& backend, nm::Graph& graph, quint64 generation) {
    inputs.apply(backend, graph, generation, compiler.registry());
  });
  QImage image(96, 32, QImage::Format_RGBA8888);
  image.fill(QColor(10, 190, 60));
  inputs.set_shared_images({{0, image}});
  for (int i = 0; i < 2; ++i) {
    engine.set_program(compiler.compile(kMedia).graph);
    engine.freeze_time(0);
    engine.tick();
    const auto pixel = engine.read_surface().pixelColor(32, 24);
    SYNC_REQUIRE(std::abs(pixel.green() - 190) <= 2);
    SYNC_REQUIRE(std::abs(pixel.red() - 10) <= 2);
  }
  inputs.set_shared_images({});
  engine.set_program(compiler.compile(kMedia).graph);
  engine.tick();
  SYNC_REQUIRE(engine.read_surface().pixelColor(32, 24).green() < 20);
}

SYNC_TEST(shared_image_slots_are_not_overwritten_by_local_media) {
  const QString path = temp_path("local-with-shared.png");
  QImage local(96, 32, QImage::Format_RGBA8888), shared(96, 32, QImage::Format_RGBA8888);
  local.fill(Qt::red); shared.fill(Qt::green);
  SYNC_REQUIRE(local.save(path));
  ProgramCompiler compiler(data_root());
  RenderEngine::Options options{QSize(64, 48), 60.0, 10.0, data_root(),
                               temp_path("mixed.frames").toStdString()};
  RenderEngine engine(options);
  QString error;
  SYNC_REQUIRE(engine.start(error));
  MediaInputs inputs({MediaSpec{MediaSpec::Kind::File, path}});
  SYNC_REQUIRE(inputs.start(error));
  inputs.set_shared_images({{0, shared}});
  engine.set_before_render([&](nm::Backend& backend, nm::Graph& graph, quint64 generation) {
    inputs.apply(backend, graph, generation, compiler.registry());
  });
  engine.set_program(compiler.compile(kMedia).graph);
  engine.tick();
  SYNC_REQUIRE(engine.read_surface().pixelColor(32, 24).green() > 250);
  inputs.set_shared_images({});
  engine.set_program(compiler.compile(kMedia).graph);
  engine.tick();
  SYNC_REQUIRE(engine.read_surface().pixelColor(32, 24).red() > 250);
  QFile::remove(path);
}

SYNC_TEST(removing_a_shared_image_clears_it_while_local_video_has_no_frame) {
  const QString path = temp_path("no-frames.mp4");
  QFile video(path);
  SYNC_REQUIRE(video.open(QIODevice::WriteOnly));
  video.write("no decodable video frames");
  video.close();
  ProgramCompiler compiler(data_root());
  RenderEngine engine(RenderEngine::Options{QSize(64, 48), 60.0, 10.0, data_root(),
                                            temp_path("waiting.frames").toStdString()});
  QString error;
  SYNC_REQUIRE(engine.start(error));
  MediaInputs inputs({MediaSpec{MediaSpec::Kind::File, path}});
  SYNC_REQUIRE(inputs.start(error));
  QImage image(96, 32, QImage::Format_RGBA8888);
  image.fill(Qt::green);
  inputs.set_shared_images({{0, image}});
  engine.set_before_render([&](nm::Backend& backend, nm::Graph& graph, quint64 generation) {
    inputs.apply(backend, graph, generation, compiler.registry());
  });
  engine.set_program(compiler.compile(kMedia).graph);
  engine.tick();
  SYNC_REQUIRE(engine.read_surface().pixelColor(32, 24).green() > 250);
  inputs.set_shared_images({});
  engine.set_program(compiler.compile(kMedia).graph);
  engine.tick();
  SYNC_REQUIRE(engine.read_surface().pixelColor(32, 24).green() < 20);
  QFile::remove(path);
}

}  // namespace
