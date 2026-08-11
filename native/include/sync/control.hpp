#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sync/secure_memory.hpp>
#include <sync/server.hpp>

namespace noisefactor::sync::control {

enum class MessageType {
  Hello,
  CreateSender,
  GetStats,
  CloseSender,
};

struct ControlMessage {
  ControlMessage() = default;
  ~ControlMessage() noexcept;
  ControlMessage(const ControlMessage &) = delete;
  ControlMessage &operator=(const ControlMessage &) = delete;
  ControlMessage(ControlMessage &&other);
  ControlMessage &operator=(ControlMessage &&other);

  void clear_sensitive(CleanseObserver* observer = nullptr) noexcept;

  MessageType type = MessageType::Hello;
  std::string token;
  std::vector<std::uint16_t> protocol_versions;
  std::string name;
  std::string sender_id;
};

enum class ParseError {
  None,
  MalformedJson,
  DuplicateField,
  UnknownField,
  MissingField,
  InvalidType,
  InvalidValue,
  UnsupportedMessage,
};

struct ParseResult {
  ParseError error = ParseError::None;
  std::optional<ControlMessage> message;
};

struct SenderStatsPayload {
  std::uint64_t accepted = 0;
  std::uint64_t dropped = 0;
  std::uint64_t rejected = 0;
  std::uint64_t failed = 0;
  std::uint64_t last_sequence = 0;
  std::uint64_t last_presentation_time_us = 0;
  std::uint64_t checksum = 0;
};

ParseResult parse_message(std::string_view json,
                          CleanseObserver* observer = nullptr);
std::string encode_capabilities(std::span<const ProviderCapability> providers);
std::string encode_health(std::string_view product_version,
                          std::string_view instance_id,
                          std::span<const ProviderCapability> providers);
std::string encode_welcome(std::uint16_t protocol_version,
                           std::string_view product_version,
                           std::string_view instance_id,
                           std::span<const ProviderCapability> providers);
std::string encode_sender_created(std::string_view id, std::string_view name,
                                  std::string_view path,
                                  std::string_view ticket);
std::string encode_sender_closed(std::string_view id);
std::string encode_stats(std::string_view id,
                         const SenderStatsPayload &payload);
std::string encode_error(std::string_view code, std::string_view message);

} // namespace noisefactor::sync::control
