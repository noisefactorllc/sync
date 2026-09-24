#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <sync/render/render_ring.hpp>

namespace {

using namespace noisefactor::sync::render;

constexpr RenderRingGeometry kSmall{.width = 64, .height = 48};

struct Ring {
  explicit Ring(const RenderRingGeometry& geometry = kSmall)
      : mapping(*render_ring_bytes(geometry)), writer(mapping, geometry, 1234) {}
  // Heap storage aligned well past what the atomics need, like a real mapping.
  std::vector<std::byte> mapping;
  RenderRingWriter writer;
};

[[nodiscard]] auto filled(const RenderRingGeometry& geometry, std::uint8_t value)
    -> std::vector<std::byte> {
  return std::vector<std::byte>(static_cast<std::size_t>(geometry.width) * 4 * geometry.height,
                                static_cast<std::byte>(value));
}

[[nodiscard]] auto all_equal(std::span<const std::byte> bytes, std::uint8_t value) -> bool {
  for (const std::byte b : bytes) {
    if (b != static_cast<std::byte>(value)) return false;
  }
  return true;
}

SYNC_TEST(geometry_outside_the_wire_limits_has_no_ring) {
  SYNC_REQUIRE(!render_ring_bytes({.width = 0, .height = 10}).has_value());
  SYNC_REQUIRE(!render_ring_bytes({.width = 4097, .height = 10}).has_value());
  SYNC_REQUIRE(!render_ring_bytes({.width = 10, .height = 4097}).has_value());
  SYNC_REQUIRE(!render_ring_bytes({.width = 10, .height = 10, .color_space = 9}).has_value());
  SYNC_REQUIRE(!render_ring_bytes({.width = 10, .height = 10, .alpha_mode = 0}).has_value());
  const auto full = render_ring_bytes({.width = 1920, .height = 1080});
  SYNC_REQUIRE(full.has_value());
  SYNC_REQUIRE(*full == render_ring_payload_offset() + std::size_t{1920} * 4 * 1080 * 3);
  SYNC_REQUIRE(render_ring_payload_offset() % kRenderRingPayloadAlignment == 0);
}

SYNC_TEST(a_writer_refuses_a_mapping_too_small_for_its_geometry) {
  std::vector<std::byte> mapping(*render_ring_bytes(kSmall) - 1);
  RenderRingWriter writer(mapping, kSmall, 1);
  SYNC_REQUIRE(!writer.valid());
}

SYNC_TEST(nothing_is_readable_before_the_first_publish) {
  Ring ring;
  RenderRingReader reader(ring.mapping);
  SYNC_REQUIRE(reader.valid());
  SYNC_REQUIRE(reader.newest_frame() == 0);
  SYNC_REQUIRE(!reader.acquire().held());
}

SYNC_TEST(frames_round_trip_with_their_number_and_time) {
  Ring ring;
  RenderRingReader reader(ring.mapping);
  for (std::uint8_t value = 1; value <= 5; ++value) {
    const auto frame = filled(kSmall, value);
    SYNC_REQUIRE(ring.writer.write(frame, kSmall.width * 4, 1000U * value));
    RenderFrameLease lease = reader.acquire();
    SYNC_REQUIRE(lease.held());
    SYNC_REQUIRE(lease.info().frame == value);
    SYNC_REQUIRE(lease.info().presentation_time_us == 1000U * value);
    SYNC_REQUIRE(lease.info().width == kSmall.width);
    SYNC_REQUIRE(lease.info().row_stride == kSmall.width * 4);
    SYNC_REQUIRE(all_equal(lease.payload(), value));
  }
  SYNC_REQUIRE(reader.newest_frame() == 5);
}

SYNC_TEST(a_padded_source_stride_is_copied_row_by_row) {
  Ring ring;
  const std::size_t stride = kSmall.width * 4 + 16;
  std::vector<std::byte> source(stride * kSmall.height, std::byte{0xEE});
  for (std::uint32_t y = 0; y < kSmall.height; ++y) {
    std::memset(source.data() + y * stride, static_cast<int>(y), kSmall.width * 4);
  }
  SYNC_REQUIRE(ring.writer.write(source, stride, 1));
  RenderRingReader reader(ring.mapping);
  RenderFrameLease lease = reader.acquire();
  SYNC_REQUIRE(lease.held());
  for (std::uint32_t y = 0; y < kSmall.height; ++y) {
    SYNC_REQUIRE(all_equal(lease.payload().subspan(y * kSmall.width * 4, kSmall.width * 4),
                           static_cast<std::uint8_t>(y)));
  }
}

SYNC_TEST(a_leased_slot_is_never_overwritten) {
  Ring ring;
  RenderRingReader reader(ring.mapping);
  SYNC_REQUIRE(ring.writer.write(filled(kSmall, 7), kSmall.width * 4, 1));
  RenderFrameLease lease = reader.acquire();
  SYNC_REQUIRE(lease.held());
  // Many laps of the ring while the lease is held: the writer must keep
  // publishing through the other two slots and leave this one alone.
  for (std::uint8_t value = 8; value < 40; ++value) {
    SYNC_REQUIRE(ring.writer.write(filled(kSmall, value), kSmall.width * 4, value));
  }
  SYNC_REQUIRE(all_equal(lease.payload(), 7));
  SYNC_REQUIRE(lease.info().frame == 1);
  SYNC_REQUIRE(reader.newest_frame() == 33);
  lease.release();
  RenderFrameLease next = reader.acquire();
  SYNC_REQUIRE(next.held());
  SYNC_REQUIRE(next.info().frame == 33);
  SYNC_REQUIRE(all_equal(next.payload(), 39));
}

SYNC_TEST(a_failed_fill_publishes_nothing) {
  Ring ring;
  RenderRingReader reader(ring.mapping);
  SYNC_REQUIRE(ring.writer.write(filled(kSmall, 3), kSmall.width * 4, 1));
  const auto fail = [](void*, std::span<std::byte> destination, std::size_t) noexcept -> bool {
    std::memset(destination.data(), 0x55, destination.size() / 2);
    return false;
  };
  SYNC_REQUIRE(!ring.writer.write_with(fail, nullptr, 2));
  SYNC_REQUIRE(reader.newest_frame() == 1);
  RenderFrameLease lease = reader.acquire();
  SYNC_REQUIRE(lease.held());
  SYNC_REQUIRE(all_equal(lease.payload(), 3));
}

SYNC_TEST(a_reader_rejects_what_it_cannot_trust) {
  Ring ring;
  {
    auto copy = ring.mapping;
    reinterpret_cast<RenderRingHeader*>(copy.data())->version = kRenderRingVersion + 1;
    SYNC_REQUIRE(!RenderRingReader(copy).valid());
  }
  {
    auto copy = ring.mapping;
    reinterpret_cast<RenderRingHeader*>(copy.data())->magic = 0;
    SYNC_REQUIRE(!RenderRingReader(copy).valid());
  }
  {
    auto copy = ring.mapping;
    // A stride that would walk the reader out of the slot.
    reinterpret_cast<RenderRingHeader*>(copy.data())->row_stride += 4;
    SYNC_REQUIRE(!RenderRingReader(copy).valid());
  }
  {
    auto copy = ring.mapping;
    // A geometry larger than the mapping it came in.
    reinterpret_cast<RenderRingHeader*>(copy.data())->height += 1;
    reinterpret_cast<RenderRingHeader*>(copy.data())->slot_bytes += kSmall.width * 4;
    SYNC_REQUIRE(!RenderRingReader(copy).valid());
  }
  {
    std::vector<std::byte> truncated(ring.mapping.begin(), ring.mapping.end() - 1);
    SYNC_REQUIRE(!RenderRingReader(truncated).valid());
  }
}

SYNC_TEST(writer_liveness_follows_close_and_heartbeat) {
  Ring ring;
  RenderRingReader reader(ring.mapping);
  const std::uint64_t now = render_clock_us();
  SYNC_REQUIRE(reader.writer_alive(now, 1'000'000));
  ring.writer.heartbeat(now - 5'000'000);
  SYNC_REQUIRE(!reader.writer_alive(now, 1'000'000));
  ring.writer.heartbeat(now);
  SYNC_REQUIRE(reader.writer_alive(now, 1'000'000));
  ring.writer.close();
  SYNC_REQUIRE(reader.writer_closed());
  SYNC_REQUIRE(!reader.writer_alive(now, 1'000'000));
}

SYNC_TEST(reader_heartbeat_is_visible_to_the_writer) {
  Ring ring;
  RenderRingReader reader(ring.mapping);
  const std::uint64_t now = render_clock_us();
  SYNC_REQUIRE(!ring.writer.reader_alive(now, 1'000'000));
  reader.heartbeat(now);
  SYNC_REQUIRE(ring.writer.reader_alive(now, 1'000'000));
  SYNC_REQUIRE(!ring.writer.reader_alive(now + 2'000'000, 1'000'000));
}

SYNC_TEST(concurrent_leases_never_see_a_torn_or_rewritten_frame) {
  // Every byte of frame n is n mod 251. A lease whose payload is not uniform,
  // or not the value its frame number predicts, saw the writer inside it.
  constexpr RenderRingGeometry geometry{.width = 96, .height = 64};
  std::vector<std::byte> mapping(*render_ring_bytes(geometry));
  RenderRingWriter writer(mapping, geometry, 1);
  RenderRingReader reader(mapping);
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> written{0};

  std::thread producer([&] {
    std::uint64_t frame = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      const std::uint64_t next = frame + 1;
      const auto fill = [](void* context, std::span<std::byte> destination,
                           std::size_t) noexcept -> bool {
        const auto value = *static_cast<const std::uint64_t*>(context) % 251U;
        std::memset(destination.data(), static_cast<int>(value), destination.size());
        return true;
      };
      std::uint64_t n = next;
      if (writer.write_with(fill, &n, n)) {
        frame = next;
        written.store(frame, std::memory_order_relaxed);
      }
    }
  });

