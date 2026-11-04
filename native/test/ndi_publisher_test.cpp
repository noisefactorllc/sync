#include "test_harness.hpp"

#include "../src/ndi_frame_copy.hpp"
#include "../src/ndi_frame_rate.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// ---------------------------------------------------------------------------
// Declared frame rate. The NDI SDK defines frame_rate_N / frame_rate_D as the
// rate of the current frame; Sync frames carry none, so each sender declares
// the cadence of its own presentation times.
// ---------------------------------------------------------------------------

namespace {

using noisefactor::sync::ndi::FrameRate;
using noisefactor::sync::ndi::FrameRateTracker;
using noisefactor::sync::ndi::kInitialFrameRate;

[[nodiscard]] auto frames_per_second(FrameRate rate) -> double {
  return static_cast<double>(rate.numerator) / rate.denominator;
}

// A 16x8 opaque straight-alpha frame: the top half red, the bottom half blue.
struct StubFrame {
  static constexpr std::uint32_t kWidth = 16;
  static constexpr std::uint32_t kHeight = 8;
  std::array<std::byte, kWidth * kHeight * 4> pixels{};

  StubFrame() {
    for (std::uint32_t row = 0; row < kHeight; ++row) {
      for (std::uint32_t column = 0; column < kWidth; ++column) {
        std::byte* pixel = pixels.data() + (row * kWidth + column) * 4;
        const bool top = row < kHeight / 2;
        pixel[0] = top ? std::byte{255} : std::byte{0};
        pixel[1] = std::byte{0};
        pixel[2] = top ? std::byte{0} : std::byte{255};
        pixel[3] = std::byte{255};
      }
    }
  }

  [[nodiscard]] auto view(std::uint64_t presentation_time_us, std::uint64_t sequence) const
      -> FrameView {
    FrameView frame = make_valid_frame(std::span(pixels));
    frame.alpha_mode = 2;
    frame.width = kWidth;
    frame.height = kHeight;
    frame.row_stride = kWidth * 4;
    frame.payload_bytes = static_cast<std::uint32_t>(pixels.size());
    frame.sequence = sequence;
    frame.presentation_time_us = presentation_time_us;
    return frame;
  }
};

struct LoggedFrame {
  int width = 0;
  int height = 0;
  long four_cc = 0;
  int rate_n = 0;
  int rate_d = 0;
  int stride = 0;
  std::array<unsigned, 4> first_pixel{};
  std::array<unsigned, 4> last_row_pixel{};
};

void set_stub_log(const std::string& path) {
#if defined(_WIN32)
  SYNC_REQUIRE(_putenv_s("SYNC_NDI_STUB_LOG", path.c_str()) == 0);
#else
  SYNC_REQUIRE(setenv("SYNC_NDI_STUB_LOG", path.c_str(), 1) == 0);
#endif
}

// Publishes `count` frames `interval_us` apart through the stub runtime and
// returns the descriptors it logged.
[[nodiscard]] auto publish_through_stub(std::uint64_t interval_us, std::size_t count)
    -> std::vector<LoggedFrame> {
  const std::filesystem::path log =
      std::filesystem::temp_directory_path() /
      ("sync-ndi-stub-" + std::to_string(interval_us) + "-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
  set_stub_log(log.string());
  {
    NdiFramePublisher publisher({.runtime_path = SYNC_NDI_STUB_RUNTIME_DIR});
    SYNC_REQUIRE(publisher.available());
    SYNC_REQUIRE(publisher.open_sender("sender", "Cadence probe"));
    const StubFrame frame;
    for (std::size_t index = 0; index < count; ++index) {
      SYNC_REQUIRE(publisher.publish("sender", frame.view(1'000'000 + index * interval_us, index)) ==
                   PublishResult::Accepted);
    }
    publisher.close_sender("sender");
  }
  set_stub_log("");
  std::vector<LoggedFrame> frames;
  std::ifstream input(log);
  std::string call;
  while (input >> call) {
    if (call != "async") {
      std::string rest;
      std::getline(input, rest);
      continue;
    }
    LoggedFrame frame;
    long long timecode = 0;
    char comma = 0;
    input >> frame.width >> frame.height >> frame.four_cc >> frame.rate_n >> frame.rate_d >>
        frame.stride >> timecode;
    for (std::size_t channel = 0; channel < 4; ++channel) {
      if (channel > 0) input >> comma;
      input >> frame.first_pixel[channel];
    }
    for (std::size_t channel = 0; channel < 4; ++channel) {
      if (channel > 0) input >> comma;
      input >> frame.last_row_pixel[channel];
    }
    frames.push_back(frame);
  }
  input.close();
  std::filesystem::remove(log);
  return frames;
}

void require_cadence(std::uint64_t interval_us, double low_fps, double high_fps) {
  constexpr std::size_t kFrames = 120;
  const std::vector<LoggedFrame> frames = publish_through_stub(interval_us, kFrames);
  SYNC_REQUIRE(frames.size() == kFrames);
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const LoggedFrame& frame = frames[index];
    SYNC_REQUIRE(frame.width == static_cast<int>(StubFrame::kWidth));
    SYNC_REQUIRE(frame.height == static_cast<int>(StubFrame::kHeight));
    // NDI_LIB_FOURCC('R', 'G', 'B', 'A').
    SYNC_REQUIRE(frame.four_cc == 0x41424752L);
    SYNC_REQUIRE(frame.stride == static_cast<int>(StubFrame::kWidth * 4));
    // Top row first: the first row is red and the last row is blue.
    SYNC_REQUIRE((frame.first_pixel == std::array<unsigned, 4>{255, 0, 0, 255}));
    SYNC_REQUIRE((frame.last_row_pixel == std::array<unsigned, 4>{0, 0, 255, 255}));
    const double fps = static_cast<double>(frame.rate_n) / frame.rate_d;
    if (index == 0) {
      // The first frame has no interval yet.
      SYNC_REQUIRE((FrameRate{frame.rate_n, frame.rate_d} == kInitialFrameRate));
    } else {
      SYNC_REQUIRE(fps >= low_fps && fps <= high_fps);
    }
  }
}

}  // namespace

