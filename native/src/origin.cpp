#include <sync/origin.hpp>

#include <arpa/inet.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace noisefactor::sync {
namespace {

constexpr char ascii_lower(char value) noexcept {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool ascii_iequal(std::string_view left, std::string_view right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (ascii_lower(left[index]) != ascii_lower(right[index])) return false;
  }
  return true;
}

bool parse_port(std::string_view input, std::uint16_t& port) noexcept {
  if (input.empty() || (input.size() > 1 && input.front() == '0')) return false;
  unsigned value = 0;
  for (const char byte : input) {
    if (byte < '0' || byte > '9') return false;
    value = value * 10U + static_cast<unsigned>(byte - '0');
    if (value > 65535U) return false;
  }
  if (value == 0) return false;
  port = static_cast<std::uint16_t>(value);
  return true;
}

bool parse_ipv4(std::string_view host, std::array<unsigned, 4>& octets) noexcept {
  std::size_t offset = 0;
  for (std::size_t part = 0; part < octets.size(); ++part) {
    const std::size_t end = part == 3 ? host.size() : host.find('.', offset);
    if (end == std::string_view::npos || end == offset) return false;
    const auto component = host.substr(offset, end - offset);
    if (component.size() > 1 && component.front() == '0') return false;
    unsigned value = 0;
    for (const char byte : component) {
      if (byte < '0' || byte > '9') return false;
      value = value * 10U + static_cast<unsigned>(byte - '0');
      if (value > 255U) return false;
    }
    octets[part] = value;
    offset = end + 1;
  }
  return offset == host.size() + 1;
}

bool looks_numeric_host(std::string_view host) noexcept {
  for (const char byte : host) {
    if (!((byte >= '0' && byte <= '9') || byte == '.')) return false;
  }
  return true;
}

bool valid_dns_host(std::string_view host) noexcept {
  if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.') {
    return false;
  }
  std::size_t label_start = 0;
  while (label_start < host.size()) {
    const std::size_t dot = host.find('.', label_start);
    const std::size_t label_end = dot == std::string_view::npos ? host.size() : dot;
    const auto label = host.substr(label_start, label_end - label_start);
    if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') {
      return false;
    }
    for (const char byte : label) {
      const char lower = ascii_lower(byte);
      if (!((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') ||
            lower == '-')) {
        return false;
      }
    }
    if (dot == std::string_view::npos) break;
    label_start = dot + 1;
  }
  return true;
}

bool has_disallowed_alabel(std::string_view host) noexcept {
  std::size_t start = 0;
  while (start < host.size()) {
    const std::size_t dot = host.find('.', start);
    const std::size_t end = dot == std::string_view::npos ? host.size() : dot;
    const auto label = host.substr(start, end - start);
    if (label.size() >= 4 && ascii_iequal(label.substr(0, 4), "xn--")) return true;
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return false;
}

bool numeric_alias_label(std::string_view label) noexcept {
  if (label.empty()) return false;
  if (label.size() >= 2 && label[0] == '0' && ascii_lower(label[1]) == 'x') {
    if (label.size() == 2) return true;
    for (const char byte : label.substr(2)) {
      const char lower = ascii_lower(byte);
      if (!((lower >= '0' && lower <= '9') || (lower >= 'a' && lower <= 'f'))) return false;
    }
    return true;
  }
  for (const char byte : label) {
    if (byte < '0' || byte > '9') return false;
  }
  return true;
}

bool has_whatwg_numeric_alias(std::string_view host) noexcept {
  const std::size_t dot = host.rfind('.');
  const auto last = dot == std::string_view::npos ? host : host.substr(dot + 1);
  return numeric_alias_label(last);
}

bool is_localhost(std::string_view host) noexcept {
  if (ascii_iequal(host, "localhost")) return true;
  constexpr std::string_view suffix = ".localhost";
  return host.size() > suffix.size() &&
         ascii_iequal(host.substr(host.size() - suffix.size()), suffix);
}

bool format_ipv6_hex(const in6_addr& address,
                     std::array<char, INET6_ADDRSTRLEN>& output) noexcept {
  std::array<std::uint16_t, 8> groups{};
  for (std::size_t index = 0; index < groups.size(); ++index) {
    groups[index] = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(address.s6_addr[index * 2]) << 8U) |
        address.s6_addr[index * 2 + 1]);
  }
  std::size_t best_start = groups.size();
  std::size_t best_length = 0;
  for (std::size_t start = 0; start < groups.size();) {
    if (groups[start] != 0) {
      ++start;
      continue;
    }
    std::size_t end = start;
    while (end < groups.size() && groups[end] == 0) ++end;
    if (end - start >= 2 && end - start > best_length) {
      best_start = start;
      best_length = end - start;
    }
    start = end;
  }
  constexpr char hex[] = "0123456789abcdef";
  std::size_t length = 0;
  auto append = [&](char byte) noexcept {
    if (length + 1 >= output.size()) return false;
    output[length++] = byte;
    return true;
  };
  for (std::size_t index = 0; index < groups.size();) {
    if (index == best_start) {
      if (!append(':') || !append(':')) return false;
      index += best_length;
      continue;
    }
    if (length > 0 && output[length - 1] != ':' && !append(':')) return false;
    const std::uint16_t group = groups[index++];
    bool emitted = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
      const unsigned nibble = (group >> shift) & 0x0fU;
      if (nibble != 0 || emitted || shift == 0) {
        if (!append(hex[nibble])) return false;
        emitted = true;
      }
    }
  }
  output[length] = '\0';
  return true;
}

}  // namespace