  // Bounded by work, not by wall time, so a slow machine (or a sanitizer
  // build) runs the same test rather than a shorter one. The ceiling only
  // stops a broken writer from hanging the suite.
  constexpr std::uint64_t kLeases = 500;
  constexpr std::uint64_t kDistinctFrames = 50;
  std::uint64_t checked = 0;
  std::uint64_t distinct = 0;
  std::uint64_t last_frame = 0;
  bool torn = false;
  bool regressed = false;
  const auto ceiling = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while ((checked < kLeases || distinct < kDistinctFrames) &&
         std::chrono::steady_clock::now() < ceiling) {
    RenderFrameLease lease = reader.acquire();
    if (!lease.held()) continue;
    const std::uint64_t frame = lease.info().frame;
    if (frame < last_frame) regressed = true;
    if (frame != last_frame) ++distinct;
    last_frame = frame;
    // Read the payload twice with the writer running flat out in between: a
    // leased slot must not change at all.
    const auto expected = static_cast<std::uint8_t>(frame % 251U);
    if (!all_equal(lease.payload(), expected)) torn = true;
    std::this_thread::yield();
    if (!all_equal(lease.payload(), expected)) torn = true;
    ++checked;
  }
  stop.store(true);
  producer.join();

  SYNC_REQUIRE(!torn);
  SYNC_REQUIRE(!regressed);
  SYNC_REQUIRE(checked >= kLeases);
  SYNC_REQUIRE(distinct >= kDistinctFrames);
  SYNC_REQUIRE(written.load() >= last_frame);
}

