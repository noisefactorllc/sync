#include "test_harness.hpp"

#include "../src/ndi_frame_copy.hpp"
#include "../src/ndi_frame_rate.hpp"
#include "../src/ndi_runtime_discovery.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

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

// No system location: only an explicit runtime_path is tried, so a runtime
// the host has installed cannot make these publishers available.
const std::vector<std::string> kNoSystemCandidates{};

[[nodiscard]] auto hermetic(NdiFramePublisher::Options options = {}) -> NdiFramePublisher::Options {
  options.system_candidates = &kNoSystemCandidates;
  return options;
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
  NdiFramePublisher publisher(hermetic({.runtime_path = "Z:/definitely/not/a/real/ndi/runtime/dir"}));

  SYNC_REQUIRE(!publisher.available());
  SYNC_REQUIRE(publisher.unavailable_reason() == NdiUnavailableReason::LoadFailed);
  SYNC_REQUIRE(!publisher.poll_failure(0).has_value());
}

SYNC_TEST(ndi_publisher_default_construction_reports_unavailable_without_crashing) {
  NdiFramePublisher publisher(hermetic());
  SYNC_REQUIRE(!publisher.available());
  SYNC_REQUIRE(publisher.unavailable_reason() != NdiUnavailableReason::None);
}

SYNC_TEST(ndi_runtime_candidates_take_the_system_locations_from_the_test_seam) {
  using noisefactor::sync::ndi::runtime_candidates;
  SYNC_REQUIRE(runtime_candidates("", &kNoSystemCandidates).empty());
  const std::vector<std::string> system{"first/runtime", "second/runtime"};
  SYNC_REQUIRE(runtime_candidates("", &system) == system);
  const std::vector<std::string> explicit_only = runtime_candidates("explicit/dir", &kNoSystemCandidates);
  SYNC_REQUIRE(explicit_only.size() == 1);
  SYNC_REQUIRE(explicit_only.front().rfind("explicit/dir", 0) == 0);
  const std::vector<std::string> both = runtime_candidates("explicit/dir", &system);
  SYNC_REQUIRE((both == std::vector<std::string>{explicit_only.front(), "first/runtime", "second/runtime"}));
}

