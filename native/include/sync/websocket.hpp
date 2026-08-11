#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sync/secure_memory.hpp>

namespace noisefactor::sync::websocket {

inline constexpr std::size_t kMaximumHttpUpgradeBytes = 16384;
inline constexpr std::size_t kMaximumReusablePayloadBytes = 8U * 1024U * 1024U;
inline constexpr std::string_view kSupportedWebSocketVersion = "13";

enum class HttpError {
  None,
  TooLarge,
  Malformed,
  WrongMethod,
  WrongVersion,
  MissingUpgrade,
  MissingConnectionUpgrade,
  MissingHost,
  MissingOrigin,
  MissingKey,
  UnsupportedWebSocketVersion,
  InvalidKey,
};

struct UpgradeRequest {
  std::string path;
  std::string host;
  std::string origin;
  std::string key;
  std::vector<std::string> subprotocols;
};

struct HttpParseResult {
  HttpError error = HttpError::None;
  std::optional<UpgradeRequest> request;
};

HttpParseResult parse_upgrade_request(std::string_view bytes);
std::string websocket_accept_key(std::string_view key);

enum class Opcode : std::uint8_t {
  Continuation = 0x0,
  Text = 0x1,
  Binary = 0x2,
  Close = 0x8,
  Ping = 0x9,
  Pong = 0xA,
};

struct Message {
  Opcode opcode = Opcode::Binary;
  std::vector<std::byte> payload;
};

enum class PayloadSensitivity { NonSensitive, Sensitive };

enum class DecodeError {
  None,
  Terminal,
  UnmaskedClientFrame,
  ReservedBits,
  UnsupportedOpcode,
  InvalidControlFrame,
  UnexpectedContinuation,
  FragmentAlreadyOpen,
  MessageTooLarge,
  InvalidLengthEncoding,
};

class ClientFrameDecoder {
 public:
  explicit ClientFrameDecoder(std::size_t max_message_bytes,
                              PayloadSensitivity sensitivity =
                                  PayloadSensitivity::NonSensitive,
                              CleanseObserver* cleanse_observer = nullptr,
                              std::size_t max_reusable_payload_bytes =
                                  kMaximumReusablePayloadBytes);
  ~ClientFrameDecoder() noexcept;

  DecodeError feed(std::span<const std::byte> bytes, std::vector<Message>& output);
  void recycle_payload(std::vector<std::byte>& payload) noexcept;

 private:
  enum class State {
    FirstHeaderByte,
    SecondHeaderByte,
    ExtendedLength,
    Mask,
    Payload,
  };

  DecodeError fail(DecodeError error);
  DecodeError finish_length();
  DecodeError finish_frame(std::vector<Message>& output);
  void prepare_fragment_storage(std::size_t required_capacity);
  void reset_frame();

  std::size_t max_message_bytes_;
  PayloadSensitivity sensitivity_ = PayloadSensitivity::NonSensitive;
  CleanseObserver* cleanse_observer_ = nullptr;
  std::size_t max_reusable_payload_bytes_ = 0;
  State state_ = State::FirstHeaderByte;
  bool terminal_ = false;
  bool final_ = false;
  bool control_ = false;
  Opcode opcode_ = Opcode::Continuation;
  std::uint8_t length_code_ = 0;
  std::uint64_t payload_length_ = 0;
  std::size_t payload_received_ = 0;
  std::array<std::byte, 8> extended_length_{};
  std::size_t extended_length_size_ = 0;
  std::size_t extended_length_received_ = 0;
  std::array<std::byte, 4> mask_{};
  std::size_t mask_received_ = 0;
  std::vector<std::byte> payload_;
  std::vector<std::byte> reusable_payload_;
  bool fragment_open_ = false;
  Opcode fragmented_opcode_ = Opcode::Continuation;
  std::vector<std::byte> fragment_payload_;
};

void cleanse_message_payloads(std::span<Message> messages,
                              CleanseObserver* observer = nullptr) noexcept;

class MessagePayloadGuard {
 public:
  MessagePayloadGuard(std::vector<Message>& messages,
                      PayloadSensitivity sensitivity,
                      CleanseObserver* observer = nullptr) noexcept
      : messages_(messages), sensitivity_(sensitivity), observer_(observer) {}
  ~MessagePayloadGuard() noexcept;
  MessagePayloadGuard(const MessagePayloadGuard&) = delete;
  MessagePayloadGuard& operator=(const MessagePayloadGuard&) = delete;

 private:
  std::vector<Message>& messages_;
  PayloadSensitivity sensitivity_;
  CleanseObserver* observer_;
};

std::vector<std::byte> encode_server_frame(Opcode opcode,
                                           std::span<const std::byte> payload);

}  // namespace noisefactor::sync::websocket
