#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <sync/control.hpp>
#include <sync/secure_memory.hpp>
#include <sync/server.hpp>

namespace {

namespace control = noisefactor::sync::control;

class RecordingCleanseObserver final
    : public noisefactor::sync::CleanseObserver {
 public:
  void after_cleanse(std::span<const std::byte> bytes) noexcept override {
    ++calls;
    largest = std::max(largest, bytes.size());
    for (const std::byte byte : bytes) {
      if (byte != std::byte{0}) all_zero = false;
    }
  }

  std::size_t calls = 0;
  std::size_t largest = 0;
  bool all_zero = true;
};

void require_error(std::string_view json, control::ParseError error) {
  const auto result = control::parse_message(json);
  SYNC_REQUIRE(result.error == error);
  SYNC_REQUIRE(!result.message.has_value());
}

std::string raw_bytes(std::initializer_list<std::uint8_t> values) {
  std::string result;
  result.reserve(values.size());
  for (const auto value : values) {
    result.push_back(static_cast<char>(value));
  }
  return result;
}

} // namespace

SYNC_TEST(control_parses_valid_hello_in_any_object_field_order) {
  const auto result =
      control::parse_message("{\"protocolVersions\":[65535,1],\"token\":\"pair-"
                             "token\",\"type\":\"hello\"}");

  SYNC_REQUIRE(result.error == control::ParseError::None);
  SYNC_REQUIRE(result.message.has_value());
  SYNC_REQUIRE(result.message->type == control::MessageType::Hello);
  SYNC_REQUIRE(result.message->token == "pair-token");
  SYNC_REQUIRE(result.message->protocol_versions.size() == 2);
  SYNC_REQUIRE(result.message->protocol_versions[0] == 65535);
  SYNC_REQUIRE(result.message->protocol_versions[1] == 1);
}

SYNC_TEST(control_message_cleanses_token_on_move_and_explicit_clear) {
  auto parsed = control::parse_message(
      "{\"type\":\"hello\",\"token\":\"pair-token\",\"protocolVersions\":[1]}");
  SYNC_REQUIRE(parsed.message.has_value());
  control::ControlMessage moved = std::move(*parsed.message);
  SYNC_REQUIRE(parsed.message->token.empty());
  SYNC_REQUIRE(moved.token == "pair-token");
  moved.clear_sensitive();
  SYNC_REQUIRE(moved.token.empty());
}

SYNC_TEST(control_token_parser_uses_cleansed_fixed_scratch_on_success_and_failure) {
  RecordingCleanseObserver observer;
  const std::string oversized(257, 's');
  const auto rejected = control::parse_message(
      "{\"type\":\"hello\",\"token\":\"" + oversized +
          "\",\"protocolVersions\":[1]}",
      &observer);
  SYNC_REQUIRE(rejected.error == control::ParseError::InvalidValue);
  SYNC_REQUIRE(!rejected.message.has_value());
  SYNC_REQUIRE(observer.calls >= 1);
  SYNC_REQUIRE(observer.largest >= 256);
  SYNC_REQUIRE(observer.all_zero);

  auto accepted = control::parse_message(
      "{\"type\":\"hello\",\"token\":\"pair-token\",\"protocolVersions\":[1]}",
      &observer);
  SYNC_REQUIRE(accepted.message.has_value());
  accepted.message->clear_sensitive(&observer);
  SYNC_REQUIRE(accepted.message->token.empty());
  SYNC_REQUIRE(observer.all_zero);
}

SYNC_TEST(control_parses_valid_create_sender_with_utf8_and_json_unescaping) {
  const auto result = control::parse_message(
      "{\"name\":\"Caf\\u00e9 \\ud83c\\udfa5\",\"type\":\"createSender\"}");

  SYNC_REQUIRE(result.error == control::ParseError::None);
  SYNC_REQUIRE(result.message->type == control::MessageType::CreateSender);
  SYNC_REQUIRE(result.message->name == "Caf\xc3\xa9 \xf0\x9f\x8e\xa5");
}