NormalizeOriginResult normalize_origin(std::string_view input) noexcept {
  NormalizeOriginResult result{};
  const auto append = [&result](std::string_view value, bool lowercase = false) noexcept {
    if (result.origin.length_ + value.size() > result.origin.bytes_.size()) return false;
    for (const char byte : value) {
      result.origin.bytes_[result.origin.length_++] = lowercase ? ascii_lower(byte) : byte;
    }
    return true;
  };
  if (input.empty()) {
    result.error = OriginError::Empty;
    return result;
  }
  if (input.size() > kMaximumOriginInputBytes) {
    result.error = OriginError::TooLong;
    return result;
  }
  for (const unsigned char byte : input) {
    if (byte < 0x21U || byte > 0x7eU) return result;
  }
  if (input == "app://noisedeck") {
    append(input);
    result.error = OriginError::None;
    return result;
  }

  const std::size_t delimiter = input.find("://");
  if (delimiter == std::string_view::npos) return result;
  const auto scheme = input.substr(0, delimiter);
  const bool https = ascii_iequal(scheme, "https");
  const bool http = ascii_iequal(scheme, "http");
  if (!https && !http) {
    if (ascii_iequal(scheme, "app")) return result;
    result.error = OriginError::UnsupportedScheme;
    return result;
  }
  const auto authority = input.substr(delimiter + 3);
  if (authority.empty() || authority.find_first_of("/@?#\\%") != std::string_view::npos) {
    return result;
  }

  std::string_view host;
  std::string_view port_text;
  bool port_delimiter = false;
  bool ipv6 = false;
  std::array<char, INET6_ADDRSTRLEN> ipv6_text{};
  if (authority.front() == '[') {
    const std::size_t close = authority.find(']');
    if (close == std::string_view::npos) return result;
    const auto literal = authority.substr(1, close - 1);
    if (literal.empty() || literal.size() >= INET6_ADDRSTRLEN || literal.find('%') != std::string_view::npos) {
      return result;
    }
    std::array<char, INET6_ADDRSTRLEN> input_text{};
    for (std::size_t i = 0; i < literal.size(); ++i) input_text[i] = literal[i];
    in6_addr address{};
    if (::inet_pton(AF_INET6, input_text.data(), &address) != 1 ||
        !format_ipv6_hex(address, ipv6_text)) {
      return result;
    }
    host = std::string_view(ipv6_text.data());
    ipv6 = true;
    const auto remainder = authority.substr(close + 1);
    if (!remainder.empty()) {
      if (remainder.front() != ':') return result;
      port_delimiter = true;
      port_text = remainder.substr(1);
    }
  } else {
    const std::size_t colon = authority.find(':');
    if (colon != std::string_view::npos) {
      if (authority.find(':', colon + 1) != std::string_view::npos) return result;
      port_delimiter = true;
      host = authority.substr(0, colon);
      port_text = authority.substr(colon + 1);
    } else {
      host = authority;
    }
  }
  if (host.empty()) return result;

  std::array<unsigned, 4> ipv4_octets{};
  const bool ipv4 = !ipv6 && parse_ipv4(host, ipv4_octets);
  if (!ipv6 && !ipv4) {
    if (looks_numeric_host(host) || !valid_dns_host(host) || has_disallowed_alabel(host) ||
        has_whatwg_numeric_alias(host)) {
      return result;
    }
  }
  if (http) {
    bool loopback = ipv6 ? host == "::1" : (ipv4 ? ipv4_octets[0] == 127U : is_localhost(host));
    if (!loopback) {
      result.error = OriginError::InsecureRemote;
      return result;
    }
  }

  std::uint16_t port = 0;
  if (port_delimiter && port_text.empty()) return result;
  if (!port_text.empty() && !parse_port(port_text, port)) return result;
  if (!append(https ? "https://" : "http://")) return result;
  if (ipv6 && !append("[")) return result;
  if (!append(host, !ipv6)) return result;
  if (ipv6 && !append("]")) return result;
  const bool default_port = port != 0 && ((https && port == 443) || (http && port == 80));
  if (port != 0 && !default_port) {
    if (!append(":") || !append(port_text)) return result;
  }
  result.error = OriginError::None;
  return result;
}

}  // namespace noisefactor::sync
