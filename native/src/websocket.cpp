#include <sync/websocket.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>

namespace noisefactor::sync::websocket {
namespace {

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

bool ascii_iequal(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto l = static_cast<unsigned char>(left[index]);
    const auto r = static_cast<unsigned char>(right[index]);
    if (std::tolower(l) != std::tolower(r)) {
      return false;
    }
  }
  return true;
}

bool contains_token(std::string_view value, std::string_view expected) {
  while (true) {
    const auto comma = value.find(',');
    const auto token = trim(value.substr(0, comma));
    if (!token.empty() && ascii_iequal(token, expected)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      return false;
    }
    value.remove_prefix(comma + 1);
  }
}

bool valid_header_name(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  for (const unsigned char byte : name) {
    const bool alphanumeric = (byte >= 'A' && byte <= 'Z') ||
                              (byte >= 'a' && byte <= 'z') ||
                              (byte >= '0' && byte <= '9');
    const bool token = alphanumeric || byte == '!' || byte == '#' || byte == '$' ||
                       byte == '%' || byte == '&' || byte == '\'' || byte == '*' ||
                       byte == '+' || byte == '-' || byte == '.' || byte == '^' ||
                       byte == '_' || byte == '`' || byte == '|' || byte == '~';
    if (!token) {
      return false;
    }
  }
  return true;
}

bool valid_header_value(std::string_view value) {
  for (const unsigned char byte : value) {
    if ((byte < 0x20 && byte != '\t') || byte == 0x7f) {
      return false;
    }
  }
  return true;
}

bool valid_base64_key(std::string_view key) {
  if (key.size() != 24 || key[22] != '=' || key[23] != '=') {
    return false;
  }
  for (std::size_t index = 0; index < 22; ++index) {
    const unsigned char byte = static_cast<unsigned char>(key[index]);
    if (!(std::isalnum(byte) || byte == '+' || byte == '/')) {
      return false;
    }
  }
  std::array<unsigned char, 18> decoded{};
  const int length = EVP_DecodeBlock(decoded.data(),
                                     reinterpret_cast<const unsigned char*>(key.data()),
                                     static_cast<int>(key.size()));
  if (length != 18) {
    return false;
  }
  std::array<unsigned char, 25> canonical{};
  const int canonical_length = EVP_EncodeBlock(canonical.data(), decoded.data(), 16);
  return canonical_length == 24 &&
         key == std::string_view(reinterpret_cast<const char*>(canonical.data()), 24);
}

HttpParseResult http_failure(HttpError error) {
  return {.error = error, .request = std::nullopt};
}

bool supported_opcode(std::uint8_t raw) {
  return raw == static_cast<std::uint8_t>(Opcode::Continuation) ||
         raw == static_cast<std::uint8_t>(Opcode::Text) ||
         raw == static_cast<std::uint8_t>(Opcode::Binary) ||
         raw == static_cast<std::uint8_t>(Opcode::Close) ||
         raw == static_cast<std::uint8_t>(Opcode::Ping) ||
         raw == static_cast<std::uint8_t>(Opcode::Pong);
}

bool control_opcode(Opcode opcode) {
  return opcode == Opcode::Close || opcode == Opcode::Ping || opcode == Opcode::Pong;
}

bool continuation_byte(std::uint8_t byte) {
  return (byte & 0xc0U) == 0x80U;
}

bool valid_utf8(std::span<const std::byte> value) {
  std::size_t position = 0;
  while (position < value.size()) {
    const auto first = std::to_integer<std::uint8_t>(value[position]);
    if (first <= 0x7f) {
      ++position;
      continue;
    }
    if (first >= 0xc2 && first <= 0xdf) {
      if (value.size() - position < 2 ||
          !continuation_byte(std::to_integer<std::uint8_t>(value[position + 1]))) {
        return false;
      }
      position += 2;
      continue;
    }
    if (first >= 0xe0 && first <= 0xef) {
      if (value.size() - position < 3) {
        return false;
      }
      const auto second = std::to_integer<std::uint8_t>(value[position + 1]);
      const auto third = std::to_integer<std::uint8_t>(value[position + 2]);
      const bool valid_second = first == 0xe0 ? second >= 0xa0 && second <= 0xbf
                                : first == 0xed ? second >= 0x80 && second <= 0x9f
                                                : continuation_byte(second);
      if (!valid_second || !continuation_byte(third)) {
        return false;
      }
      position += 3;
      continue;
    }
    if (first >= 0xf0 && first <= 0xf4) {
      if (value.size() - position < 4) {
        return false;
      }
      const auto second = std::to_integer<std::uint8_t>(value[position + 1]);
      const auto third = std::to_integer<std::uint8_t>(value[position + 2]);
      const auto fourth = std::to_integer<std::uint8_t>(value[position + 3]);
      const bool valid_second = first == 0xf0 ? second >= 0x90 && second <= 0xbf
                                : first == 0xf4 ? second >= 0x80 && second <= 0x8f
                                                : continuation_byte(second);
      if (!valid_second || !continuation_byte(third) || !continuation_byte(fourth)) {
        return false;
      }
      position += 4;
      continue;
    }
    return false;
  }
  return true;
}

bool valid_close_payload(std::span<const std::byte> payload) {
  if (payload.empty()) {
    return true;
  }
  if (payload.size() == 1) {
    return false;
  }
  const auto status = static_cast<std::uint16_t>(
      (std::to_integer<std::uint8_t>(payload[0]) << 8U) |
      std::to_integer<std::uint8_t>(payload[1]));
  const bool valid_status = (status >= 1000 && status <= 1003) ||
                            (status >= 1007 && status <= 1014) ||
                            (status >= 3000 && status <= 4999);
  return valid_status && valid_utf8(payload.subspan(2));
}

}  // namespace

