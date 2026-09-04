#include "../test_harness.hpp"

#include <sync/platform/linux_control_protocol.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace linux_control = noisefactor::sync::linux_control;

SYNC_TEST(linux_control_protocol_decodes_each_exact_command_shape) {
  const std::array cases = {
      std::pair{R"({"version":1,"command":"pair"})",
                linux_control::LinuxControlCommand::Pair},
      std::pair{R"({"version":1,"command":"decision","generation":7,"approved":true})",
                linux_control::LinuxControlCommand::Decision},
      std::pair{R"({"version":1,"command":"status"})",
                linux_control::LinuxControlCommand::Status},
      std::pair{R"({"version":1,"command":"doctor"})",
                linux_control::LinuxControlCommand::Doctor},
      std::pair{R"({"version":1,"command":"pairings"})",
                linux_control::LinuxControlCommand::Pairings},
      std::pair{R"({"version":1,"command":"revoke","origin":"https://visuals.example"})",
                linux_control::LinuxControlCommand::Revoke},
  };

  for (const auto& [json, command] : cases) {
    const auto decoded = linux_control::decode_linux_control_request(json);
    SYNC_REQUIRE(decoded.valid);
    SYNC_REQUIRE(decoded.request.command == command);
  }

  const auto decision = linux_control::decode_linux_control_request(
      R"({"approved":true,"generation":7,"command":"decision","version":1})");
  SYNC_REQUIRE(decision.valid);
  SYNC_REQUIRE(decision.request.generation == 7);
  SYNC_REQUIRE(decision.request.approved);

  const auto revoke = linux_control::decode_linux_control_request(
      R"({"origin":"HTTPS://Visuals.Example:443","command":"revoke","version":1})");
  SYNC_REQUIRE(revoke.valid);
  SYNC_REQUIRE(revoke.request.origin.view() == "https://visuals.example");
}

SYNC_TEST(linux_control_protocol_rejects_ambiguous_or_unsafe_requests) {
  for (const std::string_view rejected : {
           R"({"version":0,"command":"pair"})",
           R"({"version":2,"command":"pair"})",
           R"({"version":1,"version":1,"command":"pair"})",
           R"({"version":1,"command":"pair","unknown":true})",
           R"({"version":1,"command":"decision","generation":0,"approved":true})",
           R"({"version":1,"command":"decision","generation":7})",
           R"({"version":1,"command":"pair","generation":7})",
           R"({"version":1,"command":"revoke","origin":"http://remote.example"})",
           R"({"version":1,"command":"revoke","origin":"bad\u000aorigin"})",
           R"({"version":1,"command":"pair"} trailing)",
       }) {
    const auto decoded = linux_control::decode_linux_control_request(rejected);
    SYNC_REQUIRE(!decoded.valid);
    SYNC_REQUIRE(!decoded.error_code.empty());
  }
}

SYNC_TEST(linux_control_frames_are_big_endian_bounded_and_single_message) {
  constexpr std::string_view json = R"({"version":1,"command":"status"})";
  const auto encoded = linux_control::encode_linux_control_frame(json);
  SYNC_REQUIRE(encoded.size() == json.size() + 4);
  SYNC_REQUIRE(encoded[0] == std::byte{0});
  SYNC_REQUIRE(encoded[1] == std::byte{0});
  SYNC_REQUIRE(encoded[2] == std::byte{0});
  SYNC_REQUIRE(encoded[3] == std::byte{32});

  const auto decoded = linux_control::decode_linux_control_frame(encoded);
  SYNC_REQUIRE(decoded.valid);
  SYNC_REQUIRE(decoded.json == json);

  std::vector<std::byte> zero(4, std::byte{0});
  SYNC_REQUIRE(!linux_control::decode_linux_control_frame(zero).valid);

  std::vector<std::byte> oversized(4, std::byte{0});
  const std::uint32_t length = static_cast<std::uint32_t>(
      linux_control::kMaximumLinuxControlMessageBytes + 1);
  oversized[0] = static_cast<std::byte>((length >> 24U) & 0xffU);
  oversized[1] = static_cast<std::byte>((length >> 16U) & 0xffU);
  oversized[2] = static_cast<std::byte>((length >> 8U) & 0xffU);
  oversized[3] = static_cast<std::byte>(length & 0xffU);
  SYNC_REQUIRE(!linux_control::decode_linux_control_frame(oversized).valid);

  auto concatenated = encoded;
  concatenated.insert(concatenated.end(), encoded.begin(), encoded.end());
  SYNC_REQUIRE(!linux_control::decode_linux_control_frame(concatenated).valid);
  SYNC_REQUIRE(linux_control::encode_linux_control_frame({}).empty());
  SYNC_REQUIRE(linux_control::encode_linux_control_frame(
                   std::string(linux_control::kMaximumLinuxControlMessageBytes +
                                   1,
                               'x'))
                   .empty());
}

SYNC_TEST(linux_control_json_strings_escape_terminal_control_bytes) {
  std::string input = "a\"\\";
  input.push_back('\x01');
  input.push_back('\n');
  SYNC_REQUIRE(linux_control::encode_linux_control_json_string(input) ==
               "\"a\\\"\\\\\\u0001\\u000a\"");
}

}  // namespace
