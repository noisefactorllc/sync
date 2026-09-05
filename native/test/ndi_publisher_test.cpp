#include "test_harness.hpp"

#include "../src/ndi_frame_copy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <sync/frame_receiver.hpp>
#include <sync/platform/ndi_publisher.hpp>

// Discovery and validation tests use an unavailable runtime. The pixel tests
// exercise the row-copy helper used by publish(), so they verify the submitted
// RGBA layout and alpha conversion without installing the vendor's runtime.

namespace {

using noisefactor::sync::NdiFramePublisher;
using noisefactor::sync::NdiUnavailableReason;
using noisefactor::sync::PublishResult;
using noisefactor::sync::protocol::FrameView;

// A well-formed 2x2 RGBA8 frame: version 1, 64-byte header, top-down flag
// set, pixel_format 1 (RGBA8), color_space 1 (BT.709-ish default used
// elsewhere in this codebase), alpha_mode 3, packed stride (width * 4).
[[nodiscard]] auto make_valid_frame(std::span<const std::byte> payload) noexcept -> FrameView {
  return {
      .version = 1,
      .header_bytes = 64,
      .flags = 1,
      .pixel_format = 1,
      .color_space = 1,
      .alpha_mode = 3,
      .width = 2,
      .height = 2,
      .row_stride = 8,
      .payload_bytes = 16,
      .sequence = 1,
      .presentation_time_us = 1'000,
      .top_down = true,
      .payload = payload,
  };
}

[[nodiscard]] auto make_name(std::size_t length, char fill = 'n') -> std::string {
  return std::string(length, fill);
}

void require_copied_pixels(std::uint16_t alpha_mode,
                           const std::array<std::uint8_t, 16>& pixels,
                           const std::array<std::uint8_t, 16>& expected) {
  // Exercise both tightly packed input and input with padding after each row.
  for (const std::uint32_t stride : {8U, 12U}) {
    std::array<std::uint8_t, 24> source{};
    source.fill(0xde);
    for (std::size_t row = 0; row < 2; ++row) {
      for (std::size_t column = 0; column < 8; ++column) {
        source[row * stride + column] = pixels[row * 8 + column];
      }
    }
    const auto original_source = source;
    FrameView frame = make_valid_frame(std::as_bytes(std::span(source)).first(stride * 2U));
    frame.alpha_mode = alpha_mode;
    frame.row_stride = stride;
    frame.payload_bytes = stride * 2U;

    // Guard both ends of the exact packed destination and preserve the source.
    std::array<std::byte, 18> destination{};
    destination.fill(std::byte{0xac});
    noisefactor::sync::ndi::copy_rgba_frame(frame, destination.data() + 1);
    SYNC_REQUIRE(destination.front() == std::byte{0xac});
    SYNC_REQUIRE(destination.back() == std::byte{0xac});
    SYNC_REQUIRE(source == original_source);
    for (std::size_t index = 0; index < expected.size(); ++index) {
      SYNC_REQUIRE(std::to_integer<std::uint8_t>(destination[index + 1]) == expected[index]);
    }
  }
}

}  // namespace

SYNC_TEST(ndi_publisher_copies_straight_alpha_without_changing_rgb_or_alpha) {
  const std::array<std::uint8_t, 16> pixels = {
      255, 0, 0, 128, 3, 111, 249, 1, 10, 20, 30, 0, 11, 203, 99, 255};
  require_copied_pixels(2, pixels, pixels);
}

SYNC_TEST(ndi_publisher_converts_premultiplied_pixels_to_straight_alpha) {
  require_copied_pixels(3,
                       {128, 0, 0, 128, 3, 1, 0, 3, 10, 20, 30, 0, 11, 203, 99, 255},
                       {255, 0, 0, 128, 255, 85, 0, 3, 0, 0, 0, 0, 11, 203, 99, 255});
}

SYNC_TEST(ndi_publisher_treats_opaque_alpha_bytes_as_ignored) {
  require_copied_pixels(1,
                       {128, 0, 0, 128, 3, 111, 249, 1, 10, 20, 30, 0, 11, 203, 99, 255},
                       {128, 0, 0, 255, 3, 111, 249, 255, 10, 20, 30, 255, 11, 203, 99, 255});
}

