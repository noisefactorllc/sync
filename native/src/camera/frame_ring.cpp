#include <sync/camera/frame_ring.hpp>

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

auto FrameRingWriter::write(std::span<const std::byte> bgra, std::size_t row_stride,
                            std::uint64_t presentation_time_us) noexcept -> bool {
  if (header_ == nullptr) return false;
  if (row_stride != static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel) return false;
  if (bgra.size() < kFrameRingSlotBytes) return false;

  const std::uint64_t next = header_->newest.load(std::memory_order_acquire) + 1;
  const auto index = static_cast<std::uint32_t>(next % kFrameRingSlots);
  FrameRingSlot& slot = header_->slot[index];

  slot.sequence.store(next * 2 - 1, std::memory_order_release);
  std::memcpy(payload_ + static_cast<std::size_t>(index) * kFrameRingSlotBytes, bgra.data(),
              kFrameRingSlotBytes);
  slot.presentation_time_us = presentation_time_us;
  slot.width = kCanvas.width;
  slot.height = kCanvas.height;
  slot.row_stride = static_cast<std::uint32_t>(row_stride);
  slot.sequence.store(next * 2, std::memory_order_release);
  header_->newest.store(next, std::memory_order_release);
  return true;
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
  const_cast<FrameRingHeader*>(header_)->last_demand_us.store(now_us, std::memory_order_release);
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