#if !defined(_WIN32)

[[nodiscard]] auto temp_ring_path(const char* tag) -> std::string {
  return (std::filesystem::temp_directory_path() /
          ("sync-render-ring-test-" + std::to_string(::getpid()) + "-" + tag))
      .string();
}

SYNC_TEST(a_section_round_trips_between_creator_and_opener) {
  const std::string path = temp_ring_path("roundtrip");
  std::string error;
  auto created = RenderRingSection::create(path, *render_ring_bytes(kSmall), error);
  SYNC_REQUIRE(created.has_value());
  RenderRingWriter writer(created->bytes(), kSmall, 1);
  SYNC_REQUIRE(writer.write(filled(kSmall, 9), kSmall.width * 4, 5));

  struct stat st{};
  SYNC_REQUIRE(::stat(path.c_str(), &st) == 0);
  SYNC_REQUIRE((st.st_mode & 0777) == 0600);

  auto opened = RenderRingSection::open(path, error);
  SYNC_REQUIRE(opened.has_value());
  RenderRingReader reader(opened->bytes());
  SYNC_REQUIRE(reader.valid());
  RenderFrameLease lease = reader.acquire();
  SYNC_REQUIRE(lease.held());
  SYNC_REQUIRE(all_equal(lease.payload(), 9));
  lease.release();

  // The creator removes the name when it goes, so no new reader can attach to
  // a ring whose writer has gone.
  created.reset();
  SYNC_REQUIRE(::stat(path.c_str(), &st) != 0);
}

