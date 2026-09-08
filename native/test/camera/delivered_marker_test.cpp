#include "test_harness.hpp"

#include <sync/camera/delivered_marker.hpp>
#include <sync/camera/nv12.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace {

using namespace noisefactor::sync::camera;

constexpr std::uint32_t kW = 1920;
constexpr std::uint32_t kH = 1080;
constexpr std::size_t kStride = static_cast<std::size_t>(kW) * 4;

// A base BGRA canvas of mid-grey, then the marker painted over it. Mid-grey so
// a decoder that ignored the marker and read the base would land near the
// threshold and could not accidentally pass.
[[nodiscard]] std::vector<std::byte> marked_canvas(std::uint64_t seq) {
  std::vector<std::byte> bgra(kStride * kH, std::byte{128});
  for (std::size_t i = 3; i < bgra.size(); i += 4) bgra[i] = std::byte{255};  // opaque
  encode_marker(bgra, kStride, kW, kH, seq);
  return bgra;
}

// Decode by sampling the RGB32 blue channel directly, the way a consumer that
// negotiated RGB32 would.
[[nodiscard]] std::optional<std::uint64_t> decode_rgb32(const std::vector<std::byte>& bgra) {
  return decode_marker([&](std::uint32_t x, std::uint32_t y) -> std::uint8_t {
    return static_cast<std::uint8_t>(bgra[static_cast<std::size_t>(y) * kStride + static_cast<std::size_t>(x) * 4 + 0]);
  });
}

// Decode after a real BGRA -> NV12 conversion, the way a consumer that
// negotiated NV12 would: the marker must survive BT.709 studio-range luma.
[[nodiscard]] std::optional<std::uint64_t> decode_nv12(const std::vector<std::byte>& bgra) {
  const std::size_t ystride = kW;
  std::vector<std::byte> nv12(nv12_size_bytes(kW, kH, ystride));
  SYNC_REQUIRE(bgra_to_nv12(bgra, kStride, kW, kH, nv12, ystride));
  return decode_marker([&](std::uint32_t x, std::uint32_t y) -> std::uint8_t {
    return static_cast<std::uint8_t>(nv12[static_cast<std::size_t>(y) * ystride + x]);
  });
}

}  // namespace

SYNC_TEST(marker_round_trips_through_rgb32_blue_channel) {
  for (std::uint64_t seq : {0ULL, 1ULL, 2ULL, 255ULL, 256ULL, 65535ULL, 65536ULL,
                            1234567ULL, (1ULL << 31), 0xFFFFFFFFULL}) {
    const auto canvas = marked_canvas(seq);
    const auto got = decode_rgb32(canvas);
    SYNC_REQUIRE(got.has_value());
    SYNC_REQUIRE(*got == seq);
  }
}

SYNC_TEST(marker_round_trips_through_nv12_luma) {
  // The real path when a consumer negotiates NV12. Studio-range luma compresses
  // black to ~16 and white to ~235; the threshold must still split them.
  for (std::uint64_t seq : {0ULL, 1ULL, 42ULL, 65536ULL, 0x7FFFFFFFULL, 0xFFFFFFFFULL}) {
    const auto canvas = marked_canvas(seq);
    const auto got = decode_nv12(canvas);
    SYNC_REQUIRE(got.has_value());
    SYNC_REQUIRE(*got == seq);
  }
}

SYNC_TEST(consecutive_sequences_decode_distinctly) {
  // The whole point: frame N and frame N+1 must never read as the same value,
  // or new frames would be counted as repeats.
  std::optional<std::uint64_t> prev;
  for (std::uint64_t seq = 1000; seq < 1064; ++seq) {
    const auto got = decode_nv12(marked_canvas(seq));
    SYNC_REQUIRE(got.has_value());
    SYNC_REQUIRE(*got == seq);
    SYNC_REQUIRE(!prev.has_value() || *got != *prev);
    prev = got;
  }
}

SYNC_TEST(a_frame_with_no_marker_is_rejected) {
  // A solid frame and a mid-grey frame both carry no preamble: decode must
  // report nothing rather than invent a sequence.
  std::vector<std::byte> grey(kStride * kH, std::byte{128});
  SYNC_REQUIRE(!decode_rgb32(grey).has_value());
  std::vector<std::byte> white(kStride * kH, std::byte{255});
  SYNC_REQUIRE(!decode_rgb32(white).has_value());
}