SYNC_TEST(control_parses_valid_stats_and_close_sender_requests) {
  const auto stats = control::parse_message(
      "{\"senderId\":\"sender_A-9\",\"type\":\"getStats\"}");
  const auto close = control::parse_message(
      "{\"type\":\"closeSender\",\"senderId\":\"sender_A-9\"}");

  SYNC_REQUIRE(stats.error == control::ParseError::None);
  SYNC_REQUIRE(stats.message->type == control::MessageType::GetStats);
  SYNC_REQUIRE(stats.message->sender_id == "sender_A-9");
  SYNC_REQUIRE(close.error == control::ParseError::None);
  SYNC_REQUIRE(close.message->type == control::MessageType::CloseSender);
  SYNC_REQUIRE(close.message->sender_id == "sender_A-9");
}

SYNC_TEST(
    control_rejects_malformed_json_trailing_data_escapes_surrogates_and_utf8) {
  require_error("", control::ParseError::MalformedJson);
  require_error("{", control::ParseError::MalformedJson);
  require_error("{\"type\":\"getStats\",}", control::ParseError::MalformedJson);
  require_error("{\"type\":\"getStats\"} trailing",
                control::ParseError::MalformedJson);
  require_error("{\"type\":\"createSender\",\"name\":\"\\x\"}",
                control::ParseError::MalformedJson);
  require_error("{\"type\":\"createSender\",\"name\":\"\\ud800\"}",
                control::ParseError::MalformedJson);
  require_error(std::string("{\"type\":\"createSender\",\"name\":\"") +
                    static_cast<char>(0xc0) + static_cast<char>(0xaf) + "\"}",
                control::ParseError::MalformedJson);
}

SYNC_TEST(control_rejects_every_duplicate_field) {
  require_error("{\"type\":\"hello\",\"type\":\"hello\",\"token\":\"x\","
                "\"protocolVersions\":[1]}",
                control::ParseError::DuplicateField);
  require_error("{\"type\":\"hello\",\"token\":\"x\",\"token\":\"y\","
                "\"protocolVersions\":[1]}",
                control::ParseError::DuplicateField);
  require_error("{\"type\":\"hello\",\"token\":\"x\",\"protocolVersions\":[1],"
                "\"protocolVersions\":[2]}",
                control::ParseError::DuplicateField);
  require_error("{\"type\":\"createSender\",\"name\":\"a\",\"name\":\"b\"}",
                control::ParseError::DuplicateField);
  require_error("{\"type\":\"getStats\",\"senderId\":\"a\",\"senderId\":\"b\"}",
                control::ParseError::DuplicateField);
}

SYNC_TEST(control_rejects_unknown_and_schema_extra_fields) {
  require_error("{\"type\":\"createSender\",\"name\":\"a\",\"extra\":1}",
                control::ParseError::UnknownField);
  require_error("{\"type\":\"createSender\",\"name\":\"a\",\"token\":\"x\"}",
                control::ParseError::UnknownField);
}

SYNC_TEST(control_rejects_each_missing_required_field) {
  require_error("{}", control::ParseError::MissingField);
  require_error("{\"type\":\"hello\",\"protocolVersions\":[1]}",
                control::ParseError::MissingField);
  require_error("{\"type\":\"hello\",\"token\":\"x\"}",
                control::ParseError::MissingField);
  require_error("{\"type\":\"createSender\"}",
                control::ParseError::MissingField);
  require_error("{\"type\":\"getStats\"}", control::ParseError::MissingField);
  require_error("{\"type\":\"closeSender\"}",
                control::ParseError::MissingField);
}