HttpParseResult parse_upgrade_request(std::string_view bytes) {
  if (bytes.size() > kMaximumHttpUpgradeBytes) {
    return http_failure(HttpError::TooLarge);
  }
  if (bytes.size() < 4 || !bytes.ends_with("\r\n\r\n")) {
    return http_failure(HttpError::Malformed);
  }
  if (bytes.find("\r\n\r\n") != bytes.size() - 4) {
    return http_failure(HttpError::Malformed);
  }
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (bytes[index] == '\n' && (index == 0 || bytes[index - 1] != '\r')) {
      return http_failure(HttpError::Malformed);
    }
    if (bytes[index] == '\r' && (index + 1 >= bytes.size() || bytes[index + 1] != '\n')) {
      return http_failure(HttpError::Malformed);
    }
  }

  const auto first_end = bytes.find("\r\n");
  const auto request_line = bytes.substr(0, first_end);
  for (const unsigned char byte : request_line) {
    if (byte < 0x20 || byte == 0x7f) {
      return http_failure(HttpError::Malformed);
    }
  }
  const auto first_space = request_line.find(' ');
  const auto second_space = first_space == std::string_view::npos
                                ? std::string_view::npos
                                : request_line.find(' ', first_space + 1);
  if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
      request_line.find(' ', second_space + 1) != std::string_view::npos) {
    return http_failure(HttpError::Malformed);
  }
  if (request_line.substr(0, first_space) != "GET") {
    return http_failure(HttpError::WrongMethod);
  }
  const auto path = request_line.substr(first_space + 1, second_space - first_space - 1);
  if (path.empty()) {
    return http_failure(HttpError::Malformed);
  }
  if (request_line.substr(second_space + 1) != "HTTP/1.1") {
    return http_failure(HttpError::WrongVersion);
  }

  UpgradeRequest request;
  request.path.assign(path);
  std::string upgrade;
  std::string connection;
  std::string version;
  bool seen_host = false;
  bool seen_origin = false;
  bool seen_upgrade = false;
  bool seen_connection = false;
  bool seen_version = false;
  bool seen_key = false;
  bool seen_protocol = false;

  std::size_t position = first_end + 2;
  while (position < bytes.size() - 2) {
    const auto end = bytes.find("\r\n", position);
    if (end == std::string_view::npos) {
      return http_failure(HttpError::Malformed);
    }
    const auto line = bytes.substr(position, end - position);
    position = end + 2;
    if (line.empty()) {
      break;
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
      return http_failure(HttpError::Malformed);
    }
    const auto name = line.substr(0, colon);
    const auto value = trim(line.substr(colon + 1));
    if (!valid_header_name(name) || !valid_header_value(value)) {
      return http_failure(HttpError::Malformed);
    }

    auto set_once = [](bool& seen) {
      if (seen) {
        return false;
      }
      seen = true;
      return true;
    };
    if (ascii_iequal(name, "Host")) {
      if (!set_once(seen_host)) return http_failure(HttpError::Malformed);
      request.host.assign(value);
    } else if (ascii_iequal(name, "Origin")) {
      if (!set_once(seen_origin)) return http_failure(HttpError::Malformed);
      request.origin.assign(value);
    } else if (ascii_iequal(name, "Upgrade")) {
      if (!set_once(seen_upgrade)) return http_failure(HttpError::Malformed);
      upgrade.assign(value);
    } else if (ascii_iequal(name, "Connection")) {
      if (!set_once(seen_connection)) return http_failure(HttpError::Malformed);
      connection.assign(value);
    } else if (ascii_iequal(name, "Sec-WebSocket-Version")) {
      if (!set_once(seen_version)) return http_failure(HttpError::Malformed);
      version.assign(value);
    } else if (ascii_iequal(name, "Sec-WebSocket-Key")) {
      if (!set_once(seen_key)) return http_failure(HttpError::Malformed);
      request.key.assign(value);
    } else if (ascii_iequal(name, "Sec-WebSocket-Protocol")) {
      if (!set_once(seen_protocol)) return http_failure(HttpError::Malformed);
      if (value.empty()) return http_failure(HttpError::Malformed);
      std::string_view remaining = value;
      while (!remaining.empty()) {
        const auto comma = remaining.find(',');
        const auto item = trim(remaining.substr(0, comma));
        if (!valid_header_name(item)) {
          return http_failure(HttpError::Malformed);
        }
        request.subprotocols.emplace_back(item);
        if (comma == std::string_view::npos) break;
        if (comma + 1 == remaining.size()) return http_failure(HttpError::Malformed);
        remaining.remove_prefix(comma + 1);
      }
    }
  }

  if (!seen_upgrade || !contains_token(upgrade, "websocket")) {
    return http_failure(HttpError::MissingUpgrade);
  }
  if (!seen_connection || !contains_token(connection, "upgrade")) {
    return http_failure(HttpError::MissingConnectionUpgrade);
  }
  if (!seen_host || request.host.empty()) {
    return http_failure(HttpError::MissingHost);
  }
  if (!seen_origin || request.origin.empty()) {
    return http_failure(HttpError::MissingOrigin);
  }
  if (!seen_key || request.key.empty()) {
    return http_failure(HttpError::MissingKey);
  }
  if (!seen_version || version != kSupportedWebSocketVersion) {
    return http_failure(HttpError::UnsupportedWebSocketVersion);
  }
  if (!valid_base64_key(request.key)) {
    return http_failure(HttpError::InvalidKey);
  }
  return {.error = HttpError::None, .request = std::move(request)};
}

