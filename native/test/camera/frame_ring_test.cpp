#include "test_harness.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sync/camera/frame_ring.hpp>

namespace {

using noisefactor::sync::camera::FrameRingHeader;
using noisefactor::sync::camera::FrameRingReader;
using noisefactor::sync::camera::FrameRingWriter;
using noisefactor::sync::camera::camera_clock_us;
using noisefactor::sync::camera::frame_ring_bytes;
using noisefactor::sync::camera::kBytesPerPixel;
using noisefactor::sync::camera::kCanvas;
using noisefactor::sync::camera::kFrameRingDemandTimeoutUs;
using noisefactor::sync::camera::kFrameRingSlotBytes;
using noisefactor::sync::camera::kFrameRingSlots;
using noisefactor::sync::camera::section_name;

constexpr std::size_t kStride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;

[[nodiscard]] auto canvas_filled(std::uint8_t value) -> std::vector<std::byte> {
  return std::vector<std::byte>(kFrameRingSlotBytes, static_cast<std::byte>(value));
}

SYNC_TEST(the_section_name_is_global) {
  // Global, because the media source runs in session 0 and syncd in the
  // user's session. A Local name would put the two in different namespaces
  // and each would quietly get a section of its own.
  SYNC_REQUIRE(section_name().rfind(L"Global\\", 0) == 0);
}

SYNC_TEST(the_windows_shm_filename_matches_desktop_contract) {
  SYNC_REQUIRE(noisefactor::sync::camera::windows_shm_filename() == L"SyncCamera.frames");
}

SYNC_TEST(demand_expires_so_a_closed_consumer_stops_the_sender) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  FrameRingReader reader(mapping);
  constexpr std::uint64_t kNow = 10'000'000'000;

  // Nothing has ever asked for a frame.
  SYNC_REQUIRE(!writer.has_demand(kNow));

  reader.record_demand(kNow);
  SYNC_REQUIRE(writer.has_demand(kNow));
  SYNC_REQUIRE(writer.has_demand(kNow + kFrameRingDemandTimeoutUs));
  // Past the timeout the consumer is gone and the sender must stop fitting
  // frames for it.
  SYNC_REQUIRE(!writer.has_demand(kNow + kFrameRingDemandTimeoutUs + 1));
}

SYNC_TEST(demand_from_a_clock_running_ahead_is_not_read_as_ancient) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  FrameRingReader reader(mapping);
  // The subtraction is unsigned, so a demand stamped even slightly ahead of
  // the reader's clock would otherwise wrap to an enormous age and read as
  // "nobody is watching" for the life of the process.
  reader.record_demand(10'000'000'000);
  SYNC_REQUIRE(writer.has_demand(9'999'999'000));
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

SYNC_TEST(write_with_writes_directly_into_destination_slot) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  const FrameRingReader reader(mapping);

  std::uint8_t fill_byte = 0x33;
  const auto direct_writer = [](void* ctx, std::span<std::byte> dest, std::size_t /*stride*/) noexcept -> bool {
    auto val = *static_cast<std::uint8_t*>(ctx);
    std::fill(dest.begin(), dest.end(), static_cast<std::byte>(val));
    return true;
  };

  SYNC_REQUIRE(writer.write_with(direct_writer, &fill_byte, 4242));
  SYNC_REQUIRE(reader.newest_sequence() == 1);

  std::vector<std::byte> out(kFrameRingSlotBytes);
  std::uint64_t presentation = 0;
  SYNC_REQUIRE(reader.read(out, kStride, presentation));
  SYNC_REQUIRE(presentation == 4242);
  SYNC_REQUIRE(static_cast<std::uint8_t>(out[0]) == 0x33);
  SYNC_REQUIRE(static_cast<std::uint8_t>(out[out.size() - 1]) == 0x33);
}

SYNC_TEST(write_with_reverts_sequence_if_writer_fails) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  const FrameRingReader reader(mapping);

  const auto failing_writer = [](void*, std::span<std::byte>, std::size_t) noexcept -> bool {
    return false;
  };

  SYNC_REQUIRE(!writer.write_with(failing_writer, nullptr, 999));
  SYNC_REQUIRE(reader.newest_sequence() == 0);
}

SYNC_TEST(monotonic_demand_heartbeat_under_concurrent_readers) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  FrameRingReader reader1(mapping);
  FrameRingReader reader2(mapping);
  FrameRingReader reader3(mapping);

  std::atomic<bool> start{false};
  std::vector<std::thread> threads;
  threads.reserve(3);

  // Thread 1 stamps high timestamps
  threads.emplace_back([&]() {
    while (!start.load(std::memory_order_relaxed)) {}
    for (std::uint64_t t = 1000; t <= 2000; t += 10) {
      reader1.record_demand(t);
    }
  });

  // Thread 2 stamps lagging timestamps (must never regress reader 1)
  threads.emplace_back([&]() {
    while (!start.load(std::memory_order_relaxed)) {}
    for (std::uint64_t t = 500; t <= 1500; t += 10) {
      reader2.record_demand(t);
    }
  });

  // Thread 3 stamps interleaved timestamps
  threads.emplace_back([&]() {
    while (!start.load(std::memory_order_relaxed)) {}
    for (std::uint64_t t = 800; t <= 1800; t += 10) {
      reader3.record_demand(t);
    }
  });

  start.store(true, std::memory_order_release);
  for (auto& t : threads) {
    t.join();
  }

  const auto* header = reinterpret_cast<const FrameRingHeader*>(mapping.data());
  // The final recorded demand must be the maximum (2000), never regressed by thread 2 or 3
  SYNC_REQUIRE(header->last_demand_us.load(std::memory_order_acquire) == 2000);
}

