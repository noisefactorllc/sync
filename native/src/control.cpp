#include <sync/control.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace noisefactor::sync::control {

ControlMessage::~ControlMessage() noexcept { clear_sensitive(); }

ControlMessage::ControlMessage(ControlMessage &&other)
    : type(other.type), protocol_versions(std::move(other.protocol_versions)),
      name(std::move(other.name)), sender_id(std::move(other.sender_id)) {
  token.reserve(256);
  token.assign(other.token);
  other.clear_sensitive();
}

ControlMessage &ControlMessage::operator=(ControlMessage &&other) {
  if (this == &other)
    return *this;
  clear_sensitive();
  type = other.type;
  token.reserve(256);
  token.assign(other.token);
  protocol_versions = std::move(other.protocol_versions);
  name = std::move(other.name);
  sender_id = std::move(other.sender_id);
  other.clear_sensitive();
  return *this;
}

void ControlMessage::clear_sensitive(CleanseObserver* observer) noexcept {
  secure_cleanse(std::as_writable_bytes(std::span(token.data(), token.size())),
                 observer);
  token.clear();
}

namespace {

enum class StringStatus {
  Ok,
  Malformed,
  TooLong,
};

enum class Field : std::uint8_t {
  Type = 0,
  Token = 1,
  ProtocolVersions = 2,
  Name = 3,
  SenderId = 4,
};

constexpr std::uint8_t field_bit(Field field) {
  return static_cast<std::uint8_t>(1U << static_cast<std::uint8_t>(field));
}

struct FixedTokenScratch {
  explicit FixedTokenScratch(CleanseObserver* value) noexcept
      : observer(value) {}
  ~FixedTokenScratch() noexcept {
    secure_cleanse(std::as_writable_bytes(std::span(bytes)), observer);
  }

  [[nodiscard]] std::string_view view() const noexcept {
    return {bytes.data(), length};
  }

  std::array<char, 256> bytes{};
  std::size_t length = 0;
  CleanseObserver* observer = nullptr;
};

class Parser {
public:
  explicit Parser(std::string_view input, CleanseObserver* observer)
      : input_(input), token_(observer) {}

  ParseResult parse() {
    skip_whitespace();
    if (!consume('{')) {
      return failure(ParseError::MalformedJson);
    }
    skip_whitespace();
    if (consume('}')) {
      skip_whitespace();
      return position_ == input_.size() ? failure(ParseError::MissingField)
                                        : failure(ParseError::MalformedJson);
    }

    while (true) {
      std::string key;
      const auto key_status = parse_string(key, 32);
      if (key_status == StringStatus::Malformed) {
        return failure(ParseError::MalformedJson);
      }
      if (key_status == StringStatus::TooLong) {
        return failure(ParseError::UnknownField);
      }
      const auto field = identify_field(key);
      if (!field.has_value()) {
        return failure(ParseError::UnknownField);
      }
      const auto bit = field_bit(*field);
      if ((seen_fields_ & bit) != 0) {
        return failure(ParseError::DuplicateField);
      }
      seen_fields_ |= bit;

      skip_whitespace();
      if (!consume(':')) {
        return failure(ParseError::MalformedJson);
      }
      skip_whitespace();
      const auto value_error = parse_field(*field);
      if (value_error != ParseError::None) {
        return failure(value_error);
      }

      skip_whitespace();
      if (consume('}')) {
        break;
      }
      if (!consume(',')) {
        return failure(ParseError::MalformedJson);
      }
      skip_whitespace();
      if (position_ >= input_.size() || input_[position_] == '}') {
        return failure(ParseError::MalformedJson);
      }
    }

    skip_whitespace();
    if (position_ != input_.size()) {
      return failure(ParseError::MalformedJson);
    }
    return validate_shape();
  }

private:
  static ParseResult failure(ParseError error) {
    return {.error = error, .message = std::nullopt};
  }

  void skip_whitespace() {
    while (position_ < input_.size()) {
      const char byte = input_[position_];
      if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') {
        break;
      }
      ++position_;
    }
  }