std::string websocket_accept_key(std::string_view key) {
  if (!valid_base64_key(key)) {
    return {};
  }
  std::string source;
  source.reserve(key.size() + kWebSocketGuid.size());
  source.append(key);
  source.append(kWebSocketGuid);

  std::array<unsigned char, 20> digest{};
  unsigned int digest_length = 0;
  if (EVP_Digest(source.data(), source.size(), digest.data(), &digest_length, EVP_sha1(), nullptr) !=
          1 ||
      digest_length != digest.size()) {
    return {};
  }
  std::array<unsigned char, 29> encoded{};
  const int encoded_length = EVP_EncodeBlock(encoded.data(), digest.data(), digest.size());
  if (encoded_length != 28) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(encoded.data()), encoded_length);
}

ClientFrameDecoder::ClientFrameDecoder(std::size_t max_message_bytes,
                                       PayloadSensitivity sensitivity,
                                       CleanseObserver* cleanse_observer,
                                       std::size_t max_reusable_payload_bytes)
    : max_message_bytes_(max_message_bytes),
      sensitivity_(sensitivity),
      cleanse_observer_(cleanse_observer),
      max_reusable_payload_bytes_(
          std::min(max_message_bytes, max_reusable_payload_bytes)) {
  if (max_message_bytes == 0) {
    throw std::invalid_argument("WebSocket message limit must be nonzero");
  }
}