SYNC_TEST(concurrent_multi_reader_seqlock_consistency) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  FrameRingReader reader1(mapping);
  FrameRingReader reader2(mapping);
  FrameRingReader reader3(mapping);

  constexpr int kNumFrames = 40;
  std::atomic<bool> stop{false};
  std::atomic<int> valid_reads1{0};
  std::atomic<int> valid_reads2{0};
  std::atomic<int> valid_reads3{0};
  std::atomic<int> corruptions{0};

  auto reader_func = [&](FrameRingReader& r, std::atomic<int>& valid_reads) {
    std::vector<std::byte> out(kFrameRingSlotBytes);
    while (!stop.load(std::memory_order_relaxed)) {
      std::uint64_t presentation = 0;
      if (r.read(out, kStride, presentation)) {
        if (presentation > 0) {
          const auto expected_byte = static_cast<std::uint8_t>(presentation & 0xFF);
          // Verify that the frame is not torn: first byte, middle byte, and last byte match
          if (static_cast<std::uint8_t>(out[0]) != expected_byte ||
              static_cast<std::uint8_t>(out[out.size() / 2]) != expected_byte ||
              static_cast<std::uint8_t>(out[out.size() - 1]) != expected_byte) {
            corruptions.fetch_add(1, std::memory_order_relaxed);
          } else {
            valid_reads.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
      std::this_thread::yield();
    }
  };

  std::thread t1(reader_func, std::ref(reader1), std::ref(valid_reads1));
  std::thread t2(reader_func, std::ref(reader2), std::ref(valid_reads2));
  std::thread t3(reader_func, std::ref(reader3), std::ref(valid_reads3));

  for (int f = 1; f <= kNumFrames; ++f) {
    const auto frame = canvas_filled(static_cast<std::uint8_t>(f & 0xFF));
    SYNC_REQUIRE(writer.write(frame, kStride, static_cast<std::uint64_t>(f)));
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  stop.store(true, std::memory_order_release);
  t1.join();
  t2.join();
  t3.join();

  SYNC_REQUIRE(corruptions.load(std::memory_order_acquire) == 0);
  SYNC_REQUIRE(valid_reads1.load(std::memory_order_acquire) > 0);
  SYNC_REQUIRE(valid_reads2.load(std::memory_order_acquire) > 0);
  SYNC_REQUIRE(valid_reads3.load(std::memory_order_acquire) > 0);
}

SYNC_TEST(demand_multiplexing_staggered_lifetimes) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  FrameRingReader consumer_a(mapping);
  FrameRingReader consumer_b(mapping);

  constexpr std::uint64_t kT0 = 10'000'000;
  SYNC_REQUIRE(!writer.has_demand(kT0));

  // Consumer A connects
  consumer_a.record_demand(kT0);
  SYNC_REQUIRE(writer.has_demand(kT0));

  // Consumer B connects later at T0 + 500ms
  constexpr std::uint64_t kTB = kT0 + 500'000;
  consumer_b.record_demand(kTB);
  SYNC_REQUIRE(writer.has_demand(kTB));

  // Consumer A stops heartbeat at T0 + 800ms, but Consumer B stays active at T0 + 1200ms
  constexpr std::uint64_t kTB_active = kT0 + 1'200'000;
  consumer_b.record_demand(kTB_active);

  // At T0 + 1'500'000, consumer A has been silent for 1.5s (> timeout 1.0s),
  // but consumer B was active at 1.2s (only 300ms ago), so demand must remain active!
  constexpr std::uint64_t kCheckTime = kT0 + 1'500'000;
  SYNC_REQUIRE(writer.has_demand(kCheckTime));

  // Past Consumer B's timeout, demand finally expires
  constexpr std::uint64_t kExpiryTime = kTB_active + kFrameRingDemandTimeoutUs + 1;
  SYNC_REQUIRE(!writer.has_demand(kExpiryTime));
}

SYNC_TEST(an_adopted_previous_epoch_heartbeat_cannot_reject_fresh_demand) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  // A previous epoch's reader heartbeats, then the machine reboots: the
  // mapping outlives the reboot while the machine-wide steady_clock/QPC
  // restarts near zero. Model the old epoch as a stamp far ahead of the
  // current clock, which is what a longer previous uptime leaves behind.
  FrameRingWriter old_writer(mapping);
  FrameRingReader old_reader(mapping);
  const std::uint64_t now = camera_clock_us();
  constexpr std::uint64_t kPreviousEpochLead = 10 * kFrameRingDemandTimeoutUs;
  old_reader.record_demand(now + kPreviousEpochLead);
  SYNC_REQUIRE(old_writer.has_demand(now + kPreviousEpochLead));

  // The new epoch's writer adopts the stamped ring as it is. The fresh clock
  // is far behind the adopted stamp, so the old heartbeat reads stale.
  FrameRingWriter new_writer(mapping);
  SYNC_REQUIRE(new_writer.valid());
  SYNC_REQUIRE(!new_writer.has_demand(now));

  // A live consumer heartbeats. That one heartbeat must publish fresh demand,
  // so recovery is bounded instead of rejected for the life of the mapping.
  FrameRingReader new_reader(mapping);
  new_reader.record_demand(now);
  SYNC_REQUIRE(new_writer.has_demand(now));
  // And the replaced heartbeat expires normally afterwards.
  SYNC_REQUIRE(!new_writer.has_demand(now + kFrameRingDemandTimeoutUs + 1));
}

SYNC_TEST(a_stale_same_epoch_reader_cannot_regress_live_demand) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  FrameRingReader live_reader(mapping);
  FrameRingReader stale_reader(mapping);
  const std::uint64_t now = camera_clock_us();

  // A live consumer heartbeats now.
  live_reader.record_demand(now);
  SYNC_REQUIRE(writer.has_demand(now));

  // Another reader sampled the clock two timeouts ago and was descheduled
  // past the live heartbeat before arriving (media_source.cpp samples
  // camera_clock_us() before the call). The previous-epoch repair must not
  // let that old sample overwrite the live demand: a caller whose own
  // sample is no longer fresh leaves the stamp untouched.
  const std::uint64_t stale_sample = now - 2 * kFrameRingDemandTimeoutUs;
  stale_reader.record_demand(stale_sample);
  const auto* header = reinterpret_cast<const FrameRingHeader*>(mapping.data());
  SYNC_REQUIRE(header->last_demand_us.load(std::memory_order_acquire) == now);
  SYNC_REQUIRE(writer.has_demand(now));
  SYNC_REQUIRE(!writer.has_demand(now + kFrameRingDemandTimeoutUs + 1));
}