  bool consume(char expected) {
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  static int hex_value(char byte) {
    if (byte >= '0' && byte <= '9')
      return byte - '0';
    if (byte >= 'a' && byte <= 'f')
      return byte - 'a' + 10;
    if (byte >= 'A' && byte <= 'F')
      return byte - 'A' + 10;
    return -1;
  }

  bool read_hex_quad(std::uint16_t &value) {
    if (input_.size() - position_ < 4) {
      return false;
    }
    value = 0;
    for (int index = 0; index < 4; ++index) {
      const int digit = hex_value(input_[position_++]);
      if (digit < 0) {
        return false;
      }
      value = static_cast<std::uint16_t>((value << 4U) |
                                         static_cast<unsigned>(digit));
    }
    return true;
  }

  static void append_bounded(std::string &output, const char *bytes,
                             std::size_t count, std::size_t maximum,
                             bool &too_long) {
    if (count > maximum - std::min(output.size(), maximum)) {
      too_long = true;
      return;
    }
    if (!too_long) {
      output.append(bytes, count);
    }
  }

  static void append_bounded(FixedTokenScratch &output, const char *bytes,
                             std::size_t count, std::size_t maximum,
                             bool &too_long) {
    if (count > maximum - std::min(output.length, maximum)) {
      too_long = true;
      return;
    }
    if (!too_long) {
      std::memcpy(output.bytes.data() + output.length, bytes, count);
      output.length += count;
    }
  }

  template <typename Output>
  static void append_code_point(Output &output, std::uint32_t code_point,
                                std::size_t maximum, bool &too_long) {
    std::array<char, 4> encoded{};
    std::size_t count = 0;
    if (code_point <= 0x7f) {
      encoded[count++] = static_cast<char>(code_point);
    } else if (code_point <= 0x7ff) {
      encoded[count++] = static_cast<char>(0xc0U | (code_point >> 6U));
      encoded[count++] = static_cast<char>(0x80U | (code_point & 0x3fU));
    } else if (code_point <= 0xffff) {
      encoded[count++] = static_cast<char>(0xe0U | (code_point >> 12U));
      encoded[count++] =
          static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU));
      encoded[count++] = static_cast<char>(0x80U | (code_point & 0x3fU));
    } else {
      encoded[count++] = static_cast<char>(0xf0U | (code_point >> 18U));
      encoded[count++] =
          static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU));
      encoded[count++] =
          static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU));
      encoded[count++] = static_cast<char>(0x80U | (code_point & 0x3fU));
    }
    append_bounded(output, encoded.data(), count, maximum, too_long);
  }

  bool read_raw_utf8(std::uint32_t &code_point, std::size_t &count) {
    const auto first = static_cast<std::uint8_t>(input_[position_]);
    if (first < 0x80) {
      code_point = first;
      count = 1;
      return true;
    }
    if (first >= 0xc2 && first <= 0xdf) {
      code_point = first & 0x1fU;
      count = 2;
    } else if (first >= 0xe0 && first <= 0xef) {
      code_point = first & 0x0fU;
      count = 3;
    } else if (first >= 0xf0 && first <= 0xf4) {
      code_point = first & 0x07U;
      count = 4;
    } else {
      return false;
    }
    if (input_.size() - position_ < count) {
      return false;
    }
    for (std::size_t index = 1; index < count; ++index) {
      const auto continuation =
          static_cast<std::uint8_t>(input_[position_ + index]);
      if ((continuation & 0xc0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (continuation & 0x3fU);
    }
    if ((count == 3 && code_point < 0x800) ||
        (count == 4 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) ||
        code_point > 0x10ffff) {
      return false;
    }
    return true;
  }

  template <typename Output>
  StringStatus parse_string(Output &output, std::size_t maximum) {
    if (!consume('"')) {
      return StringStatus::Malformed;
    }
    bool too_long = false;
    while (position_ < input_.size()) {
      const auto byte = static_cast<std::uint8_t>(input_[position_]);
      if (byte == '"') {
        ++position_;
        return too_long ? StringStatus::TooLong : StringStatus::Ok;
      }
      if (byte < 0x20) {
        return StringStatus::Malformed;
      }
      if (byte == '\\') {
        ++position_;
        if (position_ >= input_.size())
          return StringStatus::Malformed;
        const char escape = input_[position_++];
        char escaped = 0;
        switch (escape) {
        case '"':
          escaped = '"';
          break;
        case '\\':
          escaped = '\\';
          break;
        case '/':
          escaped = '/';
          break;
        case 'b':
          escaped = '\b';
          break;
        case 'f':
          escaped = '\f';
          break;
        case 'n':
          escaped = '\n';
          break;
        case 'r':
          escaped = '\r';
          break;
        case 't':
          escaped = '\t';
          break;
        case 'u': {
          std::uint16_t first = 0;
          if (!read_hex_quad(first))
            return StringStatus::Malformed;
          std::uint32_t code_point = first;
          if (first >= 0xd800 && first <= 0xdbff) {
            if (input_.size() - position_ < 6 || input_[position_] != '\\' ||
                input_[position_ + 1] != 'u') {
              return StringStatus::Malformed;
            }
            position_ += 2;
            std::uint16_t second = 0;
            if (!read_hex_quad(second) || second < 0xdc00 || second > 0xdfff) {
              return StringStatus::Malformed;
            }
            code_point =
                0x10000U + ((first - 0xd800U) << 10U) + (second - 0xdc00U);
          } else if (first >= 0xdc00 && first <= 0xdfff) {
            return StringStatus::Malformed;
          }
          append_code_point(output, code_point, maximum, too_long);
          continue;
        }
        default:
          return StringStatus::Malformed;
        }
        append_bounded(output, &escaped, 1, maximum, too_long);
        continue;
      }

      std::uint32_t code_point = 0;
      std::size_t count = 0;
      if (!read_raw_utf8(code_point, count)) {
        return StringStatus::Malformed;
      }
      append_bounded(output, input_.data() + position_, count, maximum,
                     too_long);
      position_ += count;
    }
    return StringStatus::Malformed;
  }

  static std::optional<Field> identify_field(std::string_view key) {
    if (key == "type")
      return Field::Type;
    if (key == "token")
      return Field::Token;
    if (key == "protocolVersions")
      return Field::ProtocolVersions;
    if (key == "name")
      return Field::Name;
    if (key == "senderId")
      return Field::SenderId;
    return std::nullopt;
  }

  ParseError parse_bounded_string(std::string &output, std::size_t maximum) {
    if (position_ >= input_.size() || input_[position_] != '"') {
      return ParseError::InvalidType;
    }
    const auto status = parse_string(output, maximum);
    if (status == StringStatus::Malformed)
      return ParseError::MalformedJson;
    if (status == StringStatus::TooLong)
      return ParseError::InvalidValue;
    return ParseError::None;
  }

  ParseError parse_bounded_token() {
    if (position_ >= input_.size() || input_[position_] != '"') {
      return ParseError::InvalidType;
    }
    const auto status = parse_string(token_, token_.bytes.size());
    if (status == StringStatus::Malformed)
      return ParseError::MalformedJson;
    if (status == StringStatus::TooLong)
      return ParseError::InvalidValue;
    return ParseError::None;
  }

  ParseError parse_versions() {
    if (!consume('[')) {
      return ParseError::InvalidType;
    }
    skip_whitespace();
    if (consume(']')) {
      return ParseError::InvalidValue;
    }
    while (true) {
      if (position_ >= input_.size()) {
        return ParseError::MalformedJson;
      }
      if (input_[position_] == ']') {
        return ParseError::MalformedJson;
      }
      if (input_[position_] < '0' || input_[position_] > '9') {
        const char initial = input_[position_];
        if (initial == '"' || initial == '{' || initial == '[' ||
            initial == 't' || initial == 'f' || initial == 'n') {
          return ParseError::InvalidType;
        }
        return ParseError::InvalidValue;
      }
      const std::size_t first_digit = position_;
      std::uint64_t value = 0;
      bool out_of_range = false;
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        const auto digit = static_cast<std::uint64_t>(input_[position_] - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
          out_of_range = true;
        } else if (!out_of_range) {
          value = value * 10U + digit;
        }
        ++position_;
      }
      if (position_ - first_digit > 1 && input_[first_digit] == '0') {
        return ParseError::MalformedJson;
      }
      if (position_ < input_.size() &&
          (input_[position_] == '.' || input_[position_] == 'e' ||
           input_[position_] == 'E')) {
        return ParseError::InvalidValue;
      }
      if (out_of_range || value > std::numeric_limits<std::uint16_t>::max() ||
          message_.protocol_versions.size() == 16) {
        return ParseError::InvalidValue;
      }
      const auto version = static_cast<std::uint16_t>(value);
      for (const auto existing : message_.protocol_versions) {
        if (existing == version)
          return ParseError::InvalidValue;
      }
      message_.protocol_versions.push_back(version);

      skip_whitespace();
      if (consume(']'))
        return ParseError::None;
      if (!consume(','))
        return ParseError::MalformedJson;
      skip_whitespace();
    }
  }

  ParseError parse_field(Field field) {
    switch (field) {
    case Field::Type:
      return parse_bounded_string(type_, 32);
    case Field::Token:
      return parse_bounded_token();
    case Field::ProtocolVersions:
      return parse_versions();
    case Field::Name:
      return parse_bounded_string(message_.name, 64);
    case Field::SenderId:
      return parse_bounded_string(message_.sender_id, 128);
    }
    return ParseError::MalformedJson;
  }

  static bool has_control_code_point(std::string_view text) {
    for (std::size_t position = 0; position < text.size();) {
      const auto first = static_cast<std::uint8_t>(text[position]);
      std::uint32_t code_point = first;
      std::size_t count = 1;
      if ((first & 0xe0U) == 0xc0U) {
        code_point = first & 0x1fU;
        count = 2;
      } else if ((first & 0xf0U) == 0xe0U) {
        code_point = first & 0x0fU;
        count = 3;
      } else if ((first & 0xf8U) == 0xf0U) {
        code_point = first & 0x07U;
        count = 4;
      }
      for (std::size_t index = 1; index < count; ++index) {
        code_point =
            (code_point << 6U) |
            (static_cast<std::uint8_t>(text[position + index]) & 0x3fU);
      }
      if (code_point <= 0x1f || (code_point >= 0x7f && code_point <= 0x9f)) {
        return true;
      }
      position += count;
    }
    return false;
  }

  static bool valid_token(std::string_view token) {
    if (token.empty())
      return false;
    for (const auto byte : token) {
      const auto value = static_cast<unsigned char>(byte);
      if (value < 0x20 || value > 0x7e)
        return false;
    }
    return true;
  }

  static bool valid_sender_id(std::string_view sender_id) {
    if (sender_id.empty())
      return false;
    for (const unsigned char byte : sender_id) {
      if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '_' || byte == '-')) {
        return false;
      }
    }
    return true;
  }

  ParseResult validate_shape() {
    if ((seen_fields_ & field_bit(Field::Type)) == 0) {
      return failure(ParseError::MissingField);
    }

    std::uint8_t allowed = field_bit(Field::Type);
    std::uint8_t required = allowed;
    if (type_ == "hello") {
      message_.type = MessageType::Hello;
      allowed |= field_bit(Field::Token) | field_bit(Field::ProtocolVersions);
      required = allowed;
    } else if (type_ == "createSender") {
      message_.type = MessageType::CreateSender;
      allowed |= field_bit(Field::Name);
      required = allowed;
    } else if (type_ == "getStats") {
      message_.type = MessageType::GetStats;
      allowed |= field_bit(Field::SenderId);
      required = allowed;
    } else if (type_ == "closeSender") {
      message_.type = MessageType::CloseSender;
      allowed |= field_bit(Field::SenderId);
      required = allowed;
    } else {
      return failure(ParseError::UnsupportedMessage);
    }

    if ((seen_fields_ & static_cast<std::uint8_t>(~allowed)) != 0) {
      return failure(ParseError::UnknownField);
    }
    if ((seen_fields_ & required) != required) {
      return failure(ParseError::MissingField);
    }
    if (message_.type == MessageType::Hello && !valid_token(token_.view())) {
      return failure(ParseError::InvalidValue);
    }
    if (message_.type == MessageType::CreateSender &&
        (message_.name.empty() || has_control_code_point(message_.name))) {
      return failure(ParseError::InvalidValue);
    }
    if ((message_.type == MessageType::GetStats ||
         message_.type == MessageType::CloseSender) &&
        !valid_sender_id(message_.sender_id)) {
      return failure(ParseError::InvalidValue);
    }
    if (message_.type == MessageType::Hello) {
      message_.token.reserve(token_.bytes.size());
      message_.token.assign(token_.view());
    }
    return {.error = ParseError::None, .message = std::move(message_)};
  }

  std::string_view input_;
  std::size_t position_ = 0;
  std::uint8_t seen_fields_ = 0;
  std::string type_;
  FixedTokenScratch token_;
  ControlMessage message_;
};

