#include <sync/camera/frame_ring.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <cstring>

namespace noisefactor::sync::camera {

namespace {

constexpr int kTornReadRetries = 4;

[[nodiscard]] auto mapping_is_ring(const void* data, std::size_t bytes) noexcept -> bool {
  if (data == nullptr || bytes < frame_ring_bytes()) return false;
  const auto* header = static_cast<const FrameRingHeader*>(data);
  return header->magic == kFrameRingMagic && header->version == kFrameRingVersion &&
         header->slots == kFrameRingSlots && header->slot_bytes == kFrameRingSlotBytes;
}

}  // namespace

auto section_name() -> std::wstring { return L"Global\\SyncCamera.frames"; }

#if defined(_WIN32)
auto windows_shm_default_path() -> std::wstring {
  wchar_t temp[MAX_PATH];
  const DWORD len = ::GetTempPathW(MAX_PATH, temp);
  if (len > 0 && len < MAX_PATH) {
    return std::wstring(temp, len) + windows_shm_filename();
  }
  return windows_shm_filename();
}
#endif

FrameRingWriter::FrameRingWriter(std::span<std::byte> mapping) noexcept {
  if (mapping.data() == nullptr || mapping.size() < frame_ring_bytes()) return;
  auto* header = reinterpret_cast<FrameRingHeader*>(mapping.data());
  // A fresh section is zeroed, so the first writer stamps it. An already
  // stamped ring is adopted as it is; the stamp is what makes a reader trust
  // the mapping at all.
  if (header->magic != kFrameRingMagic) {
    header->version = kFrameRingVersion;
    header->slots = kFrameRingSlots;
    header->slot_bytes = static_cast<std::uint32_t>(kFrameRingSlotBytes);
    header->newest.store(0, std::memory_order_relaxed);
    header->last_demand_us.store(0, std::memory_order_relaxed);
    for (auto& slot : header->slot) {
      slot.sequence.store(0, std::memory_order_relaxed);
    }
    // Magic last, behind a release fence: a reader that sees the magic must
    // also see the rest of the header it just validated.
    std::atomic_thread_fence(std::memory_order_release);
    header->magic = kFrameRingMagic;
  }
  if (!mapping_is_ring(mapping.data(), mapping.size())) return;
  header_ = header;
  payload_ = mapping.data() + sizeof(FrameRingHeader);
}

auto FrameRingWriter::valid() const noexcept -> bool { return header_ != nullptr; }

auto FrameRingWriter::has_capacity() const noexcept -> bool { return header_ != nullptr; }

auto FrameRingWriter::has_demand(std::uint64_t now_us) const noexcept -> bool {
  if (header_ == nullptr) return false;
  const std::uint64_t last = header_->last_demand_us.load(std::memory_order_acquire);
  if (last == 0) return false;
  // Unsigned, so a demand stamped ahead of this clock would otherwise wrap to
  // an enormous age and read as stale forever. Tolerate a stamp from the
  // future by the same margin, but only that much: an unbounded allowance
  // would let one corrupt value latch demand on permanently, which is the
  // failure the heartbeat exists to remove.
  if (now_us < last) return (last - now_us) <= kFrameRingDemandTimeoutUs;
  return (now_us - last) <= kFrameRingDemandTimeoutUs;
}

auto FrameRingWriter::write_with(DirectWriter writer, void* context,
                                 std::uint64_t presentation_time_us) noexcept -> bool {
  if (header_ == nullptr || writer == nullptr) return false;
  const std::size_t stride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;
  const std::uint64_t next = header_->newest.load(std::memory_order_acquire) + 1;
  const auto index = static_cast<std::uint32_t>(next % kFrameRingSlots);
  FrameRingSlot& slot = header_->slot[index];

  slot.sequence.store(next * 2 - 1, std::memory_order_release);
  std::span<std::byte> destination(payload_ + static_cast<std::size_t>(index) * kFrameRingSlotBytes,
                                   kFrameRingSlotBytes);
  if (!writer(context, destination, stride)) {
    slot.sequence.store((next - 1) * 2, std::memory_order_release);
    return false;
  }
  slot.presentation_time_us = presentation_time_us;
  slot.width = kCanvas.width;
  slot.height = kCanvas.height;
  slot.row_stride = static_cast<std::uint32_t>(stride);
  slot.sequence.store(next * 2, std::memory_order_release);
  header_->newest.store(next, std::memory_order_release);
  return true;
}

auto FrameRingWriter::write(std::span<const std::byte> bgra, std::size_t row_stride,
                            std::uint64_t presentation_time_us) noexcept -> bool {
  if (header_ == nullptr) return false;
  if (row_stride != static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel) return false;
  if (bgra.size() < kFrameRingSlotBytes) return false;

  struct CopyContext {
    std::span<const std::byte> source;
  } context{bgra};

  const auto copy = [](void* ctx, std::span<std::byte> dest, std::size_t /*stride*/) noexcept -> bool {
    const auto& c = *static_cast<const CopyContext*>(ctx);
    std::memcpy(dest.data(), c.source.data(), kFrameRingSlotBytes);
    return true;
  };

  return write_with(copy, &context, presentation_time_us);
}

void FrameRingWriter::scrub() noexcept {
  if (header_ == nullptr) return;
  // Retire the publication count first, behind a release fence: every read
  // started after this point, and every retry of a reader that just
  // discarded a torn slot, sees "nothing published" and fails instead of
  // wandering over the scrubbed slots.
  header_->newest.store(0, std::memory_order_release);
  // Mark every slot as a write in progress before touching any payload,
  // exactly as write_with() does: a reader that already loaded a slot's
  // even sequence sees the changed odd one on its confirming load and
  // discards the copy, and one that arrives afterwards never treats a
  // scrubbed slot as complete. The mark is never closed -- the writer is
  // going away, so the slot must not look publishable again.
  for (auto& slot : header_->slot) {
    slot.sequence.store(1, std::memory_order_release);
    slot.presentation_time_us = 0;
    slot.width = 0;
    slot.height = 0;
    slot.row_stride = 0;
    slot.reserved = 0;
  }
  // Only now may the retained pixels clear: every sequence already stands
  // odd, so no reader can validate a copy that straddles this zeroing.
  for (std::uint32_t index = 0; index < kFrameRingSlots; ++index) {
    std::memset(payload_ + static_cast<std::size_t>(index) * kFrameRingSlotBytes,
                0, kFrameRingSlotBytes);
  }
  header_->last_demand_us.store(0, std::memory_order_relaxed);
  header_->slot_bytes = 0;
  header_->slots = 0;
  header_->version = 0;
  // Magic last: while it stands, a reader constructed on the still-mapped
  // section holds a consistent, empty view; once it clears, new readers
  // reject the mapping outright.
  header_->magic = 0;
  std::atomic_thread_fence(std::memory_order_release);
  // The writer is spent: nothing it can still be asked to write may land
  // in a mapping that no longer describes a ring.
  header_ = nullptr;
  payload_ = nullptr;
}

FrameRingReader::FrameRingReader(std::span<const std::byte> mapping) noexcept {
  if (!mapping_is_ring(mapping.data(), mapping.size())) return;
  header_ = reinterpret_cast<const FrameRingHeader*>(mapping.data());
  payload_ = mapping.data() + sizeof(FrameRingHeader);
}

auto FrameRingReader::valid() const noexcept -> bool { return header_ != nullptr; }

void FrameRingReader::record_demand(std::uint64_t now_us) noexcept {
  if (header_ == nullptr) return;
  // Non-const on purpose: this is the one field the reading half writes, and
  // a const method that mutated through a cast hid that from every caller.
  // The cast itself remains because the span is const -- the mapping behind
  // it must be writable, which SectionOwner guarantees by mapping
  // FILE_MAP_WRITE.
  // Use a monotonic CAS loop so concurrent readers advancing at slightly
  // different wall times never regress the newest demand heartbeat.
  auto& demand = const_cast<FrameRingHeader*>(header_)->last_demand_us;
  std::uint64_t current = demand.load(std::memory_order_relaxed);
  while (current < now_us && !demand.compare_exchange_weak(
             current, now_us, std::memory_order_release, std::memory_order_relaxed)) {
  }
  // A stamp dated ahead of this caller's clock by more than the tolerated
  // skew cannot come from a concurrent same-epoch reader -- every reader
  // samples the same machine-wide steady_clock/QPC domain just before it
  // arrives here (media_source.cpp samples camera_clock_us() before the
  // call), so two live samples never differ by a whole timeout. It is
  // either a previous epoch's heartbeat (the mapping outlived a reboot
  // while the clock restarted near zero) or a caller that was descheduled
  // for more than the timeout between sampling the clock and arriving.
  // Only the first may repair the stamp: re-sample the clock and require
  // the caller's own sample to still be fresh. A stale caller leaves the
  // stamp untouched -- its demand is genuinely old, and the next fresh
  // heartbeat, one frame period at 60 fps, is the bounded recovery.
  if (current > now_us && (current - now_us) > kFrameRingDemandTimeoutUs) {
    const std::uint64_t fresh_us = camera_clock_us();
    const bool caller_is_stale =
        fresh_us > now_us && (fresh_us - now_us) > kFrameRingDemandTimeoutUs;
    if (!caller_is_stale) {
      while (current > now_us && (current - now_us) > kFrameRingDemandTimeoutUs) {
        // Abandon the repair if the caller's sample goes stale while waiting
        // on the CAS: writing it then would regress whatever newer stamp a
        // live reader published in the meantime.
        const std::uint64_t check_us = camera_clock_us();
        if (check_us > now_us && (check_us - now_us) > kFrameRingDemandTimeoutUs) {
          break;
        }
        if (demand.compare_exchange_weak(current, now_us, std::memory_order_release,
                                         std::memory_order_relaxed)) {
          break;
        }
      }
    }
  }
}

auto FrameRingReader::newest_sequence() const noexcept -> std::uint64_t {
  return header_ == nullptr ? 0 : header_->newest.load(std::memory_order_acquire);
}

auto FrameRingReader::read(std::span<std::byte> out, std::size_t out_stride,
                           std::uint64_t& presentation_time_us) const noexcept -> bool {
  if (header_ == nullptr) return false;
  if (out_stride != static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel) return false;
  if (out.size() < kFrameRingSlotBytes) return false;

  for (int attempt = 0; attempt < kTornReadRetries; ++attempt) {
    const std::uint64_t newest = header_->newest.load(std::memory_order_acquire);
    if (newest == 0) return false;
    const auto index = static_cast<std::uint32_t>(newest % kFrameRingSlots);
    const FrameRingSlot& slot = header_->slot[index];
    const std::uint64_t before = slot.sequence.load(std::memory_order_acquire);
    if ((before & 1U) != 0) continue;  // a write is in progress
    std::memcpy(out.data(), payload_ + static_cast<std::size_t>(index) * kFrameRingSlotBytes,
                kFrameRingSlotBytes);
    const std::uint64_t presentation = slot.presentation_time_us;
    if (slot.sequence.load(std::memory_order_acquire) != before) continue;  // torn
    presentation_time_us = presentation;
    return true;
  }
  return false;
}

}  // namespace noisefactor::sync::camera