// The shutdown scrub a closing writer runs (the platform sinks' close paths)
// must go through this protocol, so a reader descheduled across it cannot
// validate a scrubbed slot as a complete frame: it loads the publication
// count before the scrub and validates its slot after it, which is the
// interleaving a plain zeroing of the mapping gets wrong (a zeroed sequence
// is an even, self-consistent value).
SYNC_TEST(a_paused_reader_cannot_validate_a_frame_over_a_shutdown_scrub) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  const FrameRingReader reader(mapping);
  const auto frame = canvas_filled(0x5A);
  SYNC_REQUIRE(writer.write(frame, kStride, 42));

  // The last complete frame stands while the writer lives.
  std::vector<std::byte> out(kFrameRingSlotBytes);
  std::uint64_t presentation = 0;
  SYNC_REQUIRE(reader.read(out, kStride, presentation));
  SYNC_REQUIRE(std::memcmp(out.data(), frame.data(), frame.size()) == 0);

  // The consumer is descheduled between the publication-count load and its
  // slot validation -- across the writer's whole scrub. These are the
  // opening steps of FrameRingReader::read(), verbatim.
  auto* header = reinterpret_cast<FrameRingHeader*>(mapping.data());
  const std::uint64_t newest = header->newest.load(std::memory_order_acquire);
  SYNC_REQUIRE(newest != 0);
  const auto index = static_cast<std::uint32_t>(newest % kFrameRingSlots);

  writer.scrub();

  // The consumer resumes: the remaining read() steps, verbatim. A scrubbed
  // slot must not validate as a complete frame.
  std::vector<std::byte> torn(kFrameRingSlotBytes);
  const std::byte* payload =
      mapping.data() + sizeof(FrameRingHeader) +
      static_cast<std::size_t>(index) * kFrameRingSlotBytes;
  const std::uint64_t before =
      header->slot[index].sequence.load(std::memory_order_acquire);
  std::memcpy(torn.data(), payload, kFrameRingSlotBytes);
  const std::uint64_t after =
      header->slot[index].sequence.load(std::memory_order_acquire);
  const bool would_complete = ((before & 1U) == 0) && (before == after);
  SYNC_REQUIRE(!would_complete);

  // The retry the protocol prescribes fails: the ring is retired, and a
  // reader constructed on the scrubbed mapping rejects it outright.
  std::uint64_t discarded = 0;
  SYNC_REQUIRE(!reader.read(out, kStride, discarded));
  const FrameRingReader late(mapping);
  SYNC_REQUIRE(!late.valid());

  // The scrub still scrubs: no retained pixels and no usable header.
  bool all_zero = true;
  for (const std::byte byte : torn) {
    if (byte != std::byte{0}) {
      all_zero = false;
      break;
    }
  }
  SYNC_REQUIRE(all_zero);
  SYNC_REQUIRE(header->magic == 0);
  SYNC_REQUIRE(!writer.valid());
}