SYNC_TEST(control_rejects_wrong_json_types_for_each_field) {
  require_error("{\"type\":1}", control::ParseError::InvalidType);
  require_error("{\"type\":\"hello\",\"token\":1,\"protocolVersions\":[1]}",
                control::ParseError::InvalidType);
  require_error("{\"type\":\"hello\",\"token\":\"x\",\"protocolVersions\":1}",
                control::ParseError::InvalidType);
  require_error("{\"type\":\"createSender\",\"name\":false}",
                control::ParseError::InvalidType);
  require_error("{\"type\":\"getStats\",\"senderId\":null}",
                control::ParseError::InvalidType);
}

SYNC_TEST(control_enforces_printable_ascii_token_boundaries) {
  const std::string token256(256, 't');
  const auto valid =
      control::parse_message("{\"type\":\"hello\",\"token\":\"" + token256 +
                             "\",\"protocolVersions\":[1]}");
  SYNC_REQUIRE(valid.error == control::ParseError::None);

  require_error("{\"type\":\"hello\",\"token\":\"\",\"protocolVersions\":[1]}",
                control::ParseError::InvalidValue);
  require_error(
      "{\"type\":\"hello\",\"token\":\"\\n\",\"protocolVersions\":[1]}",
      control::ParseError::InvalidValue);
  require_error("{\"type\":\"hello\",\"token\":\"\\u00e9\","
                "\"protocolVersions\":[1]}",
                control::ParseError::InvalidValue);
  require_error("{\"type\":\"hello\",\"token\":\"" + std::string(257, 't') +
                    "\",\"protocolVersions\":[1]}",
                control::ParseError::InvalidValue);
}

SYNC_TEST(control_enforces_unique_uint16_protocol_version_boundaries) {
  std::string sixteen = "[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,65535]";
  SYNC_REQUIRE(control::parse_message(
                   "{\"type\":\"hello\",\"token\":\"x\",\"protocolVersions\":" +
                   sixteen + "}")
                   .error == control::ParseError::None);

  require_error("{\"type\":\"hello\",\"token\":\"x\",\"protocolVersions\":[]}",
                control::ParseError::InvalidValue);
  require_error(
      "{\"type\":\"hello\",\"token\":\"x\",\"protocolVersions\":[1,1]}",
      control::ParseError::InvalidValue);
  require_error(
      "{\"type\":\"hello\",\"token\":\"x\",\"protocolVersions\":[1.0]}",
      control::ParseError::InvalidValue);
  require_error(
      "{\"type\":\"hello\",\"token\":\"x\",\"protocolVersions\":[true]}",
      control::ParseError::InvalidType);
  require_error(
      "{\"type\":\"hello\",\"token\":\"x\",\"protocolVersions\":[1,]}",
      control::ParseError::MalformedJson);
  require_error(
      "{\"type\":\"hello\",\"token\":\"x\",\"protocolVersions\":[65536]}",
      control::ParseError::InvalidValue);
  require_error(
      "{\"type\":\"hello\",\"token\":\"x\",\"protocolVersions\":[-1]}",
      control::ParseError::InvalidValue);
  require_error("{\"type\":\"hello\",\"token\":\"x\",\"protocolVersions\":"
                "[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17]}",
                control::ParseError::InvalidValue);
}

SYNC_TEST(control_enforces_sender_name_utf8_byte_and_control_boundaries) {
  const std::string name64(64, 'n');
  SYNC_REQUIRE(control::parse_message("{\"type\":\"createSender\",\"name\":\"" +
                                      name64 + "\"}")
                   .error == control::ParseError::None);
  const std::string escaped32(32, 'x');
  std::string escaped_utf8_name;
  for (char ignored : escaped32) {
    (void)ignored;
    escaped_utf8_name += "\\u00e9";
  }
  SYNC_REQUIRE(control::parse_message("{\"type\":\"createSender\",\"name\":\"" +
                                      escaped_utf8_name + "\"}")
                   .error == control::ParseError::None);
  require_error("{\"type\":\"createSender\",\"name\":\"" + escaped_utf8_name +
                    "\\u00e9\"}",
                control::ParseError::InvalidValue);
  require_error("{\"type\":\"createSender\",\"name\":\"\"}",
                control::ParseError::InvalidValue);
  require_error("{\"type\":\"createSender\",\"name\":\"" +
                    std::string(65, 'n') + "\"}",
                control::ParseError::InvalidValue);
  require_error("{\"type\":\"createSender\",\"name\":\"a\\u0000b\"}",
                control::ParseError::InvalidValue);
  require_error("{\"type\":\"createSender\",\"name\":\"a\\u007fb\"}",
                control::ParseError::InvalidValue);
}