SYNC_TEST(a_section_others_can_read_is_refused) {
  const std::string path = temp_ring_path("mode");
  std::string error;
  auto created = RenderRingSection::create(path, *render_ring_bytes(kSmall), error);
  SYNC_REQUIRE(created.has_value());
  SYNC_REQUIRE(::chmod(path.c_str(), 0644) == 0);
  SYNC_REQUIRE(!RenderRingSection::open(path, error).has_value());
}

SYNC_TEST(creating_over_a_stale_section_replaces_it) {
  const std::string path = temp_ring_path("stale");
  std::string error;
  auto first = RenderRingSection::create(path, *render_ring_bytes(kSmall), error);
  SYNC_REQUIRE(first.has_value());
  RenderRingWriter old_writer(first->bytes(), kSmall, 1);
  SYNC_REQUIRE(old_writer.write(filled(kSmall, 1), kSmall.width * 4, 1));
  auto second = RenderRingSection::create(path, *render_ring_bytes(kSmall), error);
  SYNC_REQUIRE(second.has_value());
  auto opened = RenderRingSection::open(path, error);
  SYNC_REQUIRE(opened.has_value());
  // The new section is fresh: no magic until its own writer stamps it.
  SYNC_REQUIRE(!RenderRingReader(opened->bytes()).valid());
  // The stale owner going away must not take the new writer's name with it.
  first.reset();
  struct stat st{};
  SYNC_REQUIRE(::stat(path.c_str(), &st) == 0);
}

#else

SYNC_TEST(a_section_round_trips_between_creator_and_opener) {
  const std::string name = "SyncRenderRingTest-" + std::to_string(::GetCurrentProcessId());
  std::string error;
  auto created = RenderRingSection::create(name, *render_ring_bytes(kSmall), error);
  SYNC_REQUIRE(created.has_value());
  RenderRingWriter writer(created->bytes(), kSmall, 1);
  SYNC_REQUIRE(writer.write(filled(kSmall, 9), kSmall.width * 4, 5));
  SYNC_REQUIRE(!RenderRingSection::create(name, *render_ring_bytes(kSmall), error).has_value());
  auto opened = RenderRingSection::open(name, error);
  SYNC_REQUIRE(opened.has_value());
  RenderRingReader reader(opened->bytes());
  SYNC_REQUIRE(reader.valid());
  RenderFrameLease lease = reader.acquire();
  SYNC_REQUIRE(lease.held());
  SYNC_REQUIRE(all_equal(lease.payload(), 9));
}

#endif

}  // namespace