std::size_t valid_utf8_sequence_bytes(std::string_view value,
                                      std::size_t position) {
  const auto first = static_cast<std::uint8_t>(value[position]);
  if (first <= 0x7f)
    return 1;
  if (first >= 0xc2 && first <= 0xdf) {
    if (value.size() - position >= 2 &&
        (static_cast<std::uint8_t>(value[position + 1]) & 0xc0U) == 0x80U) {
      return 2;
    }
    return 0;
  }
  if (first >= 0xe0 && first <= 0xef) {
    if (value.size() - position < 3)
      return 0;
    const auto second = static_cast<std::uint8_t>(value[position + 1]);
    const auto third = static_cast<std::uint8_t>(value[position + 2]);
    const bool valid_second = first == 0xe0   ? second >= 0xa0 && second <= 0xbf
                              : first == 0xed ? second >= 0x80 && second <= 0x9f
                                              : (second & 0xc0U) == 0x80U;
    return valid_second && (third & 0xc0U) == 0x80U ? 3 : 0;
  }
  if (first >= 0xf0 && first <= 0xf4) {
    if (value.size() - position < 4)
      return 0;
    const auto second = static_cast<std::uint8_t>(value[position + 1]);
    const auto third = static_cast<std::uint8_t>(value[position + 2]);
    const auto fourth = static_cast<std::uint8_t>(value[position + 3]);
    const bool valid_second = first == 0xf0   ? second >= 0x90 && second <= 0xbf
                              : first == 0xf4 ? second >= 0x80 && second <= 0x8f
                                              : (second & 0xc0U) == 0x80U;
    return valid_second && (third & 0xc0U) == 0x80U && (fourth & 0xc0U) == 0x80U
               ? 4
               : 0;
  }
  return 0;
}

