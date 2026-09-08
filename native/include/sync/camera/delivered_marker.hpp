#pragma once

// Sustained-delivery instrumentation for the Sync camera plane.
//
// The protocol soak measures how fast the test publisher accepts frames, which
// on this host is bounded by its per-byte checksum (~27 fps at 1080p) and can
// never reach 60. Delivery is a different quantity: how many DISTINCT frames a
// real Media Foundation consumer reads back per second from the virtual
// camera. That is the number "sustained 1080p60" is actually about, and the
// only way to see the documented multi-hour delivery decline as a trace rather
// than an anecdote.
//
// This header is the part with no camera in it: a marker that rides in the
// frame pixels so a reader can tell frame N from frame N+1 (and a repeat of N
// from a genuinely new frame), and the accounting that turns a stream of
// decoded sequence numbers into per-second delivered-fps. Both are pure and
// unit-tested with no Media Foundation, no registration, and no hardware.
//
// The marker survives the real path. At 1080p source into the fixed 1080p
// canvas fit_camera_frame is identity (placement 0,0, no scale), so a pixel
// written here arrives at the same pixel. A consumer negotiates NV12 or RGB32;
// the marker uses only pure black and pure white cells, which are unambiguous
// as NV12 luma (~16 vs ~235, BT.709 studio range) and as an RGB32 blue channel
// (0 vs 255). Decode reads a luma-like sample per cell and thresholds, so it
// works from either format through one code path.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace noisefactor::sync::camera {

// --- marker layout ---------------------------------------------------------
//
// A row of solid square cells across the top-left. A fixed preamble locates
// the marker and rejects a frame that carries none; 32 data cells hold the
// sequence little-endian by bit; one parity cell catches a single flipped
// cell. Cells are large and centre-sampled so a stray boundary column cannot
// move a bit.
struct MarkerLayout {
  std::uint32_t cell = 40;         // pixels per cell edge
  std::uint32_t preamble = 4;      // sentinel cells: white,black,white,black
  std::uint32_t data_bits = 32;    // sequence width
  std::uint32_t parity = 1;
  [[nodiscard]] constexpr std::uint32_t cells() const noexcept {
    return preamble + data_bits + parity;
  }
  [[nodiscard]] constexpr std::uint32_t width_px() const noexcept { return cells() * cell; }
  [[nodiscard]] constexpr std::uint32_t height_px() const noexcept { return cell; }
};

inline constexpr MarkerLayout kMarker{};
inline constexpr std::uint8_t kBlack = 0;
inline constexpr std::uint8_t kWhite = 255;
inline constexpr std::uint8_t kLumaThreshold = 110;  // splits ~16 from ~235 with margin

// Preamble bit i (true = white). white,black,white,black.
[[nodiscard]] constexpr bool preamble_bit(std::uint32_t i) noexcept { return (i % 2) == 0; }

// Paints the marker for `seq` into a top-down BGRA canvas. The caller fills the
// rest of the canvas with whatever base pattern it likes; only the marker cells
// are touched. Bytes per pixel is 4 (B,G,R,A).
inline void encode_marker(std::span<std::byte> bgra, std::size_t stride,
                          std::uint32_t width, std::uint32_t height,
                          std::uint64_t seq, MarkerLayout layout = kMarker) noexcept {
  if (layout.width_px() > width || layout.height_px() > height) return;
  if (bgra.size() < stride * height) return;

  bool parity_bit = false;
  const auto paint = [&](std::uint32_t cell_index, bool white) noexcept {
    const std::uint8_t v = white ? kWhite : kBlack;
    const std::uint32_t x0 = cell_index * layout.cell;
    for (std::uint32_t y = 0; y < layout.cell; ++y) {
      std::byte* row = bgra.data() + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x0) * 4;
      for (std::uint32_t x = 0; x < layout.cell; ++x) {
        row[x * 4 + 0] = std::byte{v};
        row[x * 4 + 1] = std::byte{v};
        row[x * 4 + 2] = std::byte{v};
        row[x * 4 + 3] = std::byte{255};
      }
    }
  };

  std::uint32_t c = 0;
  for (std::uint32_t i = 0; i < layout.preamble; ++i) paint(c++, preamble_bit(i));
  for (std::uint32_t b = 0; b < layout.data_bits; ++b) {
    const bool white = ((seq >> b) & 1ULL) != 0;
    if (white) parity_bit = !parity_bit;
    paint(c++, white);
  }
  for (std::uint32_t i = 0; i < layout.parity; ++i) paint(c++, parity_bit);
}

// A sample of luma-like intensity at (x, y). NV12: the Y plane byte. RGB32:
// the blue channel byte. Either yields ~0 for a black cell and ~255 for white.
using LumaSampler = std::function<std::uint8_t(std::uint32_t x, std::uint32_t y)>;

// Reads the marker back. Returns the sequence, or nullopt when the preamble
// does not match (no marker, or not our frame) or parity fails. Samples the
// cell centre; a real reader passes a sampler over NV12 luma or RGB32 blue.
[[nodiscard]] inline std::optional<std::uint64_t>
decode_marker(const LumaSampler& sample, MarkerLayout layout = kMarker) noexcept {
  const std::uint32_t cy = layout.cell / 2;
  const auto white_at = [&](std::uint32_t cell_index) noexcept -> bool {
    const std::uint32_t cx = cell_index * layout.cell + layout.cell / 2;
    // Majority of three centre pixels: cheap defence against a boundary column.
    int white = 0;
    for (int dx = -1; dx <= 1; ++dx) {
      const std::uint32_t x = static_cast<std::uint32_t>(static_cast<int>(cx) + dx);
      if (sample(x, cy) >= kLumaThreshold) ++white;
    }
    return white >= 2;
  };

  std::uint32_t c = 0;
  for (std::uint32_t i = 0; i < layout.preamble; ++i) {
    if (white_at(c++) != preamble_bit(i)) return std::nullopt;
  }
  std::uint64_t seq = 0;
  bool parity_bit = false;
  for (std::uint32_t b = 0; b < layout.data_bits; ++b) {
    const bool white = white_at(c++);
    if (white) { seq |= (1ULL << b); parity_bit = !parity_bit; }
  }
  if (white_at(c++) != parity_bit) return std::nullopt;
  return seq;
}