SYNC_TEST(ndi_publisher_unpremultiplication_rounds_and_saturates_channels) {
  require_copied_pixels(3,
                       {1, 2, 3, 128, 64, 128, 255, 128, 1, 2, 3, 1, 126, 1, 0, 127},
                       {2, 4, 6, 128, 128, 255, 255, 128, 255, 255, 255, 1, 253, 2, 0, 127});
}

SYNC_TEST(ndi_publisher_with_bogus_explicit_runtime_path_reports_unavailable_without_crashing) {
  NdiFramePublisher::Options options{.runtime_path = "Z:/definitely/not/a/real/ndi/runtime/dir"};
  NdiFramePublisher publisher(options);

  SYNC_REQUIRE(!publisher.available());
  SYNC_REQUIRE(publisher.unavailable_reason() == NdiUnavailableReason::LoadFailed);
  SYNC_REQUIRE(!publisher.poll_failure(0).has_value());
}

SYNC_TEST(ndi_publisher_default_construction_reports_unavailable_without_crashing) {
  NdiFramePublisher publisher;
  SYNC_REQUIRE(!publisher.available());
  SYNC_REQUIRE(publisher.unavailable_reason() != NdiUnavailableReason::None);
}

SYNC_TEST(ndi_unavailability_reasons_are_distinct_and_non_secret) {
  const std::array reasons = {
      NdiUnavailableReason::RuntimeNotFound,
      NdiUnavailableReason::LoadFailed,
      NdiUnavailableReason::EntryPointMissing,
      NdiUnavailableReason::InitializationFailed,
      NdiUnavailableReason::UnsupportedCpu,
  };
  std::array<std::string_view, reasons.size()> descriptions{};
  for (std::size_t index = 0; index < reasons.size(); ++index) {
    descriptions[index] = noisefactor::sync::describe(reasons[index]);
    SYNC_REQUIRE(!descriptions[index].empty());
    SYNC_REQUIRE(descriptions[index].find("token") == std::string_view::npos);
    for (std::size_t prior = 0; prior < index; ++prior) {
      SYNC_REQUIRE(descriptions[index] != descriptions[prior]);
    }
  }
}

SYNC_TEST(ndi_publisher_unavailable_rejects_open_sender_fails_publish_and_no_ops_close) {
  NdiFramePublisher publisher;
  SYNC_REQUIRE(!publisher.available());

  SYNC_REQUIRE(!publisher.open_sender("sender-1", "Sender One"));

  const std::array<std::byte, 16> payload{};
  const FrameView frame = make_valid_frame(payload);
  SYNC_REQUIRE(publisher.publish("sender-1", frame) == PublishResult::Failed);

  // close_sender on an id that was never (and could never be, given
  // unavailability) opened must be a safe no-op.
  publisher.close_sender("sender-1");
  publisher.close_sender("");
  publisher.close_sender(std::string(200, 'x'));
}

SYNC_TEST(ndi_publisher_open_sender_name_boundaries_never_crash) {
  NdiFramePublisher publisher;

  // Empty name: rejected.
  SYNC_REQUIRE(!publisher.open_sender("id", ""));
  // 1-byte name: the smallest accepted length were the provider available.
  SYNC_REQUIRE(!publisher.open_sender("id", make_name(1)));
  // Exactly at the 64-byte NDI name limit.
  SYNC_REQUIRE(!publisher.open_sender("id", make_name(64)));
  // One byte over the limit.
  SYNC_REQUIRE(!publisher.open_sender("id", make_name(65)));

  // Embedded control character (e.g. a bell) anywhere in the name.
  std::string with_control = make_name(8);
  with_control[3] = '\x07';
  SYNC_REQUIRE(!publisher.open_sender("id", with_control));

  // Embedded NUL: string_view carries an explicit length, so this must not
  // be silently truncated at the NUL by anything downstream (e.g. strlen).
  std::string with_nul = make_name(8);
  with_nul[4] = '\0';
  SYNC_REQUIRE(!publisher.open_sender("id", std::string_view(with_nul.data(), with_nul.size())));
}