void append_json_string(std::string &output, std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  output.push_back('"');
  for (std::size_t position = 0; position < value.size();) {
    const auto byte = static_cast<std::uint8_t>(value[position]);
    if (byte >= 0x80) {
      const auto sequence_bytes = valid_utf8_sequence_bytes(value, position);
      if (sequence_bytes == 0) {
        output.append("\\ufffd");
        ++position;
      } else {
        output.append(value.substr(position, sequence_bytes));
        position += sequence_bytes;
      }
      continue;
    }
    switch (byte) {
    case '"':
      output.append("\\\"");
      break;
    case '\\':
      output.append("\\\\");
      break;
    case '/':
      output.append("\\/");
      break;
    case '\b':
      output.append("\\b");
      break;
    case '\f':
      output.append("\\f");
      break;
    case '\n':
      output.append("\\n");
      break;
    case '\r':
      output.append("\\r");
      break;
    case '\t':
      output.append("\\t");
      break;
    default:
      if (byte < 0x20) {
        output.append("\\u00");
        output.push_back(kHex[byte >> 4U]);
        output.push_back(kHex[byte & 0x0fU]);
      } else {
        output.push_back(static_cast<char>(byte));
      }
      break;
    }
    ++position;
  }
  output.push_back('"');
}

void append_number(std::string &output, std::uint64_t value) {
  std::array<char, 20> buffer{};
  const auto [end, error] =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (error == std::errc{}) {
    output.append(buffer.data(), end);
  }
}

} // namespace

