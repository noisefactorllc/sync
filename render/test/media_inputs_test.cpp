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

// The OS camera stack, stood in for: the test plugs and unplugs the camera,
// delivers its frames, and reports its failures, so the reopen behavior the
// helper shows a lost or missing camera is observable without hardware.
struct FakeCamera {
  QList<CameraDevice> devices;
  int opens = 0;
  bool open_fails = false;
  bool stopped = false;
  std::function<void(const QVideoFrame&)> on_frame;
  std::function<void(const QString&)> on_error;
  std::function<void()> on_change;
};

class FakeCameraBackend final : public CameraBackend {
 public:
  explicit FakeCameraBackend(FakeCamera& camera) : camera_(camera) {}
  auto inputs() const -> QList<CameraDevice> override { return camera_.devices; }
  void watch(QObject*, std::function<void()> on_change) override {
    camera_.on_change = std::move(on_change);
  }
  auto check_permission() const -> CameraPermission::Status override {
    return CameraPermission::Status::Granted;
  }
  void request_permission(QObject*, std::function<void(CameraPermission::Status)> on_result)
      override {
    on_result(CameraPermission::Status::Granted);
  }
  auto open(const CameraDevice&, const std::function<void(const QVideoFrame&)>& on_frame,
            const std::function<void(const QString&)>& on_error)
      -> std::unique_ptr<CameraStream> override {
    ++camera_.opens;
    if (camera_.open_fails) return nullptr;
    camera_.on_frame = on_frame;
    camera_.on_error = on_error;
    class Stream final : public CameraStream {
     public:
      explicit Stream(FakeCamera& camera) : camera_(camera) {}
      void stop() override { camera_.stopped = true; }
      FakeCamera& camera_;
    };
    return std::make_unique<Stream>(camera_);
  }

 private:
  FakeCamera& camera_;

 public:
  void fire_change() {
    if (camera_.on_change) camera_.on_change();
  }
};

[[nodiscard]] auto red_frame(int width, int height, QColor color) -> QVideoFrame {
  QImage image(width, height, QImage::Format_RGBA8888);
  image.fill(color);
  return QVideoFrame(image);
}

SYNC_TEST(camera_devices_are_chosen_by_position_then_description_then_default) {
  const QList<CameraDevice> devices{{"cam-0", "FaceTime HD Camera"}, {"cam-1", "Scarlett Cam"}};
  SYNC_REQUIRE(choose_camera_device(devices, {}).value().description ==
               QStringLiteral("FaceTime HD Camera"));
  SYNC_REQUIRE(choose_camera_device(devices, QStringLiteral("1")).value().description ==
               QStringLiteral("Scarlett Cam"));
  SYNC_REQUIRE(choose_camera_device(devices, QStringLiteral("scarlett")).value().id ==
               QStringLiteral("cam-1"));
  SYNC_REQUIRE(choose_camera_device(devices, QStringLiteral("facetime")).value().id ==
               QStringLiteral("cam-0"));
  SYNC_REQUIRE(!choose_camera_device(devices, QStringLiteral("none")).has_value());
  SYNC_REQUIRE(!choose_camera_device({}, {}).has_value());
  SYNC_REQUIRE(!choose_camera_device({}, QStringLiteral("2")).has_value());
}

