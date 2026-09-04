#include "syncctl_cli.hpp"

#include <sync/pairing.hpp>
#include <sync/platform/linux_control_protocol.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace noisefactor::sync::syncctl {
namespace {

bool valid_user(std::string_view value) noexcept {
  if (value.empty() || value.size() > 32) return false;
  const auto first = static_cast<unsigned char>(value.front());
  if (!((first >= 'a' && first <= 'z') || first == '_')) return false;
  for (std::size_t index = 1; index < value.size(); ++index) {
    const auto byte = static_cast<unsigned char>(value[index]);
    if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
        byte == '_' || byte == '-' ||
        (byte == '$' && index + 1 == value.size())) {
      continue;
    }
    return false;
  }
  return true;
}

class PromptParser {
 public:
  explicit PromptParser(std::string_view input) : input_(input) {}

  bool parse(Prompt& output) {
    space();
    if (!take('{')) return false;
    space();
    while (!take('}')) {
      std::string key;
      if (!string(key, 32)) return false;
      unsigned bit = key == "version"     ? 1U
                     : key == "type"      ? 2U
                     : key == "generation" ? 4U
                     : key == "origin"    ? 8U
                     : key == "name"      ? 16U
                     : key == "deadlineMs" ? 32U
                                             : 0U;
      if (bit == 0 || (seen_ & bit) != 0) return false;
      seen_ |= bit;
      space();
      if (!take(':')) return false;
      space();
      if (bit == 1 && !number(version_)) return false;
      if (bit == 2 && !string(type_, 16)) return false;
      if (bit == 4 && !number(generation_)) return false;
      if (bit == 8 && !string(origin_, kMaximumOriginInputBytes)) return false;
      if (bit == 16 && !string(name_, pairing::kMaximumPairingNameBytes)) {
        return false;
      }
      if (bit == 32 && !number(deadline_)) return false;
      space();
      if (take('}')) break;
      if (!take(',')) return false;
      space();
    }
    space();
    if (position_ != input_.size() || seen_ != 63U || version_ != 1 ||
        type_ != "prompt" || generation_ == 0 || deadline_ != 30000 ||
        name_.empty()) {
      return false;
    }
    const auto normalized = normalize_origin(origin_);
    if (!normalized.ok() || normalized.origin.view() != origin_) return false;
    output.generation = generation_;
    output.origin = origin_;
    output.name = name_;
    return true;
  }

 private:
  void space() noexcept {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\t' ||
            input_[position_] == '\r' || input_[position_] == '\n')) {
      ++position_;
    }
  }

  bool take(char expected) noexcept {
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  static int hex(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
  }

  bool string(std::string& output, std::size_t maximum) {
    if (!take('"')) return false;
    output.clear();
    while (position_ < input_.size()) {
      unsigned char byte = static_cast<unsigned char>(input_[position_++]);
      if (byte == '"') return true;
      if (byte < 0x20U) return false;
      if (byte == '\\') {
        if (position_ >= input_.size()) return false;
        const char escaped = input_[position_++];
        switch (escaped) {
          case '"': byte = '"'; break;
          case '\\': byte = '\\'; break;
          case '/': byte = '/'; break;
          case 'b': byte = '\b'; break;
          case 'f': byte = '\f'; break;
          case 'n': byte = '\n'; break;
          case 'r': byte = '\r'; break;
          case 't': byte = '\t'; break;
          case 'u': {
            if (input_.size() - position_ < 4) return false;
            unsigned code = 0;
            for (unsigned index = 0; index < 4; ++index) {
              const int digit = hex(input_[position_++]);
              if (digit < 0) return false;
              code = (code << 4U) | static_cast<unsigned>(digit);
            }
            if (code > 0x7fU) return false;
            byte = static_cast<unsigned char>(code);
            break;
          }
          default: return false;
        }
      }
      if (output.size() == maximum) return false;
      output.push_back(static_cast<char>(byte));
    }
    return false;
  }

  bool number(std::uint64_t& output) noexcept {
    if (position_ >= input_.size()) return false;
    const char* begin = input_.data() + position_;
    const char* end = input_.data() + input_.size();
    const auto [parsed, error] = std::from_chars(begin, end, output);
    if (error != std::errc{} || parsed == begin ||
        (parsed - begin > 1 && *begin == '0')) {
      return false;
    }
    position_ = static_cast<std::size_t>(parsed - input_.data());
    return true;
  }

  std::string_view input_;
  std::size_t position_ = 0;
  unsigned seen_ = 0;
  std::uint64_t version_ = 0;
  std::uint64_t generation_ = 0;
  std::uint64_t deadline_ = 0;
  std::string type_;
  std::string origin_;
  std::string name_;
};