// --- per-second delivery accounting ---------------------------------------
//
// Fed one observation per consumer read: the wall-clock millisecond and the
// decoded sequence, or nullopt when the read returned no sample. Distinguishes
// three things a naive fps count conflates:
//   new       the sequence advanced -> a genuinely fresh delivered frame
//   repeat    the same sequence again -> the camera re-served one frame
//             because no newer one had arrived (this is the decline's shape)
//   empty     the reader got no sample at all
// Out-of-order (sequence went backwards) is counted separately as an anomaly.
struct DeliverySecond {
  std::uint64_t second = 0;   // seconds since the run's first observation
  std::uint32_t newFrames = 0;
  std::uint32_t repeats = 0;
  std::uint32_t empties = 0;
  std::uint32_t outOfOrder = 0;
};

struct DeliverySummary {
  std::uint64_t seconds = 0;
  std::uint64_t totalNew = 0;
  std::uint64_t totalRepeats = 0;
  std::uint64_t totalEmpties = 0;
  std::uint64_t totalOutOfOrder = 0;
  double meanDeliveredFps = 0.0;   // totalNew / seconds
  std::uint32_t maxSecondFps = 0;
  std::uint32_t minSecondFps = 0;  // over fully-elapsed seconds only
  // True when every fully-elapsed second delivered at least `target` new
  // frames -- the literal reading of "sustained N fps".
  bool sustainedTarget = false;
  std::uint32_t target = 0;
  // Coarse decline signal: mean new-fps over the first third of seconds versus
  // the last third. A delivery decline shows here as last << first.
  double firstThirdFps = 0.0;
  double lastThirdFps = 0.0;
};

class DeliveryAccounting {
 public:
  // onSecond fires once per fully-elapsed second, in order, so a caller can
  // stream JSONL to disk without buffering the whole run.
  explicit DeliveryAccounting(std::uint32_t target,
                              std::function<void(const DeliverySecond&)> onSecond = {})
      : target_(target), onSecond_(std::move(onSecond)) {}

  void observe(std::uint64_t wallMs, std::optional<std::uint64_t> seq) {
    if (!started_) { started_ = true; baseMs_ = wallMs; }
    const std::uint64_t sec = (wallMs - baseMs_) / 1000;
    while (current_.second < sec) { flush(); current_ = DeliverySecond{current_.second + 1}; }

    if (!seq.has_value()) { ++current_.empties; return; }
    if (!haveSeq_) { haveSeq_ = true; lastSeq_ = *seq; ++current_.newFrames; return; }
    if (*seq == lastSeq_) { ++current_.repeats; return; }
    if (*seq < lastSeq_) { ++current_.outOfOrder; lastSeq_ = *seq; return; }
    ++current_.newFrames;
    lastSeq_ = *seq;
  }

  // Closes the final partial second WITHOUT counting it toward min/sustained
  // (a partial second is not a fair sample), then returns the summary.
  [[nodiscard]] DeliverySummary finish() {
    if (started_ && !finalFlushed_) { flush(); finalFlushed_ = true; }
    DeliverySummary s;
    s.target = target_;
    s.seconds = full_.size();
    for (const auto& d : full_) {
      s.totalNew += d.newFrames;
      s.totalRepeats += d.repeats;
      s.totalEmpties += d.empties;
      s.totalOutOfOrder += d.outOfOrder;
    }
    if (s.seconds > 0) {
      s.meanDeliveredFps = static_cast<double>(s.totalNew) / static_cast<double>(s.seconds);
      s.minSecondFps = full_.front().newFrames;
      s.sustainedTarget = true;
      for (const auto& d : full_) {
        s.maxSecondFps = d.newFrames > s.maxSecondFps ? d.newFrames : s.maxSecondFps;
        s.minSecondFps = d.newFrames < s.minSecondFps ? d.newFrames : s.minSecondFps;
        if (d.newFrames < target_) s.sustainedTarget = false;
      }
      const std::size_t third = full_.size() / 3;
      if (third > 0) {
        std::uint64_t f = 0, l = 0;
        for (std::size_t i = 0; i < third; ++i) f += full_[i].newFrames;
        for (std::size_t i = full_.size() - third; i < full_.size(); ++i) l += full_[i].newFrames;
        s.firstThirdFps = static_cast<double>(f) / static_cast<double>(third);
        s.lastThirdFps = static_cast<double>(l) / static_cast<double>(third);
      }
    }
    return s;
  }

  [[nodiscard]] const std::vector<DeliverySecond>& seconds() const noexcept { return full_; }

 private:
  void flush() {
    full_.push_back(current_);
    if (onSecond_) onSecond_(current_);
  }

  std::uint32_t target_;
  std::function<void(const DeliverySecond&)> onSecond_;
  std::vector<DeliverySecond> full_;
  DeliverySecond current_{};
  std::uint64_t baseMs_ = 0;
  std::uint64_t lastSeq_ = 0;
  bool started_ = false;
  bool haveSeq_ = false;
  bool finalFlushed_ = false;
};

}  // namespace noisefactor::sync::camera