ParseResult parse_message(std::string_view json, CleanseObserver* observer) {
  return Parser(json, observer).parse();
}

std::string encode_welcome(std::uint16_t protocol_version,
                           std::string_view product_version,
                           std::string_view instance_id,
                           std::span<const ProviderCapability> providers) {
  std::string output = "{\"type\":\"welcome\",\"protocolVersion\":";
  append_number(output, protocol_version);
  output.append(",\"version\":");
  append_json_string(output, product_version);
  output.append(",\"instanceId\":");
  append_json_string(output, instance_id);
  output.append(",\"capabilities\":");
  output.append(encode_capabilities(providers));
  output.push_back('}');
  return output;
}

std::string encode_capabilities(std::span<const ProviderCapability> providers) {
  if (providers.size() > kMaximumProviderCapabilities) {
    throw std::invalid_argument(
        "Sync supports at most four provider capabilities");
  }

  bool can_send = false;
  bool can_receive = false;
  for (const ProviderCapability &provider : providers) {
    if (provider.id.empty() || provider.id.size() > kMaximumProviderIdBytes) {
      throw std::invalid_argument("Sync provider ID is outside its byte bound");
    }
    if (provider.direction != ProviderDirection::Send &&
        provider.direction != ProviderDirection::Receive) {
      throw std::invalid_argument("Sync provider direction is invalid");
    }
    if (provider.available && provider.selected) {
      if (provider.direction == ProviderDirection::Send) {
        can_send = true;
      } else {
        can_receive = true;
      }
    }
  }

  std::string output = "{\"send\":";
  output.append(can_send ? "true" : "false");
  output.append(",\"receive\":");
  output.append(can_receive ? "true" : "false");
  output.append(",\"providers\":[");
  for (std::size_t index = 0; index < providers.size(); ++index) {
    if (index != 0)
      output.push_back(',');
    const ProviderCapability &provider = providers[index];
    output.append("{\"id\":");
    append_json_string(output, provider.id);
    output.append(",\"direction\":\"");
    output.append(provider.direction == ProviderDirection::Send ? "send"
                                                                : "receive");
    output.append("\",\"available\":");
    output.append(provider.available ? "true" : "false");
    output.append(",\"selected\":");
    output.append(provider.selected ? "true" : "false");
    output.push_back('}');
  }
  output.append("]}");
  return output;
}

