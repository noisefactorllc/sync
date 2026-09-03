#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <sync/camera/frame_ring.hpp>

namespace {

using noisefactor::sync::camera::FrameRingHeader;
using noisefactor::sync::camera::FrameRingReader;
using noisefactor::sync::camera::FrameRingWriter;
using noisefactor::sync::camera::frame_event_name;
using noisefactor::sync::camera::frame_ring_bytes;
using noisefactor::sync::camera::kBytesPerPixel;
using noisefactor::sync::camera::kCanvas;
using noisefactor::sync::camera::kFrameRingSlotBytes;
using noisefactor::sync::camera::kFrameRingSlots;
using noisefactor::sync::camera::section_name;

constexpr std::size_t kStride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;

[[nodiscard]] auto canvas_filled(std::uint8_t value) -> std::vector<std::byte> {
  return std::vector<std::byte>(kFrameRingSlotBytes, static_cast<std::byte>(value));
}

SYNC_TEST(names_are_global_and_distinct) {
  const std::wstring section = section_name();
  SYNC_REQUIRE(section.rfind(L"Global\\", 0) == 0);
  const std::wstring event = frame_event_name();
  SYNC_REQUIRE(event.rfind(L"Global\\", 0) == 0);
  // Separate kernel objects: sharing a name would make CreateEventW fail
  // against the existing section.
  SYNC_REQUIRE(event != section);
}

SYNC_TEST(a_fresh_ring_has_nothing_to_read) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  SYNC_REQUIRE(writer.valid());
  const FrameRingReader reader(mapping);
  SYNC_REQUIRE(reader.valid());
  SYNC_REQUIRE(reader.newest_sequence() == 0);
  std::vector<std::byte> out(kFrameRingSlotBytes);
  std::uint64_t presentation = 0;
  SYNC_REQUIRE(!reader.read(out, kStride, presentation));
}

SYNC_TEST(a_written_frame_reads_back_intact) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  const auto frame = canvas_filled(0x5A);
  SYNC_REQUIRE(writer.write(frame, kStride, 1234));
  const FrameRingReader reader(mapping);
  SYNC_REQUIRE(reader.newest_sequence() == 1);
  std::vector<std::byte> out(kFrameRingSlotBytes);
  std::uint64_t presentation = 0;
  SYNC_REQUIRE(reader.read(out, kStride, presentation));
  SYNC_REQUIRE(presentation == 1234);
  SYNC_REQUIRE(std::memcmp(out.data(), frame.data(), frame.size()) == 0);
}

SYNC_TEST(the_reader_always_sees_the_newest_frame) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  for (std::uint8_t value = 1; value <= 5; ++value) {
    SYNC_REQUIRE(writer.write(canvas_filled(value), kStride, value));
  }
  const FrameRingReader reader(mapping);
  SYNC_REQUIRE(reader.newest_sequence() == 5);
  std::vector<std::byte> out(kFrameRingSlotBytes);
  std::uint64_t presentation = 0;
  SYNC_REQUIRE(reader.read(out, kStride, presentation));
  SYNC_REQUIRE(presentation == 5);
  SYNC_REQUIRE(static_cast<std::uint8_t>(out[0]) == 5);
}

SYNC_TEST(a_reader_rejects_a_torn_slot) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  SYNC_REQUIRE(writer.write(canvas_filled(7), kStride, 7));
  auto* header = reinterpret_cast<FrameRingHeader*>(mapping.data());
  // Forge a write in progress on the slot the reader will actually consult:
  // the newest frame's slot, not slot zero.
  const auto index = static_cast<std::uint32_t>(
      header->newest.load(std::memory_order_acquire) % kFrameRingSlots);
  header->slot[index].sequence.store(1, std::memory_order_release);
  const FrameRingReader reader(mapping);
  std::vector<std::byte> out(kFrameRingSlotBytes);
  std::uint64_t presentation = 0;
  SYNC_REQUIRE(!reader.read(out, kStride, presentation));
}

SYNC_TEST(a_reader_rejects_a_foreign_or_undersized_mapping) {
  std::vector<std::byte> too_small(64);
  const FrameRingReader small(too_small);
  SYNC_REQUIRE(!small.valid());
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  auto* header = reinterpret_cast<FrameRingHeader*>(mapping.data());
  header->magic = 0xDEADBEEF;
  const FrameRingReader foreign(mapping);
  SYNC_REQUIRE(!foreign.valid());
}

SYNC_TEST(the_writer_rejects_a_payload_that_is_not_the_canvas) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  std::vector<std::byte> short_frame(kFrameRingSlotBytes - 4);
  SYNC_REQUIRE(!writer.write(short_frame, kStride, 1));
  SYNC_REQUIRE(!writer.write(canvas_filled(1), kStride - 4, 1));
}

SYNC_TEST(a_writer_adopts_a_ring_another_writer_already_stamped) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter first(mapping);
  SYNC_REQUIRE(first.write(canvas_filled(9), kStride, 9));
  // The media source stamps the ring at creation and syncd opens it later;
  // adopting must not reset the sequence and lose the published frame.
  FrameRingWriter second(mapping);
  SYNC_REQUIRE(second.valid());
  const FrameRingReader reader(mapping);
  SYNC_REQUIRE(reader.newest_sequence() == 1);
}

}  // namespace
