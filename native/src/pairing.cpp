#include <sync/pairing.hpp>

#include <sync/label.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>

namespace noisefactor::sync::pairing {

bool valid_pairing_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > kMaximumPairingNameBytes)
    return false;
  for (std::size_t position = 0; position < name.size();) {
    const auto first = static_cast<unsigned char>(name[position]);
    if (first < 0x20 || first == 0x7f)
      return false;
    if (first < 0x80) {
      ++position;
      continue;
    }
    std::size_t count = 0;
    std::uint32_t code_point = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      count = 2;
      code_point = first & 0x1fU;
    } else if (first >= 0xe0 && first <= 0xef) {
      count = 3;
      code_point = first & 0x0fU;
    } else if (first >= 0xf0 && first <= 0xf4) {
      count = 4;
      code_point = first & 0x07U;
    } else
      return false;
    if (position + count > name.size())
      return false;
    for (std::size_t index = 1; index < count; ++index) {
      const auto continuation =
          static_cast<unsigned char>(name[position + index]);
      if ((continuation & 0xc0U) != 0x80U)
        return false;
      code_point = (code_point << 6U) | (continuation & 0x3fU);
    }
    if ((count == 2 && code_point < 0x80) ||
        (count == 3 && code_point < 0x800) ||
        (count == 4 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) ||
        code_point > 0x10ffff || (code_point >= 0x80 && code_point <= 0x9f) ||
        formatting_code_point(code_point))
      return false;
    position += count;
  }
  return true;
}