std::string encode_health(std::string_view product_version,
                          std::string_view instance_id,
                          std::span<const ProviderCapability> providers) {
  std::string output = "{\"product\":\"Sync\",\"status\":\"ok\",\"version\":";
  append_json_string(output, product_version);
  output.append(",\"protocolVersions\":[1],\"instanceId\":");
  append_json_string(output, instance_id);
  output.append(",\"capabilities\":");
  output.append(encode_capabilities(providers));
  output.push_back('}');
  return output;
}

std::string encode_sender_created(std::string_view id, std::string_view name,
                                  std::string_view path,
                                  std::string_view ticket) {
  std::string output = "{\"type\":\"senderCreated\",\"id\":";
  append_json_string(output, id);
  output.append(",\"name\":");
  append_json_string(output, name);
  output.append(",\"path\":");
  append_json_string(output, path);
  output.append(",\"ticket\":");
  append_json_string(output, ticket);
  output.push_back('}');
  return output;
}

std::string encode_sender_closed(std::string_view id) {
  std::string output = "{\"type\":\"senderClosed\",\"id\":";
  append_json_string(output, id);
  output.push_back('}');
  return output;
}

std::string encode_stats(std::string_view id,
                         const SenderStatsPayload &payload) {
  std::string output = "{\"type\":\"stats\",\"id\":";
  append_json_string(output, id);
  output.append(",\"accepted\":");
  append_number(output, payload.accepted);
  output.append(",\"dropped\":");
  append_number(output, payload.dropped);
  output.append(",\"rejected\":");
  append_number(output, payload.rejected);
  output.append(",\"failed\":");
  append_number(output, payload.failed);
  output.append(",\"lastSequence\":");
  append_number(output, payload.last_sequence);
  output.append(",\"lastPresentationTimeUs\":");
  append_number(output, payload.last_presentation_time_us);
  output.append(",\"checksum\":");
  append_number(output, payload.checksum);
  output.push_back('}');
  return output;
}

std::string encode_error(std::string_view code, std::string_view message) {
  std::string output = "{\"type\":\"error\",\"code\":";
  append_json_string(output, code);
  output.append(",\"message\":");
  append_json_string(output, message);
  output.push_back('}');
  return output;
}

} // namespace noisefactor::sync::control