SYNC_TEST(control_enforces_sender_id_alphabet_and_byte_boundaries) {
  const std::string id128(128, 'a');
  SYNC_REQUIRE(control::parse_message("{\"type\":\"getStats\",\"senderId\":\"" +
                                      id128 + "\"}")
                   .error == control::ParseError::None);
  require_error("{\"type\":\"getStats\",\"senderId\":\"\"}",
                control::ParseError::InvalidValue);
  require_error("{\"type\":\"getStats\",\"senderId\":\"bad.id\"}",
                control::ParseError::InvalidValue);
  require_error("{\"type\":\"closeSender\",\"senderId\":\"" +
                    std::string(129, 'a') + "\"}",
                control::ParseError::InvalidValue);
}

SYNC_TEST(control_rejects_unsupported_message_types) {
  require_error("{\"type\":\"deleteEverything\"}",
                control::ParseError::UnsupportedMessage);
}

SYNC_TEST(control_response_encoders_emit_exact_plain_json) {
  const std::array<noisefactor::sync::ProviderCapability, 1> providers{{{
      .id = "test",
      .direction = noisefactor::sync::ProviderDirection::Send,
      .available = true,
      .selected = true,
  }}};
  const control::SenderStatsPayload stats = {
      .accepted = 1,
      .dropped = 2,
      .rejected = 3,
      .failed = 4,
      .last_sequence = 5,
      .last_presentation_time_us = 6,
      .checksum = 7,
  };

  SYNC_REQUIRE(
      control::encode_welcome(1, noisefactor::sync::kProductVersion,
                              "instance-a", providers) ==
      "{\"type\":\"welcome\",\"protocolVersion\":1,\"version\":\"0.2.0\","
      "\"instanceId\":\"instance-a\",\"capabilities\":{\"send\":true,"
      "\"receive\":false,\"providers\":[{\"id\":\"test\",\"direction\":"
      "\"send\","
      "\"available\":true,\"selected\":true}]}}");
  SYNC_REQUIRE(
      control::encode_health(noisefactor::sync::kProductVersion, "instance-a",
                             providers) ==
      "{\"product\":\"Sync\",\"status\":\"ok\",\"version\":\"0.2.0\","
      "\"protocolVersions\":[1],\"instanceId\":\"instance-a\","
      "\"capabilities\":{\"send\":true,\"receive\":false,\"providers\":[{"
      "\"id\":\"test\",\"direction\":\"send\",\"available\":true,"
      "\"selected\":true}]}}");
  SYNC_REQUIRE(
      control::encode_status(noisefactor::sync::kProductVersion, "instance-a",
                             providers, 3) ==
      "{\"product\":\"Sync\",\"status\":\"ok\",\"version\":\"0.2.0\","
      "\"protocolVersions\":[1],\"instanceId\":\"instance-a\","
      "\"capabilities\":{\"send\":true,\"receive\":false,\"providers\":[{"
      "\"id\":\"test\",\"direction\":\"send\",\"available\":true,"
      "\"selected\":true}]},\"activeSenders\":3}");
  SYNC_REQUIRE(
      control::encode_sender_created("sender-1", "Camera", "/senders/sender-1",
                                     "ticket-1") ==
      "{\"type\":\"senderCreated\",\"id\":\"sender-1\",\"name\":\"Camera\","
      "\"path\":\"\\/senders\\/sender-1\",\"ticket\":\"ticket-1\"}");
  SYNC_REQUIRE(control::encode_sender_closed("sender-1") ==
               "{\"type\":\"senderClosed\",\"id\":\"sender-1\"}");
  SYNC_REQUIRE(
      control::encode_stats("sender-1", stats) ==
      "{\"type\":\"stats\",\"id\":\"sender-1\",\"accepted\":1,\"dropped\":2,"
      "\"rejected\":3,\"failed\":4,\"lastSequence\":5,"
      "\"lastPresentationTimeUs\":6,\"checksum\":7}");
  SYNC_REQUIRE(control::encode_error("bad_request", "No sender") ==
               "{\"type\":\"error\",\"code\":\"bad_request\",\"message\":\"No "
               "sender\"}");
}