SYNC_TEST(ndi_unavailability_reasons_are_distinct_and_non_secret) {
  const std::array reasons = {
      NdiUnavailableReason::RuntimeNotFound,
      NdiUnavailableReason::LoadFailed,
      NdiUnavailableReason::EntryPointMissing,
      NdiUnavailableReason::InitializationFailed,
      NdiUnavailableReason::UnsupportedCpu,
      NdiUnavailableReason::SignatureRejected,
      NdiUnavailableReason::RuntimeNotRootOwned,
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
  NdiFramePublisher publisher(hermetic());
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
  NdiFramePublisher publisher(hermetic());

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
  NdiFramePublisher publisher(hermetic());

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
  NdiFramePublisher publisher(hermetic());
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
  NdiFramePublisher publisher(hermetic());
  SYNC_REQUIRE(!publisher.poll_failure(0).has_value());
  SYNC_REQUIRE(!publisher.poll_failure(123'456).has_value());

  NdiFramePublisher explicit_publisher(hermetic({.runtime_path = "also/not/real"}));
  SYNC_REQUIRE(!explicit_publisher.poll_failure(0).has_value());
}

SYNC_TEST(ndi_publisher_close_sender_is_idempotent_and_order_independent) {
  NdiFramePublisher publisher(hermetic());
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

#if defined(__APPLE__)
// The stub is signed ad hoc under this identifier (CMakeLists.txt), so this
// requirement admits it where the NewTek pin would refuse it.
constexpr std::string_view kStubRequirement = "identifier \"" SYNC_NDI_STUB_IDENTIFIER "\"";
#else
constexpr std::string_view kStubRequirement{};
#endif

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
    NdiFramePublisher publisher(
        {.runtime_path = SYNC_NDI_STUB_RUNTIME_DIR, .signature_requirement = kStubRequirement});
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
    if (index < 2) {
      // The first two frames give one interval, too few to judge a cadence.
      SYNC_REQUIRE((FrameRate{frame.rate_n, frame.rate_d} == kInitialFrameRate));
    } else {
      SYNC_REQUIRE(fps >= low_fps && fps <= high_fps);
    }
  }
}

}  // namespace

#if defined(__APPLE__)
namespace {

[[nodiscard]] auto unique_log(std::string_view label) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() /
         ("sync-ndi-stub-" + std::string(label) + "-" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
}

[[nodiscard]] auto read_file(const std::filesystem::path& path) -> std::string {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace

SYNC_TEST(ndi_runtime_candidates_on_macos_are_the_flag_directory_then_the_vendor_install_path) {
  using noisefactor::sync::ndi::runtime_candidates;
  // The vendor's runtime directory variables add no candidate on macOS.
  SYNC_REQUIRE(setenv("NDI_RUNTIME_DIR_V6", "/tmp/sync-ndi-v6", 1) == 0);
  SYNC_REQUIRE(setenv("NDI_RUNTIME_DIR_V5", "/tmp/sync-ndi-v5", 1) == 0);
  const std::vector<std::string> with_flag = runtime_candidates("/opt/ndi");
  const std::vector<std::string> with_trailing_slash = runtime_candidates("/opt/ndi/");
  const std::vector<std::string> without_flag = runtime_candidates("");
  const std::vector<std::string> flag_is_vendor_dir = runtime_candidates("/usr/local/lib");
  SYNC_REQUIRE(unsetenv("NDI_RUNTIME_DIR_V6") == 0);
  SYNC_REQUIRE(unsetenv("NDI_RUNTIME_DIR_V5") == 0);
  SYNC_REQUIRE((with_flag == std::vector<std::string>{"/opt/ndi/libndi.dylib", "/usr/local/lib/libndi.dylib"}));
  SYNC_REQUIRE(with_trailing_slash == with_flag);
  SYNC_REQUIRE((without_flag == std::vector<std::string>{"/usr/local/lib/libndi.dylib"}));
  SYNC_REQUIRE((flag_is_vendor_dir == std::vector<std::string>{"/usr/local/lib/libndi.dylib"}));
}

// Declared before the control below, which loads the ad hoc copy: the tests
// run in declaration order, so here neither copy has ever been loaded.
SYNC_TEST(ndi_publisher_on_macos_refuses_unsigned_and_ad_hoc_runtimes_without_loading_them) {
  using noisefactor::sync::ndi::check_runtime_signature;
  using noisefactor::sync::ndi::RuntimeSignature;
  const std::string requirement = noisefactor::sync::ndi::vendor_runtime_requirement();
  SYNC_REQUIRE(requirement == "anchor apple generic and certificate leaf[subject.OU] = \"W8U66ET244\"");
  for (const std::string_view directory : {std::string_view(SYNC_NDI_STUB_UNSIGNED_DIR),
                                           std::string_view(SYNC_NDI_STUB_ADHOC_DIR)}) {
    const std::string library = std::string(directory) + "/libndi.dylib";
    SYNC_REQUIRE(std::filesystem::is_regular_file(library));
    SYNC_REQUIRE(check_runtime_signature(library, requirement) == RuntimeSignature::Rejected);
    const std::filesystem::path log = unique_log("refused");
    set_stub_log(log.string());
    {
      NdiFramePublisher publisher(hermetic({.runtime_path = directory}));
      SYNC_REQUIRE(!publisher.available());
      SYNC_REQUIRE(publisher.unavailable_reason() == NdiUnavailableReason::SignatureRejected);
    }
    set_stub_log("");
    // The stub writes "loaded" when it is mapped: no log means no dlopen.
    SYNC_REQUIRE(!std::filesystem::exists(log));
  }
}

SYNC_TEST(ndi_vendor_runtime_ownership_admits_only_root_owned_files_in_root_owned_directories) {
  using noisefactor::sync::ndi::check_runtime_ownership;
  using noisefactor::sync::ndi::RuntimeOwnership;
  // A system binary: root-owned, in a root-owned directory, neither writable
  // by group or others.
  SYNC_REQUIRE(check_runtime_ownership("/bin/ls") == RuntimeOwnership::RootOwned);
  SYNC_REQUIRE(check_runtime_ownership("/definitely/not/a/real/libndi.dylib") == RuntimeOwnership::Missing);

  const std::filesystem::path directory = unique_log("ownership");
  std::filesystem::create_directory(directory);
  const std::filesystem::path owned = directory / "libndi.dylib";
  { std::ofstream(owned) << "not a runtime"; }
  // This test runs as an ordinary user, so the file it made is not root's.
  SYNC_REQUIRE(::getuid() != 0);
  SYNC_REQUIRE(check_runtime_ownership(owned.string()) == RuntimeOwnership::Exposed);
  // A link to a root-owned file is still a link.
  const std::filesystem::path link = directory / "linked.dylib";
  SYNC_REQUIRE(::symlink("/bin/ls", link.c_str()) == 0);
  SYNC_REQUIRE(check_runtime_ownership(link.string()) == RuntimeOwnership::Exposed);
  std::filesystem::remove_all(directory);
  // A root-owned link in a root-owned directory to a root-owned file: only
  // the link itself makes it Exposed. macOS ships /usr/bin/vi as a link to vim.
  SYNC_REQUIRE(std::filesystem::is_symlink("/usr/bin/vi"));
  SYNC_REQUIRE(check_runtime_ownership(std::filesystem::canonical("/usr/bin/vi").string()) ==
               RuntimeOwnership::RootOwned);
  SYNC_REQUIRE(check_runtime_ownership("/usr/bin/vi") == RuntimeOwnership::Exposed);
}

SYNC_TEST(ndi_vendor_runtime_ownership_refuses_a_directory_others_can_write) {
  using noisefactor::sync::ndi::runtime_ownership;
  using noisefactor::sync::ndi::RuntimeOwnership;
  const auto status = [](mode_t mode, uid_t uid) {
    struct stat value {};
    value.st_mode = mode;
    value.st_uid = uid;
    return value;
  };
  const struct stat file = status(S_IFREG | 0755, 0);
  const struct stat directory = status(S_IFDIR | 0755, 0);
  SYNC_REQUIRE(runtime_ownership(file, directory) == RuntimeOwnership::RootOwned);
  // A world-writable directory such as /tmp, even with the sticky bit.
  SYNC_REQUIRE(runtime_ownership(file, status(S_IFDIR | S_ISVTX | 0777, 0)) == RuntimeOwnership::Exposed);
  SYNC_REQUIRE(runtime_ownership(file, status(S_IFDIR | 0775, 0)) == RuntimeOwnership::Exposed);
  // A directory a user owns, as Homebrew leaves /usr/local/lib on Intel Macs.
  SYNC_REQUIRE(runtime_ownership(file, status(S_IFDIR | 0755, 501)) == RuntimeOwnership::Exposed);
  SYNC_REQUIRE(runtime_ownership(status(S_IFREG | 0775, 0), directory) == RuntimeOwnership::Exposed);
  SYNC_REQUIRE(runtime_ownership(status(S_IFREG | 0757, 0), directory) == RuntimeOwnership::Exposed);
  SYNC_REQUIRE(runtime_ownership(status(S_IFREG | 0755, 501), directory) == RuntimeOwnership::Exposed);
  SYNC_REQUIRE(runtime_ownership(status(S_IFLNK | 0755, 0), directory) == RuntimeOwnership::Exposed);
  SYNC_REQUIRE(runtime_ownership(file, status(S_IFLNK | 0755, 0)) == RuntimeOwnership::Exposed);
}

SYNC_TEST(ndi_publisher_on_macos_loads_a_runtime_that_satisfies_its_requirement) {
  using noisefactor::sync::ndi::check_runtime_signature;
  using noisefactor::sync::ndi::RuntimeSignature;
  const std::string requirement(kStubRequirement);
  const std::string adhoc = std::string(SYNC_NDI_STUB_ADHOC_DIR) + "/libndi.dylib";
  SYNC_REQUIRE(check_runtime_signature(adhoc, requirement) == RuntimeSignature::Accepted);
  SYNC_REQUIRE(check_runtime_signature(std::string(SYNC_NDI_STUB_UNSIGNED_DIR) + "/libndi.dylib", requirement) ==
               RuntimeSignature::Rejected);
  SYNC_REQUIRE(check_runtime_signature("/definitely/not/a/real/libndi.dylib", requirement) ==
               RuntimeSignature::Missing);
  const std::filesystem::path log = unique_log("admitted");
  set_stub_log(log.string());
  {
    NdiFramePublisher publisher({.runtime_path = SYNC_NDI_STUB_ADHOC_DIR, .signature_requirement = kStubRequirement});
    SYNC_REQUIRE(publisher.available());
    SYNC_REQUIRE(publisher.unavailable_reason() == NdiUnavailableReason::None);
  }
  set_stub_log("");
  // The control for the refusal test: the load marker is written.
  SYNC_REQUIRE(read_file(log) == "loaded\n");
  std::filesystem::remove(log);
}
#endif

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

namespace {

// SplitMix64: the same jitter sequence on every platform and standard library.
struct Jitter {
  std::uint64_t state;
  auto next() -> std::uint64_t {
    std::uint64_t value = (state += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
  }
};

struct Declared {
  double seconds = 0.0;
  FrameRate rate{};
};

constexpr std::size_t kNoChange = std::numeric_limits<std::size_t>::max();

// A sender `period_us` apart (from frame `change_at` on, `next_period_us`),
// each presentation time moved by uniform capture jitter of up to 2 ms either
// way, about one frame in fifty dropped. Returns the rate declared on each
// frame that was sent, at its nominal time.
[[nodiscard]] auto declare_jittered(double period_us, std::size_t frames, std::uint64_t seed,
                                    std::size_t change_at = kNoChange, double next_period_us = 0.0)
    -> std::vector<Declared> {
  Jitter jitter{seed};
  FrameRateTracker tracker;
  std::vector<Declared> declared;
  double nominal_us = 0.0;
  for (std::size_t frame = 0; frame < frames; ++frame) {
    if (frame == change_at) period_us = next_period_us;
    if (frame > 0) nominal_us += period_us;
    const bool dropped = frame > 0 && jitter.next() % 50U == 0U;
    const double offset_us = static_cast<double>(jitter.next() % 4001U) - 2000.0;
    if (dropped) continue;
    const auto time_us = static_cast<std::uint64_t>(1'000'000'000.0 + nominal_us + offset_us);
    declared.push_back({nominal_us / 1'000'000.0, tracker.observe(time_us)});
  }
  return declared;
}

[[nodiscard]] auto changes(const std::vector<Declared>& declared) -> std::size_t {
  std::size_t count = 0;
  for (std::size_t index = 1; index < declared.size(); ++index) {
    if (!(declared[index].rate == declared[index - 1].rate)) ++count;
  }
  return count;
}

// The nominal time from which every declared rate is `rate`.
[[nodiscard]] auto settled_at(const std::vector<Declared>& declared, FrameRate rate) -> double {
  for (std::size_t index = declared.size(); index > 0; --index) {
    if (!(declared[index - 1].rate == rate)) {
      return index < declared.size() ? declared[index].seconds : std::numeric_limits<double>::infinity();
    }
  }
  return 0.0;
}

// Seeds 1 to 10, and 34 and 316: in simulation a confidence level of 1e-3
// for every decision declared 30000/1001 for a while on seed 34's 30 fps
// stream and a measured 65 fps on seed 316's third 60 fps frame.
constexpr std::array<std::uint64_t, 12> kSeeds{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 34, 316};

constexpr double kMicrosecondsPer60Fps = 1'000'000.0 / 60.0;
constexpr double kMicrosecondsPer30Fps = 1'000'000.0 / 30.0;
constexpr double kMicrosecondsPer59_94Fps = 1'000'000.0 * 1001.0 / 60'000.0;
constexpr double kMicrosecondsPer29_97Fps = 1'000'000.0 * 1001.0 / 30'000.0;

}  // namespace

// zig's live 60 fps run alternated between 60/1 and 60000/1001 because a
// short average of jittered frame times crossed the 0.05% boundary between
// them. A receiver must see one rate while the cadence holds.
SYNC_TEST(ndi_frame_rate_tracker_holds_60_and_30_fps_through_capture_jitter_and_drops) {
  for (const std::uint64_t seed : kSeeds) {
    const std::vector<Declared> sixty = declare_jittered(kMicrosecondsPer60Fps, 3600, seed);
    for (const Declared& frame : sixty) SYNC_REQUIRE((frame.rate == FrameRate{60, 1}));

    const std::vector<Declared> thirty = declare_jittered(kMicrosecondsPer30Fps, 1800, seed);
    SYNC_REQUIRE(thirty.front().rate == kInitialFrameRate);
    SYNC_REQUIRE(changes(thirty) == 1);
    SYNC_REQUIRE(settled_at(thirty, {30, 1}) <= 1.0);
  }
}

SYNC_TEST(ndi_frame_rate_tracker_settles_once_on_59_94_and_29_97_fps) {
  for (const std::uint64_t seed : kSeeds) {
    const std::vector<Declared> ntsc60 = declare_jittered(kMicrosecondsPer59_94Fps, 3600, seed);
    SYNC_REQUIRE(ntsc60.front().rate == kInitialFrameRate);
    SYNC_REQUIRE(changes(ntsc60) == 1);
    SYNC_REQUIRE(settled_at(ntsc60, {60000, 1001}) <= 4.0);

    // 60/1 first, then 30/1 once the cadence is clearly 30 fps, then 30000/1001.
    const std::vector<Declared> ntsc30 = declare_jittered(kMicrosecondsPer29_97Fps, 1800, seed);
    SYNC_REQUIRE(ntsc30.front().rate == kInitialFrameRate);
    SYNC_REQUIRE(changes(ntsc30) <= 2);
    for (const Declared& frame : ntsc30) {
      SYNC_REQUIRE((frame.rate == kInitialFrameRate || frame.rate == FrameRate{30, 1} ||
                    frame.rate == FrameRate{30000, 1001}));
    }
    SYNC_REQUIRE(settled_at(ntsc30, {30000, 1001}) <= 5.0);
  }
}

SYNC_TEST(ndi_frame_rate_tracker_follows_a_change_from_60_to_30_fps_within_a_second) {
  for (const std::uint64_t seed : kSeeds) {
    const std::vector<Declared> declared =
        declare_jittered(kMicrosecondsPer60Fps, 600, seed, 300, kMicrosecondsPer30Fps);
    const double change_seconds = 300.0 / 60.0;
    std::vector<Declared> after;
    for (const Declared& frame : declared) {
      if (frame.seconds < change_seconds) {
        SYNC_REQUIRE((frame.rate == FrameRate{60, 1}));
      } else {
        after.push_back(frame);
      }
    }
    SYNC_REQUIRE(settled_at(after, {30, 1}) - change_seconds <= 1.1);
  }
}