SYNC_TEST(a_single_flipped_cell_fails_parity) {
  auto canvas = marked_canvas(0xABCDEF01ULL);
  // Flip one data cell (cell index preamble+5) from whatever it is to the
  // opposite: its whole block, so centre sampling sees the flip.
  const std::uint32_t idx = kMarker.preamble + 5;
  const std::uint32_t x0 = idx * kMarker.cell;
  const bool was_white = static_cast<std::uint8_t>(canvas[(kMarker.cell / 2) * kStride + static_cast<std::size_t>(x0 + kMarker.cell / 2) * 4]) >= kLumaThreshold;
  const std::uint8_t v = was_white ? kBlack : kWhite;
  for (std::uint32_t y = 0; y < kMarker.cell; ++y) {
    for (std::uint32_t x = 0; x < kMarker.cell; ++x) {
      const std::size_t p = static_cast<std::size_t>(y) * kStride + static_cast<std::size_t>(x0 + x) * 4;
      canvas[p + 0] = std::byte{v};
      canvas[p + 1] = std::byte{v};
      canvas[p + 2] = std::byte{v};
    }
  }
  SYNC_REQUIRE(!decode_rgb32(canvas).has_value());
}

// --- accounting ------------------------------------------------------------

SYNC_TEST(a_clean_60fps_stream_reads_as_sustained_60) {
  std::vector<DeliverySecond> emitted;
  DeliveryAccounting acc(60, [&](const DeliverySecond& d) { emitted.push_back(d); });
  // Ten seconds, 60 distinct frames each, evenly spaced.
  std::uint64_t seq = 0;
  for (std::uint64_t s = 0; s < 10; ++s) {
    for (int i = 0; i < 60; ++i) acc.observe(s * 1000 + static_cast<std::uint64_t>(i) * 1000 / 60, seq++);
  }
  const auto sum = acc.finish();
  SYNC_REQUIRE(sum.seconds == 10);
  SYNC_REQUIRE(sum.totalNew == 600);
  SYNC_REQUIRE(sum.sustainedTarget);
  SYNC_REQUIRE(sum.minSecondFps == 60);
  SYNC_REQUIRE(sum.maxSecondFps == 60);
  SYNC_REQUIRE(emitted.size() >= 9);  // every fully-elapsed second emits once
}

SYNC_TEST(a_declining_stream_is_not_sustained_and_shows_the_decline) {
  // The documented failure: starts near 60, sags toward 20. This is exactly
  // what "sustained 1080p60" must beat, so the accounting must flag it.
  DeliveryAccounting acc(60);
  std::uint64_t seq = 0;
  for (std::uint64_t s = 0; s < 30; ++s) {
    const int fps = 60 - static_cast<int>(s);  // 60 down to 31
    for (int i = 0; i < fps; ++i) acc.observe(s * 1000 + static_cast<std::uint64_t>(i) * 1000 / static_cast<std::uint64_t>(fps), seq++);
  }
  const auto sum = acc.finish();
  SYNC_REQUIRE(sum.seconds == 30);
  SYNC_REQUIRE(!sum.sustainedTarget);
  SYNC_REQUIRE(sum.firstThirdFps > sum.lastThirdFps + 10.0);  // clear decline
  SYNC_REQUIRE(sum.maxSecondFps == 60);
}

SYNC_TEST(a_stalled_stream_counts_repeats_not_new_frames) {
  // The camera keeps serving one frame because no new one arrives: repeats
  // climb, new frames do not. A naive fps counter would call this "delivering
  // frames"; delivered-fps correctly reads it as stalled.
  DeliveryAccounting acc(60);
  // Second 0: 60 fresh. Seconds 1-4: same last frame re-read 60x each.
  std::uint64_t seq = 0;
  for (int i = 0; i < 60; ++i) acc.observe(static_cast<std::uint64_t>(i) * 1000 / 60, seq++);
  const std::uint64_t stuck = seq - 1;
  for (std::uint64_t s = 1; s < 5; ++s) {
    for (int i = 0; i < 60; ++i) acc.observe(s * 1000 + static_cast<std::uint64_t>(i) * 1000 / 60, stuck);
  }
  const auto sum = acc.finish();
  SYNC_REQUIRE(sum.seconds == 5);
  SYNC_REQUIRE(sum.totalNew == 60);
  SYNC_REQUIRE(sum.totalRepeats == 240);
  SYNC_REQUIRE(!sum.sustainedTarget);
  SYNC_REQUIRE(sum.minSecondFps == 0);  // seconds 1-4 delivered nothing new
}

SYNC_TEST(empty_reads_and_out_of_order_are_counted_separately) {
  DeliveryAccounting acc(60);
  acc.observe(0, 5);
  acc.observe(100, std::nullopt);   // reader got no sample
  acc.observe(200, std::nullopt);
  acc.observe(300, 6);              // new
  acc.observe(400, 4);              // backwards -> anomaly, not a new frame
  acc.observe(1500, 7);             // forces second 0 to flush
  const auto sum = acc.finish();
  SYNC_REQUIRE(sum.totalEmpties == 2);
  SYNC_REQUIRE(sum.totalOutOfOrder == 1);
  // second 0 delivered seq 5 and 6 as new; the backwards 4 did not add a new.
  SYNC_REQUIRE(sum.seconds == 2);
  SYNC_REQUIRE(sum.totalNew == 3);  // 5, 6, and 7
}