class Parser {
public:
  explicit Parser(std::string_view input) noexcept : input_(input) {}
  ParseResult parse() {
    if (input_.empty() || input_.size() > kMaximumPairingMessageBytes)
      return fail(ParseError::Malformed);
    space();
    if (!take('{'))
      return fail(ParseError::Malformed);
    space();
    while (!take('}')) {
      std::string key;
      if (!string(key, 32))
        return fail(ParseError::Malformed);
      unsigned bit = key == "type"               ? 1U
                     : key == "protocolVersions" ? 2U
                     : key == "name"             ? 4U
                                                 : 0U;
      if (bit == 0)
        return fail(ParseError::UnknownField);
      if ((seen_ & bit) != 0)
        return fail(ParseError::DuplicateField);
      seen_ |= bit;
      space();
      if (!take(':'))
        return fail(ParseError::Malformed);
      space();
      if (bit == 1) {
        std::string type;
        if (!string(type, 16))
          return fail(ParseError::InvalidType);
        if (type != "pair")
          return fail(ParseError::InvalidValue);
      } else if (bit == 2) {
        if (!versions())
          return fail(ParseError::InvalidValue);
      } else {
        std::string name;
        if (!string(name, kMaximumPairingNameBytes) || name.empty())
          return fail(ParseError::InvalidValue);
        if (!valid_pairing_name(name))
          return fail(ParseError::InvalidValue);
        std::memcpy(result_.request.name_.data(), name.data(), name.size());
        result_.request.name_length_ = name.size();
      }
      space();
      if (take('}'))
        break;
      if (!take(','))
        return fail(ParseError::Malformed);
      space();
      if (position_ < input_.size() && input_[position_] == '}') {
        return fail(ParseError::Malformed);
      }
    }
    space();
    if (position_ != input_.size())
      return fail(ParseError::Malformed);
    if (seen_ != 7U)
      return fail(ParseError::MissingField);
    if (!result_.request.supports(1))
      return fail(ParseError::UnsupportedVersion);
    result_.error = ParseError::None;
    return result_;
  }

private:
  ParseResult fail(ParseError error) noexcept {
    result_ = {};
    result_.error = error;
    return result_;
  }
  void space() noexcept {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\t' ||
            input_[position_] == '\r' || input_[position_] == '\n'))
      ++position_;
  }
  bool take(char value) noexcept {
    if (position_ >= input_.size() || input_[position_] != value)
      return false;
    ++position_;
    return true;
  }
  static int hex(char value) noexcept {
    if (value >= '0' && value <= '9')
      return value - '0';
    if (value >= 'a' && value <= 'f')
      return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
      return value - 'A' + 10;
    return -1;
  }
  bool quad(std::uint16_t &value) noexcept {
    if (input_.size() - position_ < 4)
      return false;
    value = 0;
    for (int i = 0; i < 4; ++i) {
      int h = hex(input_[position_++]);
      if (h < 0)
        return false;
      value = static_cast<std::uint16_t>((value << 4U) | h);
    }
    return true;
  }
  bool append_cp(std::string &out, std::uint32_t cp, std::size_t max) {
    if (cp >= 0x80 && cp <= 0x9f)
      return false;
    char b[4];
    std::size_t n = 0;
    if (cp <= 0x7f)
      b[n++] = static_cast<char>(cp);
    else if (cp <= 0x7ff) {
      b[n++] = static_cast<char>(0xc0 | (cp >> 6));
      b[n++] = static_cast<char>(0x80 | (cp & 63));
    } else if (cp <= 0xffff) {
      b[n++] = static_cast<char>(0xe0 | (cp >> 12));
      b[n++] = static_cast<char>(0x80 | ((cp >> 6) & 63));
      b[n++] = static_cast<char>(0x80 | (cp & 63));
    } else {
      b[n++] = static_cast<char>(0xf0 | (cp >> 18));
      b[n++] = static_cast<char>(0x80 | ((cp >> 12) & 63));
      b[n++] = static_cast<char>(0x80 | ((cp >> 6) & 63));
      b[n++] = static_cast<char>(0x80 | (cp & 63));
    }
    if (out.size() + n > max)
      return false;
    out.append(b, n);
    return true;
  }
  bool string(std::string &out, std::size_t max) {
    if (!take('"'))
      return false;
    while (position_ < input_.size()) {
      unsigned char c = static_cast<unsigned char>(input_[position_++]);
      if (c == '"')
        return true;
      if (c < 0x20)
        return false;
      if (c == '\\') {
        if (position_ >= input_.size())
          return false;
        char e = input_[position_++];
        if (e == 'u') {
          std::uint16_t a = 0;
          if (!quad(a))
            return false;
          std::uint32_t cp = a;
          if (a >= 0xd800 && a <= 0xdbff) {
            if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                input_[position_ + 1] != 'u')
              return false;
            position_ += 2;
            std::uint16_t b = 0;
            if (!quad(b) || b < 0xdc00 || b > 0xdfff)
              return false;
            cp = 0x10000U + ((a - 0xd800U) << 10U) + (b - 0xdc00U);
          } else if (a >= 0xdc00 && a <= 0xdfff)
            return false;
          if (!append_cp(out, cp, max))
            return false;
          continue;
        }
        char v = e == '"'    ? '"'
                 : e == '\\' ? '\\'
                 : e == '/'  ? '/'
                 : e == 'b'  ? '\b'
                 : e == 'f'  ? '\f'
                 : e == 'n'  ? '\n'
                 : e == 'r'  ? '\r'
                 : e == 't'  ? '\t'
                             : '\0';
        if (v == '\0')
          return false;
        if (out.size() == max)
          return false;
        out.push_back(v);
        continue;
      }
      if (c >= 0x80) {
        const std::size_t start = position_ - 1;
        std::size_t n = 0;
        std::uint32_t cp = 0;
        if (c >= 0xc2 && c <= 0xdf) {
          n = 2;
          cp = c & 0x1fU;
        } else if (c >= 0xe0 && c <= 0xef) {
          n = 3;
          cp = c & 0x0fU;
        } else if (c >= 0xf0 && c <= 0xf4) {
          n = 4;
          cp = c & 0x07U;
        } else
          return false;
        if (start + n > input_.size() || out.size() + n > max)
          return false;
        for (std::size_t i = 1; i < n; ++i) {
          unsigned char d = static_cast<unsigned char>(input_[position_++]);
          if ((d & 0xc0) != 0x80)
            return false;
          cp = (cp << 6U) | (d & 0x3fU);
        }
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
            (n == 4 && cp < 0x10000) || (cp >= 0xd800 && cp <= 0xdfff) ||
            cp > 0x10ffff || (cp >= 0x80 && cp <= 0x9f))
          return false;
        out.append(input_.data() + start, n);
        continue;
      }
      if (out.size() == max)
        return false;
      out.push_back(static_cast<char>(c));
    }
    return false;
  }
  bool versions() noexcept {
    if (!take('['))
      return false;
    space();
    if (take(']'))
      return false;
    while (true) {
      if (result_.request.version_count_ == kMaximumPairingVersions)
        return false;
      std::uint32_t v = 0;
      const char *start = input_.data() + position_;
      const auto [end, ec] =
          std::from_chars(start, input_.data() + input_.size(), v);
      if (ec != std::errc{} || end == start || v > 65535 ||
          (end - start > 1 && *start == '0'))
        return false;
      position_ = static_cast<std::size_t>(end - input_.data());
      for (std::size_t i = 0; i < result_.request.version_count_; ++i)
        if (result_.request.versions_[i] == v)
          return false;
      result_.request.versions_[result_.request.version_count_++] =
          static_cast<std::uint16_t>(v);
      space();
      if (take(']'))
        return true;
      if (!take(','))
        return false;
      space();
    }
  }
  std::string_view input_;
  std::size_t position_ = 0;
  unsigned seen_ = 0;
  ParseResult result_{};
};