bool write_all(int descriptor, std::span<const std::byte> bytes) noexcept {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto written = ::send(descriptor, bytes.data() + offset,
                                bytes.size() - offset, MSG_NOSIGNAL);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

bool read_all(int descriptor, std::span<std::byte> bytes) noexcept {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto received =
        ::recv(descriptor, bytes.data() + offset, bytes.size() - offset, 0);
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) return false;
    offset += static_cast<std::size_t>(received);
  }
  return true;
}

bool receive_frame(int descriptor, std::string& output) {
  std::array<std::byte, 4> prefix{};
  if (!read_all(descriptor, prefix)) return false;
  const std::uint32_t size =
      (std::to_integer<std::uint32_t>(prefix[0]) << 24U) |
      (std::to_integer<std::uint32_t>(prefix[1]) << 16U) |
      (std::to_integer<std::uint32_t>(prefix[2]) << 8U) |
      std::to_integer<std::uint32_t>(prefix[3]);
  if (size == 0 || size > linux_control::kMaximumLinuxControlMessageBytes) {
    return false;
  }
  std::vector<std::byte> payload(size);
  if (!read_all(descriptor, payload)) return false;
  output.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
  return true;
}

bool send_request(int descriptor, std::string_view json) {
  return write_all(descriptor, linux_control::encode_linux_control_frame(json));
}

int connect_control(std::ostream& error) {
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime == nullptr || runtime[0] != '/') {
    error << "syncctl: XDG_RUNTIME_DIR must be a nonempty absolute path\n";
    return -1;
  }
  const std::string path = std::string(runtime) +
                           "/noisedeck-sync/control.sock";
  if (path.size() >= sizeof(sockaddr_un{}.sun_path)) {
    error << "syncctl: control socket path is too long\n";
    return -1;
  }
  struct stat before {};
  if (::lstat(path.c_str(), &before) != 0 || !S_ISSOCK(before.st_mode) ||
      before.st_uid != ::geteuid() || (before.st_mode & 0077) != 0) {
    error << "syncctl: control socket is absent or not owner-only\n";
    return -1;
  }
  const int descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (descriptor < 0) {
    error << "syncctl: could not create the control connection\n";
    return -1;
  }
  timeval timeout{.tv_sec = 35, .tv_usec = 0};
  (void)::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
  (void)::setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout));
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::copy(path.begin(), path.end(), address.sun_path);
  if (::connect(descriptor, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) != 0) {
    error << "syncctl: could not connect to syncd\n";
    ::close(descriptor);
    return -1;
  }
  ucred peer{};
  socklen_t peer_length = sizeof(peer);
  struct stat after {};
  if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &peer, &peer_length) !=
          0 ||
      peer_length != sizeof(peer) || peer.uid != ::geteuid() ||
      ::lstat(path.c_str(), &after) != 0 || before.st_dev != after.st_dev ||
      before.st_ino != after.st_ino || before.st_uid != after.st_uid ||
      before.st_mode != after.st_mode) {
    error << "syncctl: control socket identity changed or peer is not owned by this user\n";
    ::close(descriptor);
    return -1;
  }
  return descriptor;
}

bool response_is_error(std::string_view response) noexcept {
  return response.find("\"type\":\"error\"") != std::string_view::npos;
}

}  // namespace

auto parse(std::span<const std::string_view> arguments) -> ParseResult {
  ParseResult result;
  if (arguments.empty()) return result;
  const std::string_view command = arguments[0];
  if (command == "pair") {
    if (arguments.size() != 1) return result;
    result.options.command = Command::Pair;
  } else if (command == "status" || command == "pairings" ||
             command == "doctor") {
    if (arguments.size() > 2 ||
        (arguments.size() == 2 && arguments[1] != "--json")) {
      return result;
    }
    result.options.command = command == "status"     ? Command::Status
                             : command == "pairings" ? Command::Pairings
                                                      : Command::Doctor;
    result.options.json = arguments.size() == 2;
  } else if (command == "revoke") {
    if (arguments.size() < 2 || arguments.size() > 3 ||
        (arguments.size() == 3 && arguments[2] != "--json")) {
      return result;
    }
    const auto normalized = normalize_origin(arguments[1]);
    if (!normalized.ok()) return result;
    result.options.command = Command::Revoke;
    result.options.origin = normalized.origin;
    result.options.json = arguments.size() == 3;
  } else if (command == "camera") {
    if (arguments.size() != 4 || arguments[1] != "setup" ||
        arguments[2] != "--user" || !valid_user(arguments[3])) {
      return result;
    }
    result.options.command = Command::CameraSetup;
    result.options.user.assign(arguments[3]);
  } else {
    return result;
  }
  result.valid = true;
  return result;
}