SYNC_TEST(
    control_response_encoders_escape_strings_and_preserve_valid_non_ascii_utf8) {
  const auto encoded = control::encode_sender_created(
      "id/one", "Caf\xc3\xa9 \"camera\"/\\", "/senders/id/one",
      "slash/and\\backspace\b");
  SYNC_REQUIRE(encoded == "{\"type\":\"senderCreated\",\"id\":\"id\\/one\","
                          "\"name\":\"Caf\xc3\xa9 \\\"camera\\\"\\/\\\\\","
                          "\"path\":\"\\/senders\\/id\\/one\","
                          "\"ticket\":\"slash\\/and\\\\backspace\\b\"}");

  const auto round_trip = control::parse_message(
      "{\"type\":\"createSender\",\"name\":" +
      encoded.substr(encoded.find("\"name\":") + 7,
                     encoded.find(",\"path\"") -
                         (encoded.find("\"name\":") + 7)) +
      "}");
  SYNC_REQUIRE(round_trip.error == control::ParseError::None);
  SYNC_REQUIRE(round_trip.message->name == "Caf\xc3\xa9 \"camera\"/\\");
  SYNC_REQUIRE(
      control::encode_error("quote\"slash/", "line1\nline2\\\xc3\xa9") ==
      "{\"type\":\"error\",\"code\":\"quote\\\"slash\\/\","
      "\"message\":\"line1\\nline2\\\\\xc3\xa9\"}");
}

SYNC_TEST(
    control_response_encoders_replace_malformed_utf8_in_every_string_argument) {
  const std::array<noisefactor::sync::ProviderCapability, 1> providers{{{
      .id = raw_bytes({0x80}),
      .direction = noisefactor::sync::ProviderDirection::Send,
      .available = true,
      .selected = true,
  }}};
  const control::SenderStatsPayload stats{};

  SYNC_REQUIRE(
      control::encode_welcome(1, raw_bytes({0x80}), raw_bytes({0x80}),
                              providers) ==
      "{\"type\":\"welcome\",\"protocolVersion\":1,\"version\":\"\\ufffd\","
      "\"instanceId\":\"\\ufffd\",\"capabilities\":{\"send\":true,"
      "\"receive\":false,\"providers\":[{\"id\":\"\\ufffd\","
      "\"direction\":\"send\",\"available\":true,\"selected\":true}]}}");
  SYNC_REQUIRE(control::encode_sender_created(
                   raw_bytes({0xe2, 0x82}), raw_bytes({0xc0, 0xaf}),
                   raw_bytes({0x80}), raw_bytes({0xed, 0xa0, 0x80})) ==
               "{\"type\":\"senderCreated\",\"id\":\"\\ufffd\\ufffd\","
               "\"name\":\"\\ufffd\\ufffd\","
               "\"path\":\"\\ufffd\","
               "\"ticket\":\"\\ufffd\\ufffd\\ufffd\"}");
  SYNC_REQUIRE(control::encode_sender_closed(raw_bytes({0x80})) ==
               "{\"type\":\"senderClosed\",\"id\":\"\\ufffd\"}");
  SYNC_REQUIRE(
      control::encode_stats(raw_bytes({0xf4, 0x90, 0x80, 0x80}), stats) ==
      "{\"type\":\"stats\",\"id\":\"\\ufffd\\ufffd\\ufffd\\ufffd\","
      "\"accepted\":0,\"dropped\":0,\"rejected\":0,\"failed\":0,"
      "\"lastSequence\":0,\"lastPresentationTimeUs\":0,\"checksum\":0}");
  SYNC_REQUIRE(control::encode_error(raw_bytes({0x80}), raw_bytes({0xe2})) ==
               "{\"type\":\"error\",\"code\":\"\\ufffd\","
               "\"message\":\"\\ufffd\"}");
}