bool PairRequest::supports(std::uint16_t version) const noexcept {
  return std::find(versions_.begin(), versions_.begin() + version_count_,
                   version) != versions_.begin() + version_count_;
}
ParseResult parse_request(std::string_view input) {
  return Parser(input).parse();
}
std::string encode_paired(std::uint16_t version, std::string_view token) {
  if (version != 1 || token.size() != kPairingTokenHexBytes)
    return {};
  for (const char byte : token) {
    if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
      return {};
  }
  constexpr std::string_view prefix =
      "{\"type\":\"paired\",\"protocolVersion\":1,\"token\":\"";
  constexpr std::string_view suffix = "\"}";
  std::string output;
  output.reserve(prefix.size() + token.size() + suffix.size());
  output.append(prefix);
  output.append(token);
  output.append(suffix);
  return output;
}
std::string encode_error(std::string_view code, std::string_view message) {
  if (code.empty() || code.size() > 32 || message.empty() ||
      message.size() > 128)
    return {};
  for (const char byte : code)
    if (!((byte >= 'a' && byte <= 'z') || byte == '_'))
      return {};
  for (const unsigned char byte : message)
    if (byte < 0x20 || byte > 0x7e || byte == '"' || byte == '\\')
      return {};
  return "{\"type\":\"error\",\"code\":\"" + std::string(code) +
         "\",\"message\":\"" + std::string(message) + "\"}";
}
bool PromptRequest::assign(std::uint64_t value,
                           const NormalizedOrigin &request_origin,
                           std::string_view request_name) noexcept {
  if (value == 0 || request_origin.empty() || !valid_pairing_name(request_name))
    return false;
  generation = value;
  origin = request_origin;
  std::memcpy(name_.data(), request_name.data(), request_name.size());
  name_length_ = request_name.size();
  return true;
}
PairingIssueResult
StorePairingAuthority::issue(const NormalizedOrigin &origin,
                             PairingCommitGate &gate) noexcept {
  std::lock_guard lock(store_mutex_);
  return store_.issue(origin, gate);
}
PairingAuthenticationResult
StorePairingAuthority::authenticate(const NormalizedOrigin &origin,
                                    std::string_view token) noexcept {
  std::lock_guard lock(store_mutex_);
  return store_.authenticate(origin, token);
}
PairingListResult
StorePairingAuthority::list(std::span<NormalizedOrigin> output) noexcept {
  std::lock_guard lock(store_mutex_);
  return store_.list(output);
}
PairingRevocationResult
StorePairingAuthority::revoke(const NormalizedOrigin &origin) noexcept {
  std::lock_guard lock(store_mutex_);
  return store_.revoke(origin);
}

} // namespace noisefactor::sync::pairing