SYNC_TEST(a_camera_missing_at_start_is_waited_for_and_opened_when_it_appears) {
  ProgramCompiler compiler(data_root());
  RenderEngine engine(RenderEngine::Options{QSize(64, 48), 60.0, 10.0, data_root(),
                                            temp_path("camera-late.frames").toStdString()});
  QString error;
  SYNC_REQUIRE(engine.start(error));

  FakeCamera camera;
  auto backend = std::make_unique<FakeCameraBackend>(camera);
  FakeCameraBackend* backend_ptr = backend.get();
  MediaInputs inputs({MediaSpec{MediaSpec::Kind::Camera, {}}}, nullptr, std::move(backend));
  std::vector<std::pair<QString, QString>> statuses;
  QObject::connect(&inputs, &MediaInputs::status, &engine,
                   [&statuses](int, const QString& state, const QString& detail) {
                     statuses.push_back({state, detail});
                   });
  SYNC_REQUIRE(inputs.start(error));
  // Missing is a state, not a one-shot error: the source waits, and the
  // search keeps running.
  SYNC_REQUIRE(statuses.size() == 1);
  SYNC_REQUIRE(statuses[0].first == QStringLiteral("waiting"));
  SYNC_REQUIRE(statuses[0].second == QStringLiteral("no camera"));

  // A device-list change with still no camera is quiet.
  backend_ptr->fire_change();
  SYNC_REQUIRE(statuses.size() == 1);

  // The camera arrives: the same source opens it, without a helper restart.
  camera.devices.push_back({QStringLiteral("cam-1"), QStringLiteral("Test Camera")});
  backend_ptr->fire_change();
  SYNC_REQUIRE(camera.opens == 1);
  SYNC_REQUIRE(statuses.size() == 2);
  SYNC_REQUIRE(statuses[1].first == QStringLiteral("camera"));
  SYNC_REQUIRE(statuses[1].second == QStringLiteral("Test Camera"));

  // Its frames reach the media step, at the frame's real size.
  engine.set_before_render([&](nm::Backend& backend, nm::Graph& graph, quint64 generation) {
    inputs.apply(backend, graph, generation, compiler.registry());
  });
  engine.set_program(compiler.compile(kMedia).graph);
  engine.freeze_time(0.0);
  engine.tick();
  SYNC_REQUIRE(engine.read_surface().pixelColor(32, 24).red() < 20);
  camera.on_frame(red_frame(96, 32, QColor(200, 40, 30)));
  engine.tick();
  const QColor centre = engine.read_surface().pixelColor(32, 24);
  SYNC_REQUIRE(std::abs(centre.red() - 200) <= 2);
  SYNC_REQUIRE(std::abs(centre.green() - 40) <= 2);
  // A later frame replaces the earlier one.
  camera.on_frame(red_frame(96, 32, QColor(10, 190, 60)));
  engine.tick();
  SYNC_REQUIRE(engine.read_surface().pixelColor(32, 24).green() > 180);
}

SYNC_TEST(an_unplugged_camera_is_reopened_when_it_returns) {
  ProgramCompiler compiler(data_root());
  RenderEngine engine(RenderEngine::Options{QSize(64, 48), 60.0, 10.0, data_root(),
                                            temp_path("camera-unplug.frames").toStdString()});
  QString error;
  SYNC_REQUIRE(engine.start(error));

  FakeCamera camera;
  camera.devices.push_back({QStringLiteral("cam-1"), QStringLiteral("Test Camera")});
  auto backend = std::make_unique<FakeCameraBackend>(camera);
  FakeCameraBackend* backend_ptr = backend.get();
  MediaInputs inputs({MediaSpec{MediaSpec::Kind::Camera, {}}}, nullptr, std::move(backend));
  std::vector<std::pair<QString, QString>> statuses;
  QObject::connect(&inputs, &MediaInputs::status, &engine,
                   [&statuses](int, const QString& state, const QString& detail) {
                     statuses.push_back({state, detail});
                   });
  engine.set_before_render([&](nm::Backend& backend, nm::Graph& graph, quint64 generation) {
    inputs.apply(backend, graph, generation, compiler.registry());
  });
  engine.set_program(compiler.compile(kMedia).graph);
  engine.freeze_time(0.0);
  SYNC_REQUIRE(inputs.start(error));
  SYNC_REQUIRE(camera.opens == 1);
  SYNC_REQUIRE(statuses.back().first == QStringLiteral("camera"));

  camera.on_frame(red_frame(96, 32, QColor(200, 40, 30)));
  engine.tick();
  SYNC_REQUIRE(engine.read_surface().pixelColor(32, 24).red() > 180);

  // The driver reports the unplug: the stream is retired at once, the source
  // waits with the reason, and the picture keeps its last frame.
  camera.on_error(QStringLiteral("the camera was disconnected"));
  SYNC_REQUIRE(camera.stopped);
  SYNC_REQUIRE(statuses.back().first == QStringLiteral("waiting"));
  SYNC_REQUIRE(statuses.back().second == QStringLiteral("the camera was disconnected"));
  camera.devices.clear();
  backend_ptr->fire_change();
  SYNC_REQUIRE(camera.opens == 1);
  SYNC_REQUIRE(statuses.size() == 2);

  // The camera is plugged back in: the same source reopens it.
  camera.devices.push_back({QStringLiteral("cam-1"), QStringLiteral("Test Camera")});
  backend_ptr->fire_change();
  SYNC_REQUIRE(camera.opens == 2);
  SYNC_REQUIRE(statuses.size() == 3);
  SYNC_REQUIRE(statuses.back().first == QStringLiteral("camera"));

  // And its frames come back.
  camera.on_frame(red_frame(96, 32, QColor(10, 190, 60)));
  engine.tick();
  SYNC_REQUIRE(engine.read_surface().pixelColor(32, 24).green() > 180);
  camera.on_error = nullptr;
  camera.on_frame = nullptr;
}

