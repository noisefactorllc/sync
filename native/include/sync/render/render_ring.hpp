#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace noisefactor::sync::render {

// The shared-memory ring between the render helper (sync-render), which
// compiles and renders a program on the GPU, and syncd, which publishes what it
// renders to the providers. This is the render path's replacement for the
// browser sender: the frames never cross a socket and are never encoded,
// because both halves run on the same machine as the same user.
//
// One writer, one reader. The writer is the helper; it creates the section,
// stamps the geometry, and owns it for its lifetime. The reader is syncd. A
// helper restart creates a fresh section, so the reader re-attaches rather than
// trusting an old mapping across a writer it has never seen.
//
// Unlike the camera ring (sync/camera/frame_ring.hpp), whose canvas is fixed
// at 1080p because the camera's format is negotiated before any sender
// exists, the render ring carries the program's own geometry: the helper
// renders at the size it was asked for and the ring is sized to match.

inline constexpr std::uint32_t kRenderRingMagic = 0x524E5953;  // "SYNR"
inline constexpr std::uint32_t kRenderRingVersion = 1;
inline constexpr std::uint32_t kRenderRingSlots = 3;
// The same ceilings the wire protocol enforces on a browser sender, so a frame
// from either source is one every provider already accepts.
inline constexpr std::uint32_t kRenderRingMaxDimension = 4096;
inline constexpr std::size_t kRenderRingPayloadAlignment = 64;

// The frame description the writer stamps once. The numbering is the wire
// protocol's (protocol-v1.md), so the reader hands the values to FrameView
// unchanged.
inline constexpr std::uint16_t kRenderPixelFormatRgba8 = 1;
inline constexpr std::uint16_t kRenderColorSpaceSrgb = 1;
inline constexpr std::uint16_t kRenderColorSpaceDisplayP3 = 2;
inline constexpr std::uint16_t kRenderAlphaOpaque = 1;
inline constexpr std::uint16_t kRenderAlphaStraight = 2;
inline constexpr std::uint16_t kRenderAlphaPremultiplied = 3;

struct RenderRingGeometry {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint16_t color_space = kRenderColorSpaceSrgb;
  std::uint16_t alpha_mode = kRenderAlphaOpaque;
};

// A seqlock per slot. The writer stores an odd value before it touches the
// payload and 2 * frame after, so a slot's sequence names the frame it holds
// and an odd value means "being written".
struct RenderRingSlot {
  std::atomic<std::uint64_t> sequence;
  std::uint64_t presentation_time_us;
  std::uint64_t reserved[2];
};

struct RenderRingHeader {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint32_t header_bytes;
  std::uint32_t slot_count;
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t row_stride;
  std::uint32_t slot_bytes;
  std::uint16_t pixel_format;
  std::uint16_t color_space;
  std::uint16_t alpha_mode;
  std::uint16_t reserved0;
  std::uint32_t writer_pid;
  // The newest complete frame as (frame << 2) | slot, 0 until the first
  // publish. One word rather than two so the reader can never pair a frame
  // number with the wrong slot.
  std::atomic<std::uint64_t> newest;
  // The slot the reader is reading, plus one; 0 when it holds none. The writer
  // never starts a frame in a leased slot, which is what lets syncd publish
  // straight out of the mapping instead of copying 8 MB per 1080p frame first.
  std::atomic<std::uint32_t> reader_lease;
  // 1 while the writer runs, 2 once it has closed the ring deliberately. A
  // crash leaves 1 and a stale heartbeat, which the reader treats the same way.
  std::atomic<std::uint32_t> writer_state;
  std::atomic<std::uint64_t> writer_heartbeat_us;
  std::atomic<std::uint64_t> reader_heartbeat_us;
  std::uint64_t reserved1[4];
  RenderRingSlot slot[kRenderRingSlots];
};

// Both processes read and write these atomics through a shared mapping, which
// is only sound when they are lock-free (no hidden per-process lock).
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

inline constexpr std::uint32_t kRenderWriterRunning = 1;
inline constexpr std::uint32_t kRenderWriterClosed = 2;

[[nodiscard]] constexpr auto render_ring_payload_offset() noexcept -> std::size_t {
  return (sizeof(RenderRingHeader) + kRenderRingPayloadAlignment - 1) /
         kRenderRingPayloadAlignment * kRenderRingPayloadAlignment;
}

// Total section size for a geometry, or nullopt when the geometry is outside
// the ring's limits.
[[nodiscard]] auto render_ring_bytes(const RenderRingGeometry& geometry) noexcept
    -> std::optional<std::size_t>;

// The clock both halves stamp heartbeats and presentation times with.
// steady_clock is machine-wide on every platform Sync supports, so the two
// processes agree on it.
[[nodiscard]] auto render_clock_us() noexcept -> std::uint64_t;

struct RenderFrameInfo {
  std::uint64_t frame = 0;
  std::uint64_t presentation_time_us = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_stride = 0;
  std::uint16_t color_space = 0;
  std::uint16_t alpha_mode = 0;
};

class RenderRingWriter {
 public:
  // Stamps a zeroed mapping with the geometry and adopts it. The mapping must
  // be at least render_ring_bytes(geometry) long. Invalid on any mismatch.
  RenderRingWriter(std::span<std::byte> mapping, const RenderRingGeometry& geometry,
                   std::uint32_t writer_pid) noexcept;

  [[nodiscard]] auto valid() const noexcept -> bool;
  [[nodiscard]] auto row_stride() const noexcept -> std::size_t;

