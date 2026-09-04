#include <sync/platform/linux_control_protocol.hpp>

#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <optional>

namespace noisefactor::sync::linux_control {
namespace {

enum class Field : std::uint8_t {
  Version,
  Command,
  Generation,
  Approved,
  Origin,
};

constexpr std::uint8_t bit(Field field) noexcept {
  return static_cast<std::uint8_t>(1U << static_cast<unsigned>(field));
}

template <std::size_t Size>
struct FixedString {
  std::array<char, Size> bytes{};
  std::size_t length = 0;
  [[nodiscard]] auto view() const noexcept -> std::string_view {
    return {bytes.data(), length};
  }
};

class RequestParser {
 public:
  explicit RequestParser(std::string_view input) noexcept : input_(input) {}

  [[nodiscard]] auto parse() noexcept -> LinuxControlDecodeResult {
    whitespace();
    if (!take('{')) return fail("malformed_json");
    whitespace();
    if (take('}')) return fail("missing_field");
    while (true) {
      FixedString<32> key;
      if (!string(key)) return fail("malformed_json");
      const auto field = identify(key.view());
      if (!field.has_value()) return fail("unknown_field");
      if ((seen_ & bit(*field)) != 0) return fail("duplicate_field");
      seen_ |= bit(*field);
      whitespace();
      if (!take(':')) return fail("malformed_json");
      whitespace();
      if (!value(*field)) return fail("invalid_value");
      whitespace();
      if (take('}')) break;
      if (!take(',')) return fail("malformed_json");
      whitespace();
      if (position_ >= input_.size() || input_[position_] == '}') {
        return fail("malformed_json");
      }
    }
    whitespace();
    if (position_ != input_.size()) return fail("trailing_bytes");
    return validate();
  }

 private:
  [[nodiscard]] static auto fail(std::string_view code) noexcept
      -> LinuxControlDecodeResult {
    return {.valid = false, .error_code = code};
  }

  void whitespace() noexcept {
    while (position_ < input_.size()) {
      const char value = input_[position_];
      if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
        break;
      }
      ++position_;
    }
  }

  [[nodiscard]] bool take(char expected) noexcept {
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  [[nodiscard]] static auto identify(std::string_view key) noexcept
      -> std::optional<Field> {
    if (key == "version") return Field::Version;
    if (key == "command") return Field::Command;
    if (key == "generation") return Field::Generation;
    if (key == "approved") return Field::Approved;
    if (key == "origin") return Field::Origin;
    return std::nullopt;
  }

  [[nodiscard]] static int hex(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  }

  template <std::size_t Size>
  [[nodiscard]] bool string(FixedString<Size>& output) noexcept {
    if (!take('"')) return false;
    while (position_ < input_.size()) {
      unsigned char value = static_cast<unsigned char>(input_[position_++]);
      if (value == '"') return true;
      if (value < 0x20U || value > 0x7eU) return false;
      if (value == '\\') {
        if (position_ >= input_.size()) return false;
        const char escape = input_[position_++];
        if (escape == 'u') {
          if (input_.size() - position_ < 4) return false;
          unsigned code_point = 0;
          for (unsigned index = 0; index < 4; ++index) {
            const int digit = hex(input_[position_++]);
            if (digit < 0) return false;
            code_point = (code_point << 4U) | static_cast<unsigned>(digit);
          }
          if (code_point < 0x20U || code_point > 0x7eU) return false;
          value = static_cast<unsigned char>(code_point);
        } else {
          switch (escape) {
            case '"': value = '"'; break;
            case '\\': value = '\\'; break;
            case '/': value = '/'; break;
            case 'b': value = '\b'; break;
            case 'f': value = '\f'; break;
            case 'n': value = '\n'; break;
            case 'r': value = '\r'; break;
            case 't': value = '\t'; break;
            default: return false;
          }
        }
      }
      if (output.length == output.bytes.size()) return false;
      output.bytes[output.length++] = static_cast<char>(value);
    }
    return false;
  }

  [[nodiscard]] bool unsigned_integer(std::uint64_t& output) noexcept {
    if (position_ >= input_.size()) return false;
    const char* begin = input_.data() + position_;
    const char* end = input_.data() + input_.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, output);
    if (error != std::errc{} || parsed_end == begin ||
        (parsed_end - begin > 1 && *begin == '0')) {
      return false;
    }
    position_ = static_cast<std::size_t>(parsed_end - input_.data());
    return true;
  }

  [[nodiscard]] bool boolean(bool& output) noexcept {
    const std::string_view rest = input_.substr(position_);
    if (rest.starts_with("true")) {
      output = true;
      position_ += 4;
      return true;
    }
    if (rest.starts_with("false")) {
      output = false;
      position_ += 5;
      return true;
    }
    return false;
  }