// A consumer paused between its slot validation and its payload copy must
// discard the copy rather than return a half-scrubbed frame: the scrub marks
// every slot torn before any pixel clears, so the confirming load the
// protocol prescribes cannot match the one the consumer already accepted.
SYNC_TEST(a_reader_paused_inside_its_copy_discards_a_scrubbed_slot) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  const FrameRingReader reader(mapping);
  const auto frame = canvas_filled(0x77);
  SYNC_REQUIRE(writer.write(frame, kStride, 7));

  auto* header = reinterpret_cast<FrameRingHeader*>(mapping.data());
  const std::uint64_t newest = header->newest.load(std::memory_order_acquire);
  const auto index = static_cast<std::uint32_t>(newest % kFrameRingSlots);
  // The consumer's slot validation succeeds, and it is descheduled inside
  // the copy that follows -- across the writer's whole scrub.
  const std::uint64_t before =
      header->slot[index].sequence.load(std::memory_order_acquire);
  SYNC_REQUIRE((before & 1U) == 0);

  writer.scrub();

  std::vector<std::byte> torn(kFrameRingSlotBytes);
  const std::byte* payload =
      mapping.data() + sizeof(FrameRingHeader) +
      static_cast<std::size_t>(index) * kFrameRingSlotBytes;
  std::memcpy(torn.data(), payload, kFrameRingSlotBytes);
  const std::uint64_t after =
      header->slot[index].sequence.load(std::memory_order_acquire);
  // The confirming load the protocol prescribes rejects the copy as torn,
  // whatever half of it landed before the scrub.
  SYNC_REQUIRE(before != after);
  std::uint64_t discarded = 0;
  SYNC_REQUIRE(!reader.read(torn, kStride, discarded));
}

}  // namespace