SYNC_TEST(
    control_capability_encoder_computes_legacy_truth_and_escapes_provider_ids) {
  const std::array<noisefactor::sync::ProviderCapability, 4> providers{{
      {.id = "send/\"one",
       .direction = noisefactor::sync::ProviderDirection::Send,
       .available = true,
       .selected = false},
      {.id = "send-two",
       .direction = noisefactor::sync::ProviderDirection::Send,
       .available = false,
       .selected = true},
      {.id = "receive-one",
       .direction = noisefactor::sync::ProviderDirection::Receive,
       .available = true,
       .selected = true},
      {.id = "receive-two",
       .direction = noisefactor::sync::ProviderDirection::Receive,
       .available = true,
       .selected = false},
  }};

  SYNC_REQUIRE(
      control::encode_capabilities(providers) ==
      "{\"send\":false,\"receive\":true,\"providers\":[{"
      "\"id\":\"send\\/\\\"one\",\"direction\":\"send\",\"available\":true,"
      "\"selected\":false},{\"id\":\"send-two\",\"direction\":\"send\","
      "\"available\":false,\"selected\":true},{\"id\":\"receive-one\","
      "\"direction\":\"receive\",\"available\":true,\"selected\":true},{"
      "\"id\":\"receive-two\",\"direction\":\"receive\",\"available\":true,"
      "\"selected\":false}]}");
}

SYNC_TEST(
    control_capability_encoder_rejects_out_of_contract_bounds_and_direction) {
  std::array<noisefactor::sync::ProviderCapability, 5> too_many{};
  for (std::size_t index = 0; index < too_many.size(); ++index) {
    too_many[index] = {
        .id = "provider-" + std::to_string(index),
        .direction = noisefactor::sync::ProviderDirection::Send,
        .available = true,
        .selected = true,
    };
  }
  bool rejected_too_many = false;
  try {
    (void)control::encode_capabilities(too_many);
  } catch (const std::invalid_argument &) {
    rejected_too_many = true;
  }
  SYNC_REQUIRE(rejected_too_many);

  const std::array<noisefactor::sync::ProviderCapability, 1> too_long{{{
      .id = std::string(noisefactor::sync::kMaximumProviderIdBytes + 1, 'x'),
      .direction = noisefactor::sync::ProviderDirection::Send,
      .available = true,
      .selected = true,
  }}};
  bool rejected_too_long = false;
  try {
    (void)control::encode_capabilities(too_long);
  } catch (const std::invalid_argument &) {
    rejected_too_long = true;
  }
  SYNC_REQUIRE(rejected_too_long);

  const std::array<noisefactor::sync::ProviderCapability, 1> invalid_direction{
      {{
          .id = "bad-direction",
          .direction = static_cast<noisefactor::sync::ProviderDirection>(99),
          .available = true,
          .selected = true,
      }}};
  bool rejected_direction = false;
  try {
    (void)control::encode_capabilities(invalid_direction);
  } catch (const std::invalid_argument &) {
    rejected_direction = true;
  }
  SYNC_REQUIRE(rejected_direction);
}