  [[nodiscard]] bool value(Field field) noexcept {
    switch (field) {
      case Field::Version:
        return unsigned_integer(version_);
      case Field::Command:
        return string(command_);
      case Field::Generation:
        return unsigned_integer(request_.generation);
      case Field::Approved:
        return boolean(request_.approved);
      case Field::Origin:
        return string(origin_);
    }
    return false;
  }

  [[nodiscard]] auto validate() noexcept -> LinuxControlDecodeResult {
    constexpr std::uint8_t base = bit(Field::Version) | bit(Field::Command);
    if ((seen_ & base) != base) return fail("missing_field");
    if (version_ != kLinuxControlProtocolVersion) {
      return fail("unsupported_version");
    }

    std::uint8_t required = base;
    if (command_.view() == "pair") {
      request_.command = LinuxControlCommand::Pair;
    } else if (command_.view() == "decision") {
      request_.command = LinuxControlCommand::Decision;
      required |= bit(Field::Generation) | bit(Field::Approved);
      if (request_.generation == 0) return fail("invalid_generation");
    } else if (command_.view() == "status") {
      request_.command = LinuxControlCommand::Status;
    } else if (command_.view() == "doctor") {
      request_.command = LinuxControlCommand::Doctor;
    } else if (command_.view() == "pairings") {
      request_.command = LinuxControlCommand::Pairings;
    } else if (command_.view() == "revoke") {
      request_.command = LinuxControlCommand::Revoke;
      required |= bit(Field::Origin);
      const auto normalized = normalize_origin(origin_.view());
      if (!normalized.ok()) return fail("invalid_origin");
      request_.origin = normalized.origin;
    } else {
      return fail("invalid_command");
    }
    if (seen_ != required) return fail("invalid_shape");
    return {.valid = true, .request = request_};
  }

  std::string_view input_;
  std::size_t position_ = 0;
  std::uint8_t seen_ = 0;
  std::uint64_t version_ = 0;
  FixedString<16> command_{};
  FixedString<kMaximumOriginInputBytes> origin_{};
  LinuxControlRequest request_{};
};

}  // namespace

auto decode_linux_control_request(std::string_view json) noexcept
    -> LinuxControlDecodeResult {
  if (json.empty()) return {.error_code = "empty_message"};
  if (json.size() > kMaximumLinuxControlMessageBytes) {
    return {.error_code = "message_too_large"};
  }
  return RequestParser(json).parse();
}

auto encode_linux_control_frame(std::string_view json)
    -> std::vector<std::byte> {
  if (json.empty() || json.size() > kMaximumLinuxControlMessageBytes ||
      json.size() > std::numeric_limits<std::uint32_t>::max()) {
    return {};
  }
  const auto size = static_cast<std::uint32_t>(json.size());
  std::vector<std::byte> output(json.size() + 4);
  output[0] = static_cast<std::byte>((size >> 24U) & 0xffU);
  output[1] = static_cast<std::byte>((size >> 16U) & 0xffU);
  output[2] = static_cast<std::byte>((size >> 8U) & 0xffU);
  output[3] = static_cast<std::byte>(size & 0xffU);
  std::memcpy(output.data() + 4, json.data(), json.size());
  return output;
}

auto decode_linux_control_frame(std::span<const std::byte> bytes) noexcept
    -> LinuxControlFrameDecodeResult {
  if (bytes.size() < 4) return {.error_code = "incomplete_frame"};
  const std::uint32_t size =
      (std::to_integer<std::uint32_t>(bytes[0]) << 24U) |
      (std::to_integer<std::uint32_t>(bytes[1]) << 16U) |
      (std::to_integer<std::uint32_t>(bytes[2]) << 8U) |
      std::to_integer<std::uint32_t>(bytes[3]);
  if (size == 0 || size > kMaximumLinuxControlMessageBytes) {
    return {.error_code = "invalid_length"};
  }
  const std::size_t total = static_cast<std::size_t>(size) + 4;
  if (bytes.size() < total) return {.error_code = "incomplete_frame"};
  if (bytes.size() > total) return {.error_code = "trailing_frame"};
  return {
      .valid = true,
      .json = {reinterpret_cast<const char*>(bytes.data() + 4), size},
  };
}

auto encode_linux_control_json_string(std::string_view value) -> std::string {
  constexpr char hex[] = "0123456789abcdef";
  std::string output;
  output.reserve(value.size() + 2);
  output.push_back('"');
  for (const unsigned char byte : value) {
    if (byte == '"' || byte == '\\') {
      output.push_back('\\');
      output.push_back(static_cast<char>(byte));
    } else if (byte < 0x20U) {
      output.append("\\u00");
      output.push_back(hex[(byte >> 4U) & 0x0fU]);
      output.push_back(hex[byte & 0x0fU]);
    } else {
      output.push_back(static_cast<char>(byte));
    }
  }
  output.push_back('"');
  return output;
}

}  // namespace noisefactor::sync::linux_control