std::size_t next_fragment_capacity(std::size_t current_capacity,
                                   std::size_t required_capacity,
                                   std::size_t maximum) noexcept {
  if (required_capacity >= maximum) return required_capacity;
  std::size_t target = std::min(
      std::max(current_capacity, kMinimumFragmentCapacityBytes), maximum);
  while (target < required_capacity) {
    target = target > maximum / 2 ? maximum : target * 2;
  }
  return target;
}

void ClientFrameDecoder::prepare_fragment_storage(
    std::size_t required_capacity) {
  if (fragment_payload_.capacity() >= required_capacity) return;
  if (fragment_payload_.empty() &&
      reusable_payload_.capacity() >= required_capacity) {
    fragment_payload_.swap(reusable_payload_);
    return;
  }
  std::vector<std::byte>().swap(reusable_payload_);
  fragment_payload_.reserve(next_fragment_capacity(
      fragment_payload_.capacity(), required_capacity, max_message_bytes_));
}

ClientFrameDecoder::~ClientFrameDecoder() noexcept {
  secure_cleanse(payload_, cleanse_observer_);
  secure_cleanse(reusable_payload_, cleanse_observer_);
  secure_cleanse(fragment_payload_, cleanse_observer_);
}

DecodeError ClientFrameDecoder::fail(DecodeError error) {
  terminal_ = true;
  secure_cleanse(payload_, cleanse_observer_);
  secure_cleanse(reusable_payload_, cleanse_observer_);
  secure_cleanse(fragment_payload_, cleanse_observer_);
  payload_.clear();
  reusable_payload_.clear();
  fragment_payload_.clear();
  return error;
}

DecodeError ClientFrameDecoder::finish_length() {
  if (length_code_ == 126 && payload_length_ < 126) {
    return fail(DecodeError::InvalidLengthEncoding);
  }
  if (length_code_ == 127 &&
      ((std::to_integer<std::uint8_t>(extended_length_[0]) & 0x80U) != 0 ||
       payload_length_ < 65536)) {
    return fail(DecodeError::InvalidLengthEncoding);
  }
  if (control_ && payload_length_ > 125) {
    return fail(DecodeError::InvalidControlFrame);
  }

  if (opcode_ == Opcode::Continuation) {
    if (!fragment_open_) {
      return fail(DecodeError::UnexpectedContinuation);
    }
    if (payload_length_ > max_message_bytes_ - fragment_payload_.size()) {
      return fail(DecodeError::MessageTooLarge);
    }
    if (sensitivity_ == PayloadSensitivity::Sensitive) {
      if (fragment_payload_.capacity() < max_message_bytes_) {
        if (cleanse_observer_ != nullptr) {
          cleanse_observer_->before_sensitive_fragment_reserve(
              fragment_payload_.size(), max_message_bytes_);
        }
        fragment_payload_.reserve(max_message_bytes_);
      }
    } else {
      prepare_fragment_storage(
          fragment_payload_.size() + static_cast<std::size_t>(payload_length_));
    }
  } else if (!control_) {
    if (fragment_open_) {
      return fail(DecodeError::FragmentAlreadyOpen);
    }
    if (payload_length_ > max_message_bytes_) {
      return fail(DecodeError::MessageTooLarge);
    }
    if (!final_) {
      if (sensitivity_ == PayloadSensitivity::Sensitive) {
        if (fragment_payload_.capacity() < max_message_bytes_) {
          if (cleanse_observer_ != nullptr) {
            cleanse_observer_->before_sensitive_fragment_reserve(
                fragment_payload_.size(), max_message_bytes_);
          }
          fragment_payload_.reserve(max_message_bytes_);
        }
      } else {
        prepare_fragment_storage(static_cast<std::size_t>(payload_length_));
      }
    } else {
      if (payload_.capacity() < payload_length_ &&
          reusable_payload_.capacity() >= payload_length_) {
        payload_.swap(reusable_payload_);
      }
      payload_.reserve(static_cast<std::size_t>(payload_length_));
    }
  } else {
    if (payload_.capacity() < payload_length_ &&
        reusable_payload_.capacity() >= payload_length_) {
      payload_.swap(reusable_payload_);
    }
    payload_.reserve(static_cast<std::size_t>(payload_length_));
  }
  state_ = State::Mask;
  return DecodeError::None;
}