SYNC_TEST(ndi_publisher_open_sender_id_boundaries_never_crash) {
  NdiFramePublisher publisher;

  SYNC_REQUIRE(!publisher.open_sender("", "Name"));
  SYNC_REQUIRE(!publisher.open_sender(make_name(1), "Name"));
  SYNC_REQUIRE(!publisher.open_sender(make_name(128), "Name"));
  SYNC_REQUIRE(!publisher.open_sender(make_name(129), "Name"));

  std::string id_with_control = make_name(8);
  id_with_control[0] = '\x1b';
  SYNC_REQUIRE(!publisher.open_sender(id_with_control, "Name"));

  std::string id_with_nul = make_name(8);
  id_with_nul[5] = '\0';
  SYNC_REQUIRE(!publisher.open_sender(std::string_view(id_with_nul.data(), id_with_nul.size()), "Name"));
}

SYNC_TEST(ndi_publisher_publish_rejects_each_malformed_frame_shape) {
  NdiFramePublisher publisher;
  const std::array<std::byte, 16> payload{};
  const FrameView valid = make_valid_frame(payload);

  // The "control" case is itself rejected too, since the publisher is never
  // available in this environment — this documents that baseline alongside
  // every intentionally-malformed variant below.
  SYNC_REQUIRE(publisher.publish("sender", valid) == PublishResult::Failed);

  FrameView bad_version = valid;
  bad_version.version = 2;
  SYNC_REQUIRE(publisher.publish("sender", bad_version) == PublishResult::Failed);

  FrameView bad_pixel_format = valid;
  bad_pixel_format.pixel_format = 9;
  SYNC_REQUIRE(publisher.publish("sender", bad_pixel_format) == PublishResult::Failed);

  FrameView zero_width = valid;
  zero_width.width = 0;
  SYNC_REQUIRE(publisher.publish("sender", zero_width) == PublishResult::Failed);

  FrameView zero_height = valid;
  zero_height.height = 0;
  SYNC_REQUIRE(publisher.publish("sender", zero_height) == PublishResult::Failed);

  FrameView stride_too_small = valid;
  stride_too_small.row_stride = 4;  // Below packed width (2 * 4 = 8 bytes).
  SYNC_REQUIRE(publisher.publish("sender", stride_too_small) == PublishResult::Failed);

  FrameView payload_mismatch = valid;
  payload_mismatch.payload_bytes = 15;  // Disagrees with row_stride * height.
  SYNC_REQUIRE(publisher.publish("sender", payload_mismatch) == PublishResult::Failed);

  const std::array<std::byte, 8> short_payload{};
  FrameView payload_span_mismatch = valid;
  payload_span_mismatch.payload = short_payload;  // span.size() != payload_bytes.
  SYNC_REQUIRE(publisher.publish("sender", payload_span_mismatch) == PublishResult::Failed);

  FrameView oversized_width = valid;
  oversized_width.width = 5000;  // Exceeds the 4096 bound.
  SYNC_REQUIRE(publisher.publish("sender", oversized_width) == PublishResult::Failed);

  FrameView oversized_height = valid;
  oversized_height.height = 5000;
  SYNC_REQUIRE(publisher.publish("sender", oversized_height) == PublishResult::Failed);

  FrameView not_top_down = valid;
  not_top_down.top_down = false;
  SYNC_REQUIRE(publisher.publish("sender", not_top_down) == PublishResult::Failed);
}

SYNC_TEST(ndi_publisher_poll_failure_is_nullopt_on_a_fresh_publisher) {
  NdiFramePublisher publisher;
  SYNC_REQUIRE(!publisher.poll_failure(0).has_value());
  SYNC_REQUIRE(!publisher.poll_failure(123'456).has_value());

  NdiFramePublisher::Options options{.runtime_path = "also/not/real"};
  NdiFramePublisher explicit_publisher(options);
  SYNC_REQUIRE(!explicit_publisher.poll_failure(0).has_value());
}

SYNC_TEST(ndi_publisher_close_sender_is_idempotent_and_order_independent) {
  NdiFramePublisher publisher;
  // None of these ever opened anything (the provider is unavailable), but
  // close_sender must remain a safe, repeatable no-op regardless of call
  // order or repetition.
  publisher.close_sender("a");
  publisher.close_sender("a");
  publisher.close_sender("b");
  publisher.close_sender("a");
}
