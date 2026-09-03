#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <sync/platform/camera_identity.hpp>

namespace noisefactor::sync::camera {

// Three slots: one being written, one being read, one spare. Matches the queue
// depth CmioCameraSink uses on macOS so both platforms drop frames at the same
// point under load. Three also gives a reader two whole frame periods to
// finish copying before the writer can come back around to that slot.
inline constexpr std::uint32_t kFrameRingSlots = 3;
inline constexpr std::uint32_t kFrameRingMagic = 0x53594E43;  // "SYNC"
inline constexpr std::uint32_t kFrameRingVersion = 1;

// One slot's payload: the full canvas as top-down BGRA.
inline constexpr std::size_t kFrameRingSlotBytes =
    static_cast<std::size_t>(kCanvas.width) * kCanvas.height * kBytesPerPixel;

// A seqlock per slot. The writer stores an odd sequence before touching the
// payload and the next even one after, so a reader that sees an odd sequence,
// or a different one before and after its copy, knows it read a torn frame.
//
// These atomics live in shared memory written by two processes. uint64 is
// always lock-free on x64, so this is the ordinary cross-process seqlock
// rather than std::atomic used outside its contract.
struct FrameRingSlot {
  std::atomic<std::uint64_t> sequence;
  std::uint64_t presentation_time_us;
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t row_stride;
  std::uint32_t reserved;
};

struct FrameRingHeader {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint32_t slots;
  std::uint32_t slot_bytes;
  std::atomic<std::uint64_t> newest;  // monotonic count of published frames
  // When the media source last asked for a frame, in the sender's own clock
  // units. A heartbeat rather than a consumer count: the count would leak if
  // the frame server were ever torn down between the increment and the
  // decrement, and a stale count is indistinguishable from a live consumer.
  // Staleness is self-correcting.
  std::atomic<std::uint64_t> last_demand_us;
  FrameRingSlot slot[kFrameRingSlots];
};

[[nodiscard]] constexpr auto frame_ring_bytes() noexcept -> std::size_t {
  return sizeof(FrameRingHeader) + kFrameRingSlotBytes * kFrameRingSlots;
}

// The shared section's name. It lives in the Global namespace because the
// media source runs in session 0 and syncd in the user's session. Deliberately
// not keyed by user: the media source cannot be told which account to pair
// with without an administrator-only API, so it grants INTERACTIVE on the
// section instead and whoever is logged in feeds the camera.
[[nodiscard]] auto section_name() -> std::wstring;

// Writes frames into a mapped ring. Does not own the mapping.
class FrameRingWriter {
 public:
  explicit FrameRingWriter(std::span<std::byte> mapping) noexcept;
  [[nodiscard]] auto valid() const noexcept -> bool;
  // True while a consumer has asked for a frame recently enough to still be
  // watching. False once the camera is open to nobody, which is what stops
  // the publisher fitting 1080p frames that nothing would read.
  [[nodiscard]] auto has_demand(std::uint64_t now_us) const noexcept -> bool;
  [[nodiscard]] auto has_capacity() const noexcept -> bool;
  // Copies bgra into the next slot and publishes it. False when the mapping is
  // invalid or the payload does not match the canvas.
  [[nodiscard]] auto write(std::span<const std::byte> bgra, std::size_t row_stride,
                           std::uint64_t presentation_time_us) noexcept -> bool;

 private:
  FrameRingHeader* header_ = nullptr;
  std::byte* payload_ = nullptr;
};

// How long after the media source's last request for a frame the sender keeps
// treating the camera as watched. Generous next to a 60 fps request cadence,
// so an ordinary hitch never reads as "nobody is looking", and short enough
// that a closed consumer stops the sender fitting frames within a second.
inline constexpr std::uint64_t kFrameRingDemandTimeoutUs = 1'000'000;

// The clock both halves stamp and compare demand with. It has to be the same
// domain in two processes, which steady_clock is: MSVC implements it on
// QueryPerformanceCounter, which is machine-wide. Defined here so neither
// side can pick a different one.
[[nodiscard]] inline auto camera_clock_us() noexcept -> std::uint64_t {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// Reads the newest complete frame. Does not own the mapping.
class FrameRingReader {
 public:
  explicit FrameRingReader(std::span<const std::byte> mapping) noexcept;
  [[nodiscard]] auto valid() const noexcept -> bool;
  // Called by the media source each time a consumer asks for a frame, so the
  // sender on the other side can tell whether anything is still watching.
  void record_demand(std::uint64_t now_us) const noexcept;
  // Monotonic publish count, so a caller can tell a new frame from a repeat.
  [[nodiscard]] auto newest_sequence() const noexcept -> std::uint64_t;
  // Copies the newest complete frame into out. False when nothing has been
  // published, when out is too small, or when the frame tore under a
  // concurrent write and did not settle within a bounded number of retries.
  [[nodiscard]] auto read(std::span<std::byte> out, std::size_t out_stride,
                          std::uint64_t& presentation_time_us) const noexcept -> bool;

 private:
  const FrameRingHeader* header_ = nullptr;
  const std::byte* payload_ = nullptr;
};

}  // namespace noisefactor::sync::camera