DecodeError ClientFrameDecoder::finish_frame(std::vector<Message>& output) {
  if (opcode_ == Opcode::Close && !valid_close_payload(payload_)) {
    return fail(DecodeError::InvalidControlFrame);
  }
  // RFC 6455 section 5.6: a text message carries UTF-8. Validating the
  // assembled message keeps fragmented and unfragmented text on one rule; the
  // message limit already bounds how much is held before the check runs.
  if (control_) {
    output.reserve(output.size() + 1);
    output.push_back({.opcode = opcode_, .payload = std::move(payload_)});
  } else if (opcode_ == Opcode::Continuation) {
    if (final_) {
      if (fragmented_opcode_ == Opcode::Text && !valid_utf8(fragment_payload_)) {
        return fail(DecodeError::InvalidTextPayload);
      }
      output.reserve(output.size() + 1);
      output.push_back({.opcode = fragmented_opcode_, .payload = std::move(fragment_payload_)});
      fragment_payload_.clear();
      fragment_open_ = false;
      fragmented_opcode_ = Opcode::Continuation;
    }
  } else if (final_) {
    if (opcode_ == Opcode::Text && !valid_utf8(payload_)) {
      return fail(DecodeError::InvalidTextPayload);
    }
    output.reserve(output.size() + 1);
    output.push_back({.opcode = opcode_, .payload = std::move(payload_)});
  } else {
    fragment_open_ = true;
    fragmented_opcode_ = opcode_;
  }
  reset_frame();
  return DecodeError::None;
}

void ClientFrameDecoder::reset_frame() {
  state_ = State::FirstHeaderByte;
  final_ = false;
  control_ = false;
  opcode_ = Opcode::Continuation;
  length_code_ = 0;
  payload_length_ = 0;
  payload_received_ = 0;
  extended_length_size_ = 0;
  extended_length_received_ = 0;
  mask_received_ = 0;
  payload_.clear();
}

DecodeError ClientFrameDecoder::feed(std::span<const std::byte> bytes,
                                     std::vector<Message>& output) {
  if (terminal_) {
    return DecodeError::Terminal;
  }

  std::size_t position = 0;
  while (position < bytes.size()) {
    if (state_ == State::Payload) {
      const std::size_t remaining =
          static_cast<std::size_t>(payload_length_) - payload_received_;
      const std::size_t count = std::min(remaining, bytes.size() - position);
      std::vector<std::byte>& destination =
          !control_ && (!final_ || opcode_ == Opcode::Continuation)
              ? fragment_payload_
              : payload_;
      const std::size_t destination_offset = destination.size();
      destination.resize(destination_offset + count);
      const std::byte* const source = bytes.data() + position;
      std::byte* const target = destination.data() + destination_offset;
      const std::size_t phase = payload_received_ & 3U;
      // The mask is copied into a local, already rotated to this frame's phase.
      // Reading it straight from the member forces the compiler to assume every
      // store through `target` may alias `mask_`, so it reloads the mask byte
      // after each store and neither vectorizes nor pipelines the unmask.
      const std::array<std::byte, 4> mask = {
          mask_[phase], mask_[(phase + 1) & 3U], mask_[(phase + 2) & 3U],
          mask_[(phase + 3) & 3U]};
      std::size_t index = 0;
      for (; index + 4 <= count; index += 4) {
        target[index] = source[index] ^ mask[0];
        target[index + 1] = source[index + 1] ^ mask[1];
        target[index + 2] = source[index + 2] ^ mask[2];
        target[index + 3] = source[index + 3] ^ mask[3];
      }
      for (; index < count; ++index) {
        target[index] = source[index] ^ mask[index & 3U];
      }
      position += count;
      payload_received_ += count;
      if (payload_received_ == payload_length_) {
        if (const auto error = finish_frame(output); error != DecodeError::None) {
          return error;
        }
      }
      continue;
    }

    const auto byte = bytes[position++];
    const auto raw = std::to_integer<std::uint8_t>(byte);
    switch (state_) {
      case State::FirstHeaderByte: {
        final_ = (raw & 0x80U) != 0;
        if ((raw & 0x70U) != 0) {
          return fail(DecodeError::ReservedBits);
        }
        const auto raw_opcode = static_cast<std::uint8_t>(raw & 0x0fU);
        if (!supported_opcode(raw_opcode)) {
          return fail(DecodeError::UnsupportedOpcode);
        }
        opcode_ = static_cast<Opcode>(raw_opcode);
        control_ = control_opcode(opcode_);
        if (control_ && !final_) {
          return fail(DecodeError::InvalidControlFrame);
        }
        state_ = State::SecondHeaderByte;
        break;
      }
      case State::SecondHeaderByte:
        if ((raw & 0x80U) == 0) {
          return fail(DecodeError::UnmaskedClientFrame);
        }
        length_code_ = static_cast<std::uint8_t>(raw & 0x7fU);
        if (control_ && length_code_ > 125) {
          return fail(DecodeError::InvalidControlFrame);
        }
        if (length_code_ <= 125) {
          payload_length_ = length_code_;
          if (const auto error = finish_length(); error != DecodeError::None) {
            return error;
          }
        } else {
          extended_length_size_ = length_code_ == 126 ? 2 : 8;
          extended_length_received_ = 0;
          payload_length_ = 0;
          state_ = State::ExtendedLength;
        }
        break;
      case State::ExtendedLength:
        extended_length_[extended_length_received_++] = byte;
        payload_length_ = (payload_length_ << 8U) | raw;
        if (extended_length_received_ == extended_length_size_) {
          if (const auto error = finish_length(); error != DecodeError::None) {
            return error;
          }
        }
        break;
      case State::Mask:
        mask_[mask_received_++] = byte;
        if (mask_received_ == mask_.size()) {
          if (payload_length_ == 0) {
            if (const auto error = finish_frame(output); error != DecodeError::None) {
              return error;
            }
          } else {
            state_ = State::Payload;
          }
        }
        break;
      case State::Payload:
        break;
    }
  }
  return DecodeError::None;
}

