#include "../test_harness.hpp"

#include <sync/platform/spout_publisher.hpp>
#include <sync/protocol.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// This suite runs on Windows CI machines with NO Spout installed (that is
// the whole point of the runtime-discovery boundary -- see
// docs/dependencies/spout.md). SpoutFramePublisher::available() is
// therefore false for every publisher constructed below, which means every
// call into open_sender/publish is guaranteed to fail regardless of input.
// What these tests can and do prove in that environment:
//   - discovery itself never crashes, including with a bogus explicit path;
//   - the public API is a safe, total no-op surface when unavailable;
//   - the shape/content validation that runs unconditionally (ahead of the
//     availability check -- see the comments in spout_publisher.cpp) does
//     not crash or misbehave across every documented boundary, even though
//     its *result* is masked by unavailability either way.
// Behavioural coverage of a live Spout install (does SetSenderName/SendImage
// actually work, is bInvert the right sense, etc.) is out of scope here by
// construction and belongs to manual/on-device verification.

namespace noisefactor::sync {
namespace {

using protocol::FrameView;

auto make_valid_frame(std::span<const std::byte> payload,
                      std::uint32_t width = 1,
                      std::uint32_t height = 1,
                      std::uint32_t row_stride = 4) -> FrameView {
  return {
      .version = 1,
      .header_bytes = 64,
      .flags = 1,
      .pixel_format = 1,
      .color_space = 1,
      .alpha_mode = 3,
      .width = width,
      .height = height,
      .row_stride = row_stride,
      .payload_bytes = static_cast<std::uint32_t>(payload.size()),
      .sequence = 1,
      .presentation_time_us = 1,
      .top_down = true,
      .payload = payload,
  };
}

auto repeated(std::size_t length, char fill) -> std::string {
  return std::string(length, fill);
}

}  // namespace

SYNC_TEST(spout_publisher_with_bogus_explicit_path_reports_unavailable_and_does_not_crash) {
  SpoutFramePublisher::Options options;
  options.library_path = "Z:\\this\\path\\does\\not\\exist\\SpoutLibrary.dll";
  SpoutFramePublisher publisher(options);
  SYNC_REQUIRE(!publisher.available());
}

SYNC_TEST(spout_publisher_default_construction_is_unavailable_on_a_machine_without_spout) {
  SpoutFramePublisher publisher;
  SYNC_REQUIRE(!publisher.available());
}

SYNC_TEST(unavailable_spout_publisher_rejects_open_sender_and_publish_and_close_is_a_no_op) {
  SpoutFramePublisher publisher;
  SYNC_REQUIRE(!publisher.available());

  SYNC_REQUIRE(!publisher.open_sender("sender-1", "Display Name"));

  const std::array<std::byte, 4> payload{
      std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const FrameView frame = make_valid_frame(payload);
  SYNC_REQUIRE(publisher.publish("sender-1", frame) == PublishResult::Failed);

  // No sender was ever opened, so this is a no-op; it must not crash.
  publisher.close_sender("sender-1");
  publisher.close_sender("some-other-unknown-id");

  SYNC_REQUIRE(!publisher.poll_failure(0).has_value());
}

// Every assertion here is `!open_sender(...)`, which an unavailable
// publisher satisfies on its own, so this test proves absence of a crash
// rather than distinguishing valid input from invalid. It is still worth
// running: the validators execute before the availability check precisely
// so this exercises them, and a bad-bounds change that faulted or read out
// of range would be caught here on a CI machine with no Spout installed.
// The lengths below therefore also serve as documentation of the real
// contract: sender_id is bounded at 128 to match the server, and the name
// at 64 because it is what other applications display.
SYNC_TEST(spout_publisher_sender_id_and_name_validation_boundaries_do_not_crash) {
  SpoutFramePublisher publisher;
  SYNC_REQUIRE(!publisher.available());

  const std::string name1(1, 'n');
  const std::string name64 = repeated(64, 'n');
  const std::string name65 = repeated(65, 'n');
  const std::string id1(1, 'i');
  const std::string id128 = repeated(128, 'i');
  const std::string id129 = repeated(129, 'i');
  const std::string with_control_char = std::string("bad") + '\x01' + "name";
  const std::string with_embedded_nul = std::string("bad") + '\0' + "name";

  // Empty.
  SYNC_REQUIRE(!publisher.open_sender("", "Name"));
  SYNC_REQUIRE(!publisher.open_sender("id", ""));

  // 1 byte (shortest valid length).
  SYNC_REQUIRE(!publisher.open_sender(id1, "Name"));
  SYNC_REQUIRE(!publisher.open_sender("id", name1));

  // Longest valid length: 128 bytes for the id, 64 for the name.
  SYNC_REQUIRE(!publisher.open_sender(id128, "Name"));
  SYNC_REQUIRE(!publisher.open_sender("id", name64));

  // One past the longest valid length.
  SYNC_REQUIRE(!publisher.open_sender(id129, "Name"));
  SYNC_REQUIRE(!publisher.open_sender("id", name65));

  // Embedded control character (0x01).
  SYNC_REQUIRE(!publisher.open_sender(with_control_char, "Name"));
  SYNC_REQUIRE(!publisher.open_sender("id", with_control_char));

  // Embedded NUL: std::string carries the length explicitly, so this
  // exercises a byte value of 0x00 in the middle of an otherwise
  // well-formed identifier, not premature truncation.
  SYNC_REQUIRE(!publisher.open_sender(with_embedded_nul, "Name"));
  SYNC_REQUIRE(!publisher.open_sender("id", with_embedded_nul));
}

SYNC_TEST(spout_publisher_frame_validation_rejects_every_malformed_shape) {
  SpoutFramePublisher publisher;
  SYNC_REQUIRE(!publisher.available());

  const std::array<std::byte, 4> payload{
      std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};

  // A well-formed 1x1 frame is itself rejected (publisher unavailable), but
  // must not crash -- this is the control case the malformed variants below
  // are diffed against.
  SYNC_REQUIRE(publisher.publish("sender", make_valid_frame(payload)) == PublishResult::Failed);

  // Bad version.
  {
    FrameView frame = make_valid_frame(payload);
    frame.version = 2;
    SYNC_REQUIRE(publisher.publish("sender", frame) == PublishResult::Failed);
  }

  // Bad header_bytes.
  {
    FrameView frame = make_valid_frame(payload);
    frame.header_bytes = 32;
    SYNC_REQUIRE(publisher.publish("sender", frame) == PublishResult::Failed);
  }

  // Bad flags (missing the top-down bit).
  {
    FrameView frame = make_valid_frame(payload);
    frame.flags = 0;
    SYNC_REQUIRE(publisher.publish("sender", frame) == PublishResult::Failed);
  }

  // top_down false (protocol requires top-down rows).
  {
    FrameView frame = make_valid_frame(payload);
    frame.top_down = false;
    SYNC_REQUIRE(publisher.publish("sender", frame) == PublishResult::Failed);
  }

  // Bad pixel format (only RGBA8 == 1 is supported).
  {
    FrameView frame = make_valid_frame(payload);
    frame.pixel_format = 2;
    SYNC_REQUIRE(publisher.publish("sender", frame) == PublishResult::Failed);
  }

  // Bad color space.
  {
    FrameView frame = make_valid_frame(payload);
    frame.color_space = 3;
    SYNC_REQUIRE(publisher.publish("sender", frame) == PublishResult::Failed);
  }

  // Bad alpha mode.
  {
    FrameView frame = make_valid_frame(payload);
    frame.alpha_mode = 0;
    SYNC_REQUIRE(publisher.publish("sender", frame) == PublishResult::Failed);
  }

  // Zero width.
  {
    FrameView frame = make_valid_frame(payload);
    frame.width = 0;
    SYNC_REQUIRE(publisher.publish("sender", frame) == PublishResult::Failed);
  }

  // Zero height.
  {
    FrameView frame = make_valid_frame(payload);
    frame.height = 0;
    SYNC_REQUIRE(publisher.publish("sender", frame) == PublishResult::Failed);
  }

  // Oversized width (well beyond the 4096 bound).
  {
    FrameView frame = make_valid_frame(payload);
    frame.width = 1U << 20;
    SYNC_REQUIRE(publisher.publish("sender", frame) == PublishResult::Failed);
  }

  // Oversized height.
  {
    FrameView frame = make_valid_frame(payload);
    frame.height = 1U << 20;
    SYNC_REQUIRE(publisher.publish("sender", frame) == PublishResult::Failed);
  }

  // Stride below the packed width (width * 4).
  {
    FrameView frame = make_valid_frame(payload);
    frame.row_stride = 3;
    SYNC_REQUIRE(publisher.publish("sender", frame) == PublishResult::Failed);
  }

  // Payload size disagrees with row_stride * height (payload_bytes field).
  {
    FrameView frame = make_valid_frame(payload);
    frame.payload_bytes = 3;
    SYNC_REQUIRE(publisher.publish("sender", frame) == PublishResult::Failed);
  }

  // Payload size disagrees with row_stride * height (actual span size).
  {
    const std::array<std::byte, 3> short_payload{std::byte{1}, std::byte{2}, std::byte{3}};
    FrameView frame = make_valid_frame(short_payload);
    frame.payload_bytes = 4;  // Claims the size of the well-formed case above.
    SYNC_REQUIRE(publisher.publish("sender", frame) == PublishResult::Failed);
  }
}

SYNC_TEST(spout_publisher_poll_failure_returns_nullopt_on_a_fresh_publisher) {
  SpoutFramePublisher publisher;
  SYNC_REQUIRE(!publisher.poll_failure(0).has_value());
  SYNC_REQUIRE(!publisher.poll_failure(123456).has_value());
}

}  // namespace noisefactor::sync