void print_usage(std::ostream& error) {
  error << "usage: syncctl pair\n"
           "       syncctl status [--json]\n"
           "       syncctl pairings [--json]\n"
           "       syncctl revoke <origin> [--json]\n"
           "       syncctl doctor [--json]\n"
           "       syncctl camera setup --user <name>\n";
}

auto escape_terminal_text(std::string_view input) -> std::string {
  constexpr char hex[] = "0123456789abcdef";
  std::string output;
  output.reserve(input.size());
  for (const unsigned char byte : input) {
    if (byte == '\n') output += "\\n";
    else if (byte == '\r') output += "\\r";
    else if (byte == '\t') output += "\\t";
    else if (byte == '"') output += "\\\"";
    else if (byte == '\\') output += "\\\\";
    else if (byte < 0x20U || byte == 0x7fU) {
      output += "\\x";
      output.push_back(hex[(byte >> 4U) & 0x0fU]);
      output.push_back(hex[byte & 0x0fU]);
    } else {
      output.push_back(static_cast<char>(byte));
    }
  }
  return output;
}

auto render_pair_prompt(std::string_view json, std::ostream& output,
                        Prompt& prompt) -> bool {
  Prompt parsed;
  if (!PromptParser(json).parse(parsed)) return false;
  output << "Origin: " << escape_terminal_text(parsed.origin) << '\n'
         << "Name: " << escape_terminal_text(parsed.name) << '\n'
         << "Allow this browser to pair? [y/N] ";
  if (!output) return false;
  prompt = std::move(parsed);
  return true;
}

auto read_approval(std::istream& input) -> bool {
  std::string answer;
  return static_cast<bool>(std::getline(input, answer)) &&
         (answer == "y" || answer == "Y");
}

auto execute(const Options& options, std::istream& input,
             std::ostream& output, std::ostream& error) -> int {
  if (options.command == Command::CameraSetup) {
    error << "syncctl: command_not_built\n";
    return kFailureExit;
  }
  const int descriptor = connect_control(error);
  if (descriptor < 0) return kFailureExit;
  auto close_and_return = [&](int result) {
    ::close(descriptor);
    return result;
  };

  std::string request;
  switch (options.command) {
    case Command::Pair:
      request = R"({"version":1,"command":"pair"})";
      break;
    case Command::Status:
      request = R"({"version":1,"command":"status"})";
      break;
    case Command::Pairings:
      request = R"({"version":1,"command":"pairings"})";
      break;
    case Command::Doctor:
      request = R"({"version":1,"command":"doctor"})";
      break;
    case Command::Revoke:
      request = "{\"version\":1,\"command\":\"revoke\",\"origin\":" +
                linux_control::encode_linux_control_json_string(
                    options.origin.view()) +
                "}";
      break;
    case Command::CameraSetup:
      return close_and_return(kFailureExit);
  }
  if (!send_request(descriptor, request)) {
    error << "syncctl: failed to send the control request\n";
    return close_and_return(kFailureExit);
  }
  std::string response;
  if (!receive_frame(descriptor, response)) {
    error << "syncctl: failed to read the control response\n";
    return close_and_return(kFailureExit);
  }
  if (options.command == Command::Pair) {
    Prompt prompt;
    if (!render_pair_prompt(response, output, prompt)) {
      error << "syncctl: invalid pairing prompt\n";
      return close_and_return(kFailureExit);
    }
    const bool approved = read_approval(input);
    const std::string decision =
        "{\"version\":1,\"command\":\"decision\",\"generation\":" +
        std::to_string(prompt.generation) + ",\"approved\":" +
        (approved ? "true" : "false") + "}";
    if (!send_request(descriptor, decision) ||
        !receive_frame(descriptor, response) || response_is_error(response)) {
      error << "syncctl: pairing decision was not accepted\n";
      return close_and_return(kFailureExit);
    }
    output << (approved ? "paired\n" : "denied\n");
    return close_and_return(approved ? kSuccessExit : kFailureExit);
  }
  if (response_is_error(response)) {
    error << response << '\n';
    return close_and_return(kFailureExit);
  }
  output << response << '\n';
  if (options.command == Command::Revoke &&
      response.find("\"status\":\"durability_uncertain\"") !=
          std::string::npos) {
    return close_and_return(kDurabilityUncertainExit);
  }
  return close_and_return(kSuccessExit);
}

}  // namespace noisefactor::sync::syncctl
