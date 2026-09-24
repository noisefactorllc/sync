#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <filesystem>
#include <unistd.h>
#endif

#include <sync/render/render_source.hpp>

namespace {

using namespace noisefactor::sync;
using namespace noisefactor::sync::render;

constexpr RenderRingGeometry kGeometry{.width = 32, .height = 16,
                                       .alpha_mode = kRenderAlphaPremultiplied};

class RecordingPublisher final : public FramePublisher {
 public:
  auto open_sender(std::string_view id, std::string_view name) noexcept -> bool override {
    opened.emplace_back(id);
    names.emplace_back(name);
    return accept_open;
  }
  void close_sender(std::string_view id) noexcept override { closed.emplace_back(id); }
  auto publish(std::string_view, const protocol::FrameView& frame) noexcept
      -> PublishResult override {
    frames.push_back(frame.sequence);
    first_bytes.push_back(frame.payload.empty() ? 0 : static_cast<int>(frame.payload[0]));
    last = frame;
    return next_result;
  }
  std::vector<std::string> opened, names, closed;
  std::vector<std::uint64_t> frames;
  std::vector<int> first_bytes;
  protocol::FrameView last{};
  bool accept_open = true;
  PublishResult next_result = PublishResult::Accepted;
};

[[nodiscard]] auto ring_name(const char* tag) -> std::string {
#if defined(_WIN32)
  return "SyncRenderSourceTest-" + std::to_string(::GetCurrentProcessId()) + "-" + tag;
#else
  return (std::filesystem::temp_directory_path() /
          ("sync-render-source-test-" + std::to_string(::getpid()) + "-" + tag))
      .string();
#endif
}

struct Writer {
  explicit Writer(const std::string& name) {
    std::string error;
    section = RenderRingSection::create(name, *render_ring_bytes(kGeometry), error);
    SYNC_REQUIRE(section.has_value());
    writer.emplace(section->bytes(), kGeometry, 42);
    SYNC_REQUIRE(writer->valid());
  }
  void write(std::uint8_t value) {
    std::vector<std::byte> frame(std::size_t{kGeometry.width} * 4 * kGeometry.height,
                                 static_cast<std::byte>(value));
    SYNC_REQUIRE(writer->write(frame, kGeometry.width * 4, render_clock_us()));
  }
  std::optional<RenderRingSection> section;
  std::optional<RenderRingWriter> writer;
};

SYNC_TEST(attaching_opens_one_sender_and_publishes_each_new_frame_once) {
  const std::string name = ring_name("publish");
  Writer writer(name);
  RecordingPublisher publisher;
  RenderSource source(publisher, "render", "Noisedeck Render");
  std::string error;
  SYNC_REQUIRE(source.attach(name, error));
  SYNC_REQUIRE(publisher.opened.size() == 1);
  SYNC_REQUIRE(publisher.names[0] == "Noisedeck Render");

  const std::uint64_t now = render_clock_us();
  SYNC_REQUIRE(source.poll(now) == RenderSource::Poll::NoFrame);
  writer.write(11);
  SYNC_REQUIRE(source.poll(now) == RenderSource::Poll::Published);
  SYNC_REQUIRE(source.poll(now) == RenderSource::Poll::Repeat);
  SYNC_REQUIRE(publisher.frames == std::vector<std::uint64_t>{1});
  SYNC_REQUIRE(publisher.first_bytes == std::vector<int>{11});

  // The frame reaches providers exactly as a browser RGBA frame would.
  const protocol::FrameView& frame = publisher.last;
  SYNC_REQUIRE(frame.pixel_format == kRenderPixelFormatRgba8);
  SYNC_REQUIRE(frame.top_down);
  SYNC_REQUIRE(frame.width == kGeometry.width);
  SYNC_REQUIRE(frame.height == kGeometry.height);
  SYNC_REQUIRE(frame.row_stride == kGeometry.width * 4);
  SYNC_REQUIRE(frame.payload_bytes == kGeometry.width * 4 * kGeometry.height);
  SYNC_REQUIRE(frame.alpha_mode == kRenderAlphaPremultiplied);
  SYNC_REQUIRE(frame.color_space == kRenderColorSpaceSrgb);
}

SYNC_TEST(frames_the_poll_never_saw_are_counted_as_skipped) {
  const std::string name = ring_name("skipped");
  Writer writer(name);
  RecordingPublisher publisher;
  RenderSource source(publisher, "render", "Render");
  std::string error;
  SYNC_REQUIRE(source.attach(name, error));
  const std::uint64_t now = render_clock_us();
  writer.write(1);
  SYNC_REQUIRE(source.poll(now) == RenderSource::Poll::Published);
  writer.write(2);
  writer.write(3);
  writer.write(4);
  SYNC_REQUIRE(source.poll(now) == RenderSource::Poll::Published);
  SYNC_REQUIRE(publisher.frames == (std::vector<std::uint64_t>{1, 4}));
  SYNC_REQUIRE(source.stats().skipped_frames == 2);
  SYNC_REQUIRE(source.stats().published == 2);
}

SYNC_TEST(a_backpressured_frame_is_retried_by_the_next_poll) {
  const std::string name = ring_name("backpressure");
  Writer writer(name);
  RecordingPublisher publisher;
  RenderSource source(publisher, "render", "Render");
  std::string error;
  SYNC_REQUIRE(source.attach(name, error));
  const std::uint64_t now = render_clock_us();
  writer.write(5);
  publisher.next_result = PublishResult::Backpressured;
  SYNC_REQUIRE(source.poll(now) == RenderSource::Poll::Backpressured);
  publisher.next_result = PublishResult::Accepted;
  SYNC_REQUIRE(source.poll(now) == RenderSource::Poll::Published);
  SYNC_REQUIRE(publisher.frames == (std::vector<std::uint64_t>{1, 1}));
  SYNC_REQUIRE(source.stats().backpressured == 1);
}

SYNC_TEST(a_closed_writer_closes_the_sender_and_detaches) {
  const std::string name = ring_name("closed");
  Writer writer(name);
  RecordingPublisher publisher;
  RenderSource source(publisher, "render", "Render");
  std::string error;
  SYNC_REQUIRE(source.attach(name, error));
  writer.writer->close();
  SYNC_REQUIRE(source.poll(render_clock_us()) == RenderSource::Poll::WriterGone);
  SYNC_REQUIRE(publisher.closed == std::vector<std::string>{"render"});
  SYNC_REQUIRE(!source.attached());
  SYNC_REQUIRE(source.poll(render_clock_us()) == RenderSource::Poll::Detached);
}

SYNC_TEST(a_stalled_writer_is_treated_as_gone) {
  const std::string name = ring_name("stalled");
  Writer writer(name);
  RecordingPublisher publisher;
  RenderSource source(publisher, "render", "Render", 1'000'000);
  std::string error;
  SYNC_REQUIRE(source.attach(name, error));
  const std::uint64_t now = render_clock_us();
  writer.writer->heartbeat(now);
  SYNC_REQUIRE(source.poll(now + 500'000) == RenderSource::Poll::NoFrame);
  SYNC_REQUIRE(source.poll(now + 1'500'000) == RenderSource::Poll::WriterGone);
  SYNC_REQUIRE(publisher.closed.size() == 1);
}

SYNC_TEST(a_refused_sender_leaves_the_source_detached) {
  const std::string name = ring_name("refused");
  Writer writer(name);
  RecordingPublisher publisher;
  publisher.accept_open = false;
  RenderSource source(publisher, "render", "Render");
  std::string error;
  SYNC_REQUIRE(!source.attach(name, error));
  SYNC_REQUIRE(!source.attached());
  SYNC_REQUIRE(publisher.closed.empty());
}

SYNC_TEST(a_missing_section_is_an_attach_error_not_a_crash) {
  RecordingPublisher publisher;
  RenderSource source(publisher, "render", "Render");
  std::string error;
  SYNC_REQUIRE(!source.attach(ring_name("missing"), error));
  SYNC_REQUIRE(!error.empty());
  SYNC_REQUIRE(publisher.opened.empty());
}

}  // namespace
