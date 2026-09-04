#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sync/origin.hpp>

namespace noisefactor::sync::linux_control {

inline constexpr std::uint16_t kLinuxControlProtocolVersion = 1;
inline constexpr std::size_t kMaximumLinuxControlMessageBytes = 262'144;

enum class LinuxControlCommand {
  Pair,
  Decision,
  Status,
  Doctor,
  Pairings,
  Revoke,
};

struct LinuxControlRequest {
  LinuxControlCommand command = LinuxControlCommand::Status;
  std::uint64_t generation = 0;
  bool approved = false;
  NormalizedOrigin origin{};
};

struct LinuxControlDecodeResult {
  bool valid = false;
  LinuxControlRequest request{};
  std::string_view error_code{};
};

struct LinuxControlFrameDecodeResult {
  bool valid = false;
  std::string_view json{};
  std::string_view error_code{};
};

[[nodiscard]] auto decode_linux_control_request(std::string_view json) noexcept
    -> LinuxControlDecodeResult;
[[nodiscard]] auto encode_linux_control_frame(std::string_view json)
    -> std::vector<std::byte>;
[[nodiscard]] auto decode_linux_control_frame(
    std::span<const std::byte> bytes) noexcept
    -> LinuxControlFrameDecodeResult;
[[nodiscard]] auto encode_linux_control_json_string(std::string_view value)
    -> std::string;

}  // namespace noisefactor::sync::linux_control
