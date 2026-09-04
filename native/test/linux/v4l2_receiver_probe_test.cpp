#include "../test_harness.hpp"

#include <sync/platform/v4l2_receiver_probe.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace probe = noisefactor::sync::v4l2_probe;

SYNC_TEST(v4l2_receiver_probe_requires_exact_bounded_arguments) {
  const std::array<std::string_view, 6> valid{
      "--device", "/dev/video12", "--frames", "90", "--output", "/tmp/out"};
  const auto parsed = probe::parse(valid);
  SYNC_REQUIRE(parsed.ok());
  SYNC_REQUIRE(parsed.options.device == "/dev/video12");
  SYNC_REQUIRE(parsed.options.frame_count == 90);
  SYNC_REQUIRE(parsed.options.output_directory == "/tmp/out");

  const std::array<std::string_view, 6> traversal{
      "--device", "/dev/video1/../video2", "--frames", "90", "--output", "/tmp/out"};
  SYNC_REQUIRE(!probe::parse(traversal).ok());
  const std::array<std::string_view, 6> zero{
      "--device", "/dev/video0", "--frames", "0", "--output", "/tmp/out"};
  SYNC_REQUIRE(!probe::parse(zero).ok());
  const std::array<std::string_view, 6> too_many{
      "--device", "/dev/video0", "--frames", "601", "--output", "/tmp/out"};
  SYNC_REQUIRE(!probe::parse(too_many).ok());
  const std::array<std::string_view, 8> duplicate{
      "--device", "/dev/video0", "--frames", "2", "--output", "/tmp/out",
      "--frames", "3"};
  SYNC_REQUIRE(!probe::parse(duplicate).ok());
}

SYNC_TEST(v4l2_receiver_probe_checksum_and_uniqueness_are_deterministic) {
  const std::array<std::byte, 5> bytes{
      std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  SYNC_REQUIRE(probe::fnv1a64(bytes) == 0x3378e3d0c52edfafULL);
  const std::array<std::uint64_t, 6> checksums{9, 4, 9, 8, 4, 7};
  SYNC_REQUIRE(probe::unique_checksum_count(checksums) == 4);
  SYNC_REQUIRE(probe::unique_checksum_count({}) == 0);
}