  // Hands the destination slot to fill (top-down RGBA8 at row_stride()) and
  // publishes it when fill returns true. Returns false, publishing nothing,
  // when the ring is invalid or fill fails.
  using Fill = bool (*)(void* context, std::span<std::byte> destination,
                        std::size_t row_stride) noexcept;
  [[nodiscard]] auto write_with(Fill fill, void* context,
                                std::uint64_t presentation_time_us) noexcept -> bool;

  // Copies a whole top-down frame. source_stride may exceed row_stride();
  // rows are copied individually then.
  [[nodiscard]] auto write(std::span<const std::byte> source, std::size_t source_stride,
                           std::uint64_t presentation_time_us) noexcept -> bool;

  // Marks the writer alive without publishing, for a program that renders
  // nothing new (an error, or a paused clock).
  void heartbeat(std::uint64_t now_us) noexcept;
  // A deliberate shutdown, so the reader closes the sender immediately
  // instead of waiting for the heartbeat to go stale.
  void close() noexcept;

  // True while the reader has stamped its heartbeat recently. Not required for
  // writing; it exists for diagnostics.
  [[nodiscard]] auto reader_alive(std::uint64_t now_us,
                                  std::uint64_t timeout_us) const noexcept -> bool;

 private:
  RenderRingHeader* header_ = nullptr;
  std::byte* payload_ = nullptr;
};

// A frame the reader holds. The payload stays valid and unchanged until
// release(), because the writer will not start a frame in a leased slot.
class RenderFrameLease {
 public:
  RenderFrameLease() noexcept = default;
  RenderFrameLease(const RenderFrameLease&) = delete;
  auto operator=(const RenderFrameLease&) -> RenderFrameLease& = delete;
  RenderFrameLease(RenderFrameLease&& other) noexcept;
  auto operator=(RenderFrameLease&& other) noexcept -> RenderFrameLease&;
  ~RenderFrameLease();

  [[nodiscard]] auto held() const noexcept -> bool { return header_ != nullptr; }
  [[nodiscard]] auto info() const noexcept -> const RenderFrameInfo& { return info_; }
  [[nodiscard]] auto payload() const noexcept -> std::span<const std::byte> { return payload_; }
  void release() noexcept;

 private:
  friend class RenderRingReader;
  RenderRingHeader* header_ = nullptr;
  RenderFrameInfo info_{};
  std::span<const std::byte> payload_{};
};

class RenderRingReader {
 public:
  // Adopts a mapping the writer has stamped. Invalid unless the stamp, the
  // version and the geometry all check out against the mapping's length.
  explicit RenderRingReader(std::span<std::byte> mapping) noexcept;

  [[nodiscard]] auto valid() const noexcept -> bool;
  [[nodiscard]] auto newest_frame() const noexcept -> std::uint64_t;
  [[nodiscard]] auto writer_closed() const noexcept -> bool;
  // True while the writer has published or heartbeated within timeout_us.
  [[nodiscard]] auto writer_alive(std::uint64_t now_us,
                                  std::uint64_t timeout_us) const noexcept -> bool;
  void heartbeat(std::uint64_t now_us) noexcept;

  // Leases the newest complete frame. Nothing is returned when no frame has
  // been published or the newest slot kept changing under the writer for a
  // bounded number of attempts. Only one lease may be held at a time.
  [[nodiscard]] auto acquire() noexcept -> RenderFrameLease;

  [[nodiscard]] auto geometry() const noexcept -> RenderRingGeometry;

 private:
  RenderRingHeader* header_ = nullptr;
  std::byte* payload_ = nullptr;
};

// An owner-only shared section backing a ring. The writer creates it, the
// reader opens it by name; the name is a filesystem path on POSIX and a
// section name in the session's Local namespace on Windows.
class RenderRingSection {
 public:
  RenderRingSection() noexcept = default;
  RenderRingSection(const RenderRingSection&) = delete;
  auto operator=(const RenderRingSection&) -> RenderRingSection& = delete;
  RenderRingSection(RenderRingSection&& other) noexcept;
  auto operator=(RenderRingSection&& other) noexcept -> RenderRingSection&;
  ~RenderRingSection();

  // Replaces any existing section at name with a fresh zeroed one of bytes.
  [[nodiscard]] static auto create(const std::string& name, std::size_t bytes,
                                   std::string& error) -> std::optional<RenderRingSection>;
  // Maps an existing section. On POSIX the file must be a regular file owned
  // by this user and closed to everyone else.
  [[nodiscard]] static auto open(const std::string& name, std::string& error)
      -> std::optional<RenderRingSection>;

  [[nodiscard]] auto bytes() noexcept -> std::span<std::byte>;
  // Removes the name (POSIX) so a new reader cannot attach to a ring whose
  // writer is gone. Existing mappings stay valid.
  void unlink() noexcept;

 private:
  void reset() noexcept;
  void* view_ = nullptr;
  std::size_t size_ = 0;
  std::string name_;
  bool owner_ = false;
#if defined(_WIN32)
  void* handle_ = nullptr;
#else
  // The inode this owner created, so unlink() never removes a newer writer's
  // section that has since taken the same name.
  std::uint64_t device_ = 0;
  std::uint64_t inode_ = 0;
#endif
};

// The default section name for a helper instance.
[[nodiscard]] auto default_render_ring_name() -> std::string;

}  // namespace noisefactor::sync::render
