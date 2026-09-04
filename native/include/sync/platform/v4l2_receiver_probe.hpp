#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace noisefactor::sync::v4l2_probe {

struct Options {
  std::string device;
  std::string output_directory;
  std::uint32_t frame_count = 0;
};

struct ParseResult {
  Options options;
  std::string error;
  [[nodiscard]] auto ok() const noexcept -> bool { return error.empty(); }
};

[[nodiscard]] auto parse(std::span<const std::string_view> arguments)
    -> ParseResult;
[[nodiscard]] auto fnv1a64(std::span<const std::byte> bytes) noexcept
    -> std::uint64_t;
[[nodiscard]] auto unique_checksum_count(
    std::span<const std::uint64_t> checksums) noexcept -> std::size_t;
[[nodiscard]] auto valid_camera_path(std::string_view path) noexcept -> bool;

}  // namespace noisefactor::sync::v4l2_probe