SYNC_TEST(a_camera_that_fails_to_open_is_searched_for_again) {
  FakeCamera camera;
  camera.devices.push_back({QStringLiteral("cam-1"), QStringLiteral("Test Camera")});
  camera.open_fails = true;
  auto backend = std::make_unique<FakeCameraBackend>(camera);
  FakeCameraBackend* backend_ptr = backend.get();
  MediaInputs inputs({MediaSpec{MediaSpec::Kind::Camera, {}}}, nullptr, std::move(backend));
  std::vector<std::pair<QString, QString>> statuses;
  QObject::connect(&inputs, &MediaInputs::status,
                   [&statuses](int, const QString& state, const QString& detail) {
                     statuses.push_back({state, detail});
                   });
  QString error;
  SYNC_REQUIRE(inputs.start(error));
  SYNC_REQUIRE(statuses.back().first == QStringLiteral("waiting"));
  SYNC_REQUIRE(statuses.back().second == QStringLiteral("the camera did not open"));
  // The next device-list change tries the same camera again.
  camera.open_fails = false;
  backend_ptr->fire_change();
  SYNC_REQUIRE(camera.opens == 2);
  SYNC_REQUIRE(statuses.back().first == QStringLiteral("camera"));
  camera.on_frame = nullptr;
  camera.on_error = nullptr;
}

SYNC_TEST(a_camera_gone_from_the_device_list_is_retired_and_reopened) {
  FakeCamera camera;
  camera.devices.push_back({QStringLiteral("cam-1"), QStringLiteral("Test Camera")});
  auto backend = std::make_unique<FakeCameraBackend>(camera);
  FakeCameraBackend* backend_ptr = backend.get();
  MediaInputs inputs({MediaSpec{MediaSpec::Kind::Camera, {}}}, nullptr, std::move(backend));
  std::vector<std::pair<QString, QString>> statuses;
  QObject::connect(&inputs, &MediaInputs::status,
                   [&statuses](int, const QString& state, const QString& detail) {
                     statuses.push_back({state, detail});
                   });
  QString error;
  SYNC_REQUIRE(inputs.start(error));
  SYNC_REQUIRE(camera.opens == 1);
  // The stream never reported an error, but the machine's camera list no
  // longer carries the device: retire it, wait, and reopen when it is listed
  // again.
  camera.devices.clear();
  backend_ptr->fire_change();
  SYNC_REQUIRE(camera.stopped);
  SYNC_REQUIRE(statuses.back().first == QStringLiteral("waiting"));
  SYNC_REQUIRE(statuses.back().second == QStringLiteral("the camera left"));
  SYNC_REQUIRE(camera.opens == 1);
  camera.devices.push_back({QStringLiteral("cam-1"), QStringLiteral("Test Camera")});
  backend_ptr->fire_change();
  SYNC_REQUIRE(camera.opens == 2);
  SYNC_REQUIRE(statuses.back().first == QStringLiteral("camera"));
  camera.on_frame = nullptr;
  camera.on_error = nullptr;
}

}  // namespace