void ClientFrameDecoder::recycle_payload(std::vector<std::byte>& payload) noexcept {
  if (sensitivity_ == PayloadSensitivity::Sensitive ||
      payload.capacity() > max_reusable_payload_bytes_) {
    return;
  }
  payload.clear();
  if (payload.capacity() > reusable_payload_.capacity()) {
    reusable_payload_.swap(payload);
  }
}

void cleanse_message_payloads(std::span<Message> messages,
                              CleanseObserver* observer) noexcept {
  for (Message& message : messages) {
    secure_cleanse(message.payload, observer);
    message.payload.clear();
  }
}

MessagePayloadGuard::~MessagePayloadGuard() noexcept {
  if (sensitivity_ == PayloadSensitivity::Sensitive)
    cleanse_message_payloads(messages_, observer_);
}

std::vector<std::byte> encode_server_frame(Opcode opcode,
                                           std::span<const std::byte> payload) {
  const auto raw_opcode = static_cast<std::uint8_t>(opcode);
  if (!supported_opcode(raw_opcode)) {
    return {};
  }
  if (control_opcode(opcode) && payload.size() > 125) {
    return {};
  }
  if (opcode == Opcode::Close && !valid_close_payload(payload)) {
    return {};
  }
  if (payload.size() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return {};
  }

  const std::size_t header_size = payload.size() <= 125 ? 2 : payload.size() <= 65535 ? 4 : 10;
  std::vector<std::byte> result;
  result.reserve(header_size + payload.size());
  result.push_back(static_cast<std::byte>(0x80U | raw_opcode));
  if (payload.size() <= 125) {
    result.push_back(static_cast<std::byte>(payload.size()));
  } else if (payload.size() <= 65535) {
    result.push_back(std::byte{126});
    result.push_back(static_cast<std::byte>((payload.size() >> 8U) & 0xffU));
    result.push_back(static_cast<std::byte>(payload.size() & 0xffU));
  } else {
    result.push_back(std::byte{127});
    const auto length = static_cast<std::uint64_t>(payload.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
      result.push_back(static_cast<std::byte>((length >> shift) & 0xffU));
    }
  }
  result.insert(result.end(), payload.begin(), payload.end());
  return result;
}

}  // namespace noisefactor::sync::websocket