SYNC_TEST(ndi_publisher_declares_a_60_fps_cadence_through_a_stub_runtime) {
  require_cadence(16'667, 59.4, 60.6);
}

SYNC_TEST(ndi_publisher_declares_a_30_fps_cadence_through_a_stub_runtime) {
  require_cadence(33'333, 29.7, 30.3);
}

SYNC_TEST(ndi_frame_rate_tracker_declares_standard_rates_for_steady_cadences) {
  const std::array<std::pair<std::uint64_t, FrameRate>, 5> cases{{
      {16'667, {60, 1}}, {33'333, {30, 1}}, {16'683, {60000, 1001}},
      {20'000, {50, 1}}, {41'708, {24000, 1001}},
  }};
  for (const auto& [interval, expected] : cases) {
    FrameRateTracker tracker;
    SYNC_REQUIRE(tracker.observe(0) == kInitialFrameRate);
    FrameRate declared{};
    for (std::uint64_t index = 1; index < 30; ++index) declared = tracker.observe(index * interval);
    SYNC_REQUIRE(declared == expected);
  }
}

SYNC_TEST(ndi_frame_rate_tracker_ignores_jitter_drops_and_pauses) {
  FrameRateTracker tracker;
  std::uint64_t time = 0;
  FrameRate declared = tracker.observe(time);
  // A browser's capture timestamps wobble by a few milliseconds.
  const std::array<std::int64_t, 6> jitter{3'000, -2'500, 1'200, -3'000, 2'000, -700};
  for (std::size_t index = 0; index < 60; ++index) {
    time = static_cast<std::uint64_t>(static_cast<std::int64_t>(time) + 16'667 +
                                      jitter[index % jitter.size()]);
    declared = tracker.observe(time);
    if (index >= 8) SYNC_REQUIRE(frames_per_second(declared) > 59.4 &&
                                 frames_per_second(declared) < 60.6);
  }
  const FrameRate steady = declared;
  // One dropped frame doubles a single interval.
  declared = tracker.observe(time += 2 * 16'667);
  SYNC_REQUIRE(declared == steady);
  // A pause longer than a second is not the cadence.
  declared = tracker.observe(time += 5'000'000);
  SYNC_REQUIRE(declared == steady);
  // A timestamp that does not advance adds no interval.
  declared = tracker.observe(time);
  SYNC_REQUIRE(declared == steady);
}

SYNC_TEST(ndi_frame_rate_tracker_declares_an_unusual_cadence_as_measured) {
  FrameRateTracker tracker;
  FrameRate declared{};
  for (std::uint64_t index = 0; index < 20; ++index) declared = tracker.observe(index * 12'345);
  SYNC_REQUIRE(declared == (FrameRate{1'000'000, 12'345}));
}
