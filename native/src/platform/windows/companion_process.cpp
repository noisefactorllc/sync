#include "companion_process.hpp"

#include <sync/origin.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

// CMake already links winhttp and ws2_32 into this target; these pragmas
// duplicate that for MSVC so the file also builds standalone. Naming a
// library twice is a no-op for the linker.
#if defined(_MSC_VER)
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace noisefactor::sync::companion {
namespace {

// GenerateConsoleCtrlEvent can only be SENT by a process that owns a console
// and only RECEIVED by one that has a console. Sync.exe is built for the
// WIN32 subsystem, so it starts with neither -- which silently made the
// whole graceful-shutdown path unreachable: the call failed with
// ERROR_INVALID_HANDLE and every stop fell through to TerminateProcess after
// the full termination timeout. Allocating a console and hiding its window
// gives both sides what the API needs while showing the user nothing.
// Idempotent, and a no-op when a console already exists.
void ensure_console_for_control_events() noexcept {
  if (::GetConsoleWindow() != nullptr) return;
  if (::AllocConsole() == 0) return;
  if (HWND console = ::GetConsoleWindow(); console != nullptr) {
    ::ShowWindow(console, SW_HIDE);
  }
}

constexpr std::size_t kMaximumHealthBodyBytes = 65'536;
constexpr std::size_t kManagementOutputLimit = 65'536;

// ---------------------------------------------------------------------
// A small, bounded, hand-rolled JSON reader.
//
// There is no JSON library in this repo's native code (see
// native/src/control.cpp, which takes the same hand-rolled approach for the
// control-channel protocol). Unlike control.cpp's parser -- which is
// field-specific to ControlMessage and rejects unknown keys outright --
// this one builds a small generic value tree so it can both (a) tolerate
// and ignore fields it does not recognise, the same way the macOS
// companion's parse_health does by reading an NSDictionary key-by-key, and
// (b) still be used for the pairing/revocation JSON shapes below, which
// *do* need an exact key set. Every entry point bounds the input size
// before parsing (matching the macOS companion's 65536-byte cap), and the
// parser itself bounds string length, array/object entry counts, and
// nesting depth so a malformed or hostile body cannot cause unbounded
// allocation or a stack overflow.
// ---------------------------------------------------------------------

constexpr std::size_t kMaximumJsonDepth = 32;
constexpr std::size_t kMaximumJsonStringBytes = 4'096;
constexpr std::size_t kMaximumJsonArrayEntries = 256;
constexpr std::size_t kMaximumJsonObjectEntries = 64;

enum class JsonType { Null, Boolean, Number, String, Array, Object };

struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

struct JsonValue {
  JsonType type = JsonType::Null;
  bool boolean_value = false;
  double number_value = 0.0;
  std::string string_value;
  std::unique_ptr<JsonArray> array_value;
  std::unique_ptr<JsonObject> object_value;

  [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept {
    if (type != JsonType::Object || object_value == nullptr) return nullptr;
    for (const auto& entry : *object_value) {
      if (entry.first == key) return &entry.second;
    }
    return nullptr;
  }
};

bool utf8_sequence_length(unsigned char first, std::size_t& length) noexcept {
  if (first < 0x80) { length = 1; return true; }
  if (first >= 0xc2 && first <= 0xdf) { length = 2; return true; }
  if (first >= 0xe0 && first <= 0xef) { length = 3; return true; }
  if (first >= 0xf0 && first <= 0xf4) { length = 4; return true; }
  return false;
}

void append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point <= 0x7f) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7ff) {
    out.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
  } else if (code_point <= 0xffff) {
    out.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
  } else {
    out.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
  }
}

class JsonParser {
 public:
  explicit JsonParser(std::string_view input) : input_(input) {}

  [[nodiscard]] bool parse(JsonValue& out) {
    skip_whitespace();
    if (!parse_value(out, 0)) return false;
    skip_whitespace();
    return position_ == input_.size();
  }

 private:
  void skip_whitespace() noexcept {
    while (position_ < input_.size()) {
      const char byte = input_[position_];
      if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r') break;
      ++position_;
    }
  }

  bool consume(char expected) noexcept {
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  bool consume_literal(std::string_view literal) noexcept {
    if (input_.size() - position_ < literal.size()) return false;
    if (input_.substr(position_, literal.size()) != literal) return false;
    position_ += literal.size();
    return true;
  }

  bool parse_value(JsonValue& out, std::size_t depth) {
    if (depth > kMaximumJsonDepth || position_ >= input_.size()) return false;
    const char byte = input_[position_];
    if (byte == '{') return parse_object(out, depth);
    if (byte == '[') return parse_array(out, depth);
    if (byte == '"') return parse_string_value(out);
    if (byte == 't') {
      if (!consume_literal("true")) return false;
      out.type = JsonType::Boolean;
      out.boolean_value = true;
      return true;
    }
    if (byte == 'f') {
      if (!consume_literal("false")) return false;
      out.type = JsonType::Boolean;
      out.boolean_value = false;
      return true;
    }
    if (byte == 'n') {
      if (!consume_literal("null")) return false;
      out.type = JsonType::Null;
      return true;
    }
    if (byte == '-' || (byte >= '0' && byte <= '9')) return parse_number(out);
    return false;
  }

  bool parse_object(JsonValue& out, std::size_t depth) {
    ++position_;  // '{'
    out.type = JsonType::Object;
    out.object_value = std::make_unique<JsonObject>();
    skip_whitespace();
    if (consume('}')) return true;
    while (true) {
      skip_whitespace();
      JsonValue key;
      if (!parse_string_value(key)) return false;
      if (out.object_value->size() >= kMaximumJsonObjectEntries) return false;
      skip_whitespace();
      if (!consume(':')) return false;
      skip_whitespace();
      JsonValue value;
      if (!parse_value(value, depth + 1)) return false;
      out.object_value->emplace_back(std::move(key.string_value),
                                     std::move(value));
      skip_whitespace();
      if (consume('}')) return true;
      if (!consume(',')) return false;
    }
  }

  bool parse_array(JsonValue& out, std::size_t depth) {
    ++position_;  // '['
    out.type = JsonType::Array;
    out.array_value = std::make_unique<JsonArray>();
    skip_whitespace();
    if (consume(']')) return true;
    while (true) {
      skip_whitespace();
      if (out.array_value->size() >= kMaximumJsonArrayEntries) return false;
      JsonValue value;
      if (!parse_value(value, depth + 1)) return false;
      out.array_value->push_back(std::move(value));
      skip_whitespace();
      if (consume(']')) return true;
      if (!consume(',')) return false;
    }
  }

  bool parse_string_value(JsonValue& out) {
    std::string decoded;
    if (!parse_string_literal(decoded)) return false;
    out.type = JsonType::String;
    out.string_value = std::move(decoded);
    return true;
  }

  bool read_hex_quad(std::uint32_t& value) noexcept {
    if (input_.size() - position_ < 4) return false;
    value = 0;
    for (int index = 0; index < 4; ++index) {
      const char byte = input_[position_++];
      int digit = -1;
      if (byte >= '0' && byte <= '9') digit = byte - '0';
      else if (byte >= 'a' && byte <= 'f') digit = byte - 'a' + 10;
      else if (byte >= 'A' && byte <= 'F') digit = byte - 'A' + 10;
      if (digit < 0) return false;
      value = (value << 4U) | static_cast<std::uint32_t>(digit);
    }
    return true;
  }

  bool parse_string_literal(std::string& out) {
    if (!consume('"')) return false;
    while (position_ < input_.size()) {
      const auto byte = static_cast<unsigned char>(input_[position_]);
      if (byte == '"') {
        ++position_;
        return out.size() <= kMaximumJsonStringBytes;
      }
      if (byte < 0x20) return false;
      if (byte == '\\') {
        ++position_;
        if (position_ >= input_.size()) return false;
        const char escape = input_[position_++];
        switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t code_point = 0;
          if (!read_hex_quad(code_point)) return false;
          if (code_point >= 0xd800 && code_point <= 0xdbff) {
            if (input_.size() - position_ < 6 || input_[position_] != '\\' ||
                input_[position_ + 1] != 'u') {
              return false;
            }
            position_ += 2;
            std::uint32_t low = 0;
            if (!read_hex_quad(low) || low < 0xdc00 || low > 0xdfff) {
              return false;
            }
            code_point =
                0x10000U + ((code_point - 0xd800U) << 10U) + (low - 0xdc00U);
          } else if (code_point >= 0xdc00 && code_point <= 0xdfff) {
            return false;
          }
          append_utf8(out, code_point);
          break;
        }
        default:
          return false;
        }
        if (out.size() > kMaximumJsonStringBytes) return false;
        continue;
      }
      std::size_t sequence_length = 0;
      if (!utf8_sequence_length(byte, sequence_length)) return false;
      if (input_.size() - position_ < sequence_length) return false;
      for (std::size_t index = 1; index < sequence_length; ++index) {
        const auto continuation =
            static_cast<unsigned char>(input_[position_ + index]);
        if ((continuation & 0xc0U) != 0x80U) return false;
      }
      out.append(input_.data() + position_, sequence_length);
      position_ += sequence_length;
      if (out.size() > kMaximumJsonStringBytes) return false;
    }
    return false;  // unterminated string
  }

  bool parse_number(JsonValue& out) {
    const std::size_t start = position_;
    if (position_ < input_.size() && input_[position_] == '-') ++position_;
    if (position_ >= input_.size() || input_[position_] < '0' ||
        input_[position_] > '9') {
      return false;
    }
    if (input_[position_] == '0') {
      ++position_;
    } else {
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      if (position_ >= input_.size() || input_[position_] < '0' ||
          input_[position_] > '9') {
        return false;
      }
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ < input_.size() &&
        (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() &&
          (input_[position_] == '+' || input_[position_] == '-')) {
        ++position_;
      }
      if (position_ >= input_.size() || input_[position_] < '0' ||
          input_[position_] > '9') {
        return false;
      }
      while (position_ < input_.size() && input_[position_] >= '0' &&
             input_[position_] <= '9') {
        ++position_;
      }
    }
    const std::string_view text = input_.substr(start, position_ - start);
    double value = 0.0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
      return false;
    }
    out.type = JsonType::Number;
    out.number_value = value;
    return true;
  }

  std::string_view input_;
  std::size_t position_ = 0;
};

bool parse_json_object(std::string_view json, JsonValue& out) {
  if (json.empty() || json.size() > kMaximumHealthBodyBytes) return false;
  return JsonParser(json).parse(out) && out.type == JsonType::Object;
}

}  // namespace

std::optional<HealthSnapshot> parse_health(std::string_view json) {
  JsonValue root;
  if (!parse_json_object(json, root)) return std::nullopt;

  const JsonValue* product = root.find("product");
  const JsonValue* status = root.find("status");
  const JsonValue* version = root.find("version");
  const JsonValue* protocols = root.find("protocolVersions");
  const JsonValue* capabilities = root.find("capabilities");
  if (product == nullptr || product->type != JsonType::String ||
      product->string_value != "Sync" || status == nullptr ||
      status->type != JsonType::String || status->string_value != "ok" ||
      version == nullptr || version->type != JsonType::String ||
      protocols == nullptr || protocols->type != JsonType::Array ||
      capabilities == nullptr || capabilities->type != JsonType::Object) {
    return std::nullopt;
  }

  bool protocol_v1 = false;
  for (const JsonValue& entry : *protocols->array_value) {
    if (entry.type == JsonType::Number && entry.number_value == 1.0) {
      protocol_v1 = true;
    }
  }
  if (!protocol_v1) return std::nullopt;

  const JsonValue* providers = capabilities->find("providers");
  if (providers == nullptr || providers->type != JsonType::Array) {
    return std::nullopt;
  }

  // Sync publishes through every available send provider at once, so the
  // companion reports the whole set rather than probing for one id. A
  // provider only counts if it is both available AND selected: the daemon
  // marks a provider selected exactly when it is actually publishing
  // through it, so an available-but-deselected provider would tell the
  // operator to go looking for a source that is not there. This mirrors
  // the macOS companion's parse_health exactly, including treating a
  // missing/non-boolean `selected` field as a reason to skip the entry
  // rather than a default.
  AvailableProviders available;
  for (const JsonValue& entry : *providers->array_value) {
    if (entry.type != JsonType::Object) continue;
    const JsonValue* id = entry.find("id");
    const JsonValue* direction = entry.find("direction");
    const JsonValue* is_available = entry.find("available");
    const JsonValue* is_selected = entry.find("selected");
    if (id == nullptr || id->type != JsonType::String ||
        direction == nullptr || direction->type != JsonType::String ||
        direction->string_value != "send" || is_available == nullptr ||
        is_available->type != JsonType::Boolean ||
        !is_available->boolean_value || is_selected == nullptr ||
        is_selected->type != JsonType::Boolean ||
        !is_selected->boolean_value) {
      continue;
    }
    if (!available.add(id->string_value)) {
      // A helper advertising more providers than this build bounds for is
      // reporting something this companion cannot represent truthfully.
      return std::nullopt;
    }
  }

  // The companion only ever polls /status (never the older /health
  // endpoint), and encode_status always includes activeSenders, so its
  // absence here means the body is not actually a /status response.
  const JsonValue* count = root.find("activeSenders");
  if (count == nullptr || count->type != JsonType::Number) {
    return std::nullopt;
  }
  const double numeric = count->number_value;
  if (!std::isfinite(numeric) || numeric < 0 || numeric > 64 ||
      std::floor(numeric) != numeric) {
    return std::nullopt;
  }

  return HealthSnapshot{
      .reachable = true,
      .compatible = true,
      .product = product->string_value,
      .version = version->string_value,
      .providers = available,
      .active_senders = static_cast<std::size_t>(numeric),
  };
}

PairingsResult parse_pairings_json(std::string_view json) {
  PairingsResult result;
  JsonValue root;
  if (!parse_json_object(json, root) ||
      root.object_value->size() != 2) {
    result.error = "Sync returned malformed pairing data.";
    return result;
  }
  const JsonValue* type = root.find("type");
  const JsonValue* origins = root.find("origins");
  if (type == nullptr || type->type != JsonType::String ||
      type->string_value != "pairings" || origins == nullptr ||
      origins->type != JsonType::Array ||
      origins->array_value->size() > 64) {
    result.error = "Sync returned malformed pairing data.";
    return result;
  }

  std::vector<std::string> unique;
  for (const JsonValue& entry : *origins->array_value) {
    if (entry.type != JsonType::String) {
      result.error = "Sync returned malformed pairing data.";
      result.origins.clear();
      return result;
    }
    const auto normalized = normalize_origin(entry.string_value);
    const bool duplicate =
        std::find(unique.begin(), unique.end(), entry.string_value) !=
        unique.end();
    if (!normalized.ok() || normalized.origin.view() != entry.string_value ||
        duplicate) {
      result.error = "Sync returned noncanonical pairing data.";
      result.origins.clear();
      return result;
    }
    unique.push_back(entry.string_value);
    result.origins.push_back(entry.string_value);
  }
  return result;
}

RevocationResult classify_revocation(int exit_status, std::string_view json) {
  RevocationResult result;
  if (exit_status != 0 && exit_status != 3) {
    result.error = "Could not revoke Sync pairing.";
    return result;
  }
  JsonValue root;
  if (!parse_json_object(json, root) || root.object_value->size() != 3) {
    result.error = "Sync returned malformed revocation data.";
    return result;
  }
  const JsonValue* type = root.find("type");
  const JsonValue* origin = root.find("origin");
  const JsonValue* status = root.find("status");
  if (type == nullptr || type->type != JsonType::String ||
      type->string_value != "revocation" || origin == nullptr ||
      origin->type != JsonType::String || status == nullptr ||
      status->type != JsonType::String) {
    result.error = "Sync returned malformed revocation data.";
    return result;
  }
  if (status->string_value == "revoked_durability_uncertain") {
    result.error =
        "Sync removed the pairing but could not confirm it was written to "
        "disk. Revoke again after checking free disk space.";
    return result;
  }
  if (status->string_value != "revoked" && status->string_value != "not_found") {
    result.error = "Sync returned malformed revocation data.";
    return result;
  }
  // Exit 0 with a well-formed record is the only durable outcome. `revoked`
  // means the origin is confirmed absent, which "not_found" also satisfies.
  if (exit_status != 0) {
    result.error = "Sync could not confirm the pairing was revoked.";
    return result;
  }
  result.revoked = true;
  return result;
}

std::string resolve_helper_path(std::string_view module_path) {
  if (module_path.empty()) return {};
  const auto last_backslash = module_path.find_last_of('\\');
  const auto last_slash = module_path.find_last_of('/');
  std::size_t separator = std::string_view::npos;
  if (last_backslash != std::string_view::npos &&
      (last_slash == std::string_view::npos || last_backslash > last_slash)) {
    separator = last_backslash;
  } else {
    separator = last_slash;
  }
  std::string result(separator == std::string_view::npos
                         ? std::string_view{}
                         : module_path.substr(0, separator + 1));
  result += "syncd.exe";
  return result;
}

namespace {

// ---------------------------------------------------------------------
// Win32 process supervision.
// ---------------------------------------------------------------------

std::wstring utf8_to_wide(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int needed = ::MultiByteToWideChar(
      CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), needed);
  return wide;
}

// Standard Windows argv-quoting algorithm (matches CommandLineToArgvW / the
// MSVC CRT's own argv parser), needed because CreateProcessW takes one flat
// command line string rather than an argv array.
std::wstring quote_argument(std::wstring_view argument) {
  if (!argument.empty() &&
      argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring(argument);
  }
  std::wstring quoted = L"\"";
  for (auto it = argument.begin();; ++it) {
    std::size_t backslashes = 0;
    while (it != argument.end() && *it == L'\\') {
      ++backslashes;
      ++it;
    }
    if (it == argument.end()) {
      quoted.append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
      quoted.push_back(L'"');
    } else {
      quoted.append(backslashes, L'\\');
      quoted.push_back(*it);
    }
  }
  quoted.push_back(L'"');
  return quoted;
}

std::wstring build_command_line(const std::wstring& executable,
                                const std::vector<std::wstring>& arguments) {
  std::wstring command_line = quote_argument(executable);
  for (const auto& argument : arguments) {
    command_line.push_back(L' ');
    command_line.append(quote_argument(argument));
  }
  return command_line;
}

struct HandleDeleter {
  using pointer = HANDLE;
  void operator()(HANDLE handle) const noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle);
    }
  }
};
using UniqueHandle = std::unique_ptr<void, HandleDeleter>;

// WinHTTP handles are HINTERNET (also a void*), but MUST be released with
// WinHttpCloseHandle rather than CloseHandle -- a separate RAII type keeps
// that distinction impossible to get wrong by accident.
struct WinHttpHandleDeleter {
  using pointer = HINTERNET;
  void operator()(HINTERNET handle) const noexcept {
    if (handle != nullptr) ::WinHttpCloseHandle(handle);
  }
};
using UniqueWinHttpHandle = std::unique_ptr<void, WinHttpHandleDeleter>;

UniqueHandle create_kill_on_close_job() {
  UniqueHandle job(::CreateJobObjectW(nullptr, nullptr));
  if (!job) return job;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!::SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
    return UniqueHandle{};
  }
  return job;
}

void perform_status_request(std::uint16_t port, std::uint32_t timeout_ms,
                            bool& ok, DWORD& status_code, std::string& body) {
  ok = false;
  status_code = 0;
  body.clear();

  UniqueWinHttpHandle session(::WinHttpOpen(
      L"Sync/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) return;
  const int timeout = static_cast<int>(timeout_ms);
  ::WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout);

  // Always 127.0.0.1: the health probe must never be pointed at a remote
  // host, so the target is hardcoded rather than accepted as a URL.
  UniqueWinHttpHandle connection(
      ::WinHttpConnect(session.get(), L"127.0.0.1", port, 0));
  if (!connection) return;

  UniqueWinHttpHandle request(::WinHttpOpenRequest(
      connection.get(), L"GET", L"/status", nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
  if (!request) return;

  // Never follow redirects: a redirect is not a legitimate answer from our
  // own loopback-only daemon, and following one could send this probe
  // somewhere other than 127.0.0.1.
  DWORD disable_redirects = WINHTTP_DISABLE_REDIRECTS;
  ::WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
                     &disable_redirects, sizeof(disable_redirects));

  if (!::WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
    return;
  }
  if (!::WinHttpReceiveResponse(request.get(), nullptr)) return;

  DWORD status_size = sizeof(status_code);
  ::WinHttpQueryHeaders(request.get(),
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code,
                        &status_size, WINHTTP_NO_HEADER_INDEX);

  for (;;) {
    DWORD available = 0;
    if (!::WinHttpQueryDataAvailable(request.get(), &available) ||
        available == 0) {
      break;
    }
    if (body.size() + available > kMaximumHealthBodyBytes) break;
    std::vector<char> chunk(available);
    DWORD read = 0;
    if (!::WinHttpReadData(request.get(), chunk.data(), available, &read)) {
      break;
    }
    body.append(chunk.data(), read);
    if (read == 0) break;
  }
  ok = true;
}

bool tcp_port_reachable(std::uint16_t port, std::uint32_t timeout_ms) {
  WSADATA wsa_data{};
  if (::WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) return false;
  bool reachable = false;
  const SOCKET socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_handle != INVALID_SOCKET) {
    u_long non_blocking = 1;
    ::ioctlsocket(socket_handle, FIONBIO, &non_blocking);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    const int connected = ::connect(
        socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (connected == 0) {
      reachable = true;
    } else if (::WSAGetLastError() == WSAEWOULDBLOCK) {
      fd_set writable;
      FD_ZERO(&writable);
      FD_SET(socket_handle, &writable);
      timeval timeout{};
      timeout.tv_sec = static_cast<long>(timeout_ms / 1000);
      timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
      if (::select(0, nullptr, &writable, nullptr, &timeout) > 0) {
        int socket_error = 0;
        int error_size = sizeof(socket_error);
        if (::getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&socket_error),
                         &error_size) == 0 &&
            socket_error == 0) {
          reachable = true;
        }
      }
    }
    ::closesocket(socket_handle);
  }
  ::WSACleanup();
  return reachable;
}

}  // namespace

struct OwnedProcessState {
  UniqueHandle process;
  UniqueHandle thread;
  UniqueHandle job;
  UniqueHandle stderr_read;
  DWORD pid = 0;
  // stderr_reader and exit_waiter are deliberately NOT std::thread members
  // here: both threads capture a shared_ptr<OwnedProcessState> back to this
  // very struct (they need `state` alive to reach stderr_callback/
  // exit_callback), which would make a joinable std::thread member of this
  // struct a reference cycle -- the struct could never be destroyed to
  // release the thread, and the thread could never be joined/detached to
  // let the struct go. Instead they are spawned as immediately-detached
  // threads and tracked through CompanionProcess::Impl's begin_operation/
  // end_operation bracketing (the same mechanism probe() and
  // run_management() use), which needs no named thread handle at all.
  // Guards the two callbacks below. ~CompanionProcess clears them from the
  // owner thread while the stderr reader and the exit waiter may still be
  // invoking them -- and when no dispatcher is configured, Impl::dispatch
  // runs those inline, on the background thread. Reading a std::function on
  // one thread while another assigns to it is a data race whose failure mode
  // is a call through a half-torn object, so every access goes through the
  // accessors below.
  std::mutex callback_mutex;
  CompanionProcess::StderrCallback stderr_callback;  // guarded
  CompanionProcess::ExitCallback exit_callback;      // guarded

  // Copied rather than moved: stderr arrives in many chunks, so this one has
  // to survive being invoked repeatedly.
  [[nodiscard]] CompanionProcess::StderrCallback copy_stderr_callback() {
    std::lock_guard lock(callback_mutex);
    return stderr_callback;
  }
  // Moved out: a process exits once, and taking it here is what guarantees
  // it cannot be invoked twice.
  [[nodiscard]] CompanionProcess::ExitCallback take_exit_callback() {
    std::lock_guard lock(callback_mutex);
    return std::move(exit_callback);
  }
  void clear_callbacks() {
    std::lock_guard lock(callback_mutex);
    stderr_callback = nullptr;
    exit_callback = nullptr;
  }

  std::mutex termination_mutex;
  std::vector<CompanionProcess::Completion> termination_completions;
  bool termination_requested = false;
  // Set once the exit waiter has taken the completions above and run them.
  // A completion pushed after that point would sit in a vector nothing will
  // ever look at again, so terminate() has to run it directly instead --
  // otherwise a caller that asked to stop a helper which had just exited on
  // its own waits for a completion that never comes.
  bool termination_completions_drained = false;
};

struct CompanionProcess::Impl {
  explicit Impl(CompanionProcessOptions value) : options(std::move(value)) {}

  CompanionProcessOptions options;
  std::mutex state_mutex;
  std::shared_ptr<OwnedProcessState> owned;  // guarded by state_mutex

  // Marshals `callback` onto the UI/owner thread via options.dispatch_to_
  // owner, or runs it inline if no dispatcher was configured (as in tests).
  void dispatch(std::function<void()> callback) {
    if (options.dispatch_to_owner) {
      options.dispatch_to_owner(std::move(callback));
    } else {
      callback();
    }
  }

  // Tracks detached background threads (probe(), run_management(),
  // terminate()'s hard-kill watchdog) that touch `this` so the destructor
  // can wait for all of them to finish before Impl itself is torn down,
  // without needing to keep a std::thread handle for each one. Every such
  // thread calls begin_operation() synchronously, on the calling thread,
  // before it is spawned, and end_operation() as the very last thing it
  // does that touches `this`.
  std::mutex outstanding_mutex;
  std::condition_variable outstanding_condition;
  std::size_t outstanding = 0;

  void begin_operation() {
    std::lock_guard lock(outstanding_mutex);
    ++outstanding;
  }
  void end_operation() {
    std::lock_guard lock(outstanding_mutex);
    --outstanding;
    if (outstanding == 0) outstanding_condition.notify_all();
  }
  void drain_operations() noexcept {
    std::unique_lock lock(outstanding_mutex);
    outstanding_condition.wait(lock, [this] { return outstanding == 0; });
  }

  // Spawns a detached background thread under the begin/end_operation
  // bracket. The bracket has to be taken before the thread starts (or the
  // thread could finish before it was ever counted), which leaves one hole:
  // std::thread's constructor throws std::system_error when the system
  // cannot create a thread, and an increment with no matching decrement
  // means `outstanding` never returns to zero and drain_operations() waits
  // for a thread that does not exist -- a permanent hang on shutdown, from a
  // transient resource failure. Undoing the bracket on that path closes it.
  // Returns false if the thread could not be started; callers that owe
  // someone a completion must still deliver one.
  template <typename Work>
  [[nodiscard]] bool spawn_tracked(Work work) {
    begin_operation();
    try {
      std::thread(std::move(work)).detach();
      return true;
    } catch (...) {
      end_operation();
      return false;
    }
  }

  // Hands a completion to the owner thread as a tracked operation. Plain
  // dispatch() is not enough for anything user-visible: without the bracket
  // ~CompanionProcess can return while the completion is still sitting in
  // the owner's queue, and it then runs against state the caller believed
  // was finished with.
  void dispatch_tracked(std::function<void()> callback) {
    begin_operation();
    dispatch([this, callback = std::move(callback)]() mutable {
      callback();
      end_operation();
    });
  }

  void run_management(std::vector<std::wstring> arguments,
                      std::function<void(int, std::string, bool)> completion);
};

void CompanionProcess::Impl::run_management(
    std::vector<std::wstring> arguments,
    std::function<void(int, std::string, bool)> completion) {
  begin_operation();

  // Management commands get the same kill-on-close job the long-lived helper
  // gets. They are short and bounded by a watchdog, so this looks redundant
  // -- but the watchdog lives in this process, and if this process is killed
  // outright the watchdog dies with it while the child keeps running. That
  // child holds the pairing store's lock file, so an orphan does not merely
  // linger: it makes the next syncd fail to open the store.
  UniqueHandle job = create_kill_on_close_job();
  if (!job) {
    dispatch([this, completion = std::move(completion)]() mutable {
      completion(-1, {}, false);
      end_operation();
    });
    return;
  }

  SECURITY_ATTRIBUTES inheritable_sa{sizeof(SECURITY_ATTRIBUTES), nullptr,
                                     TRUE};
  HANDLE stdout_read_raw = nullptr;
  HANDLE stdout_write_raw = nullptr;
  if (!::CreatePipe(&stdout_read_raw, &stdout_write_raw, &inheritable_sa, 0)) {
    dispatch([this, completion = std::move(completion)]() mutable {
      completion(-1, {}, false);
      end_operation();
    });
    return;
  }
  UniqueHandle stdout_read(stdout_read_raw);
  UniqueHandle stdout_write(stdout_write_raw);
  ::SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0);

  HANDLE nul_raw =
      ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                   FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable_sa,
                   OPEN_EXISTING, 0, nullptr);
  UniqueHandle nul(nul_raw == INVALID_HANDLE_VALUE ? nullptr : nul_raw);

  std::wstring command_line =
      build_command_line(options.helper_path, arguments);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = nul.get();
  startup.hStdOutput = stdout_write.get();
  startup.hStdError = nul.get();

  PROCESS_INFORMATION process_info{};
  // CREATE_SUSPENDED so the child is confined to the job before it can run a
  // single instruction; assigning afterwards would leave a window in which an
  // unconfined child exists. Same reasoning as start().
  const BOOL created = ::CreateProcessW(
      options.helper_path.c_str(), command_line.data(), nullptr,
      nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
      &startup, &process_info);
  // The parent must close its own copies of the pipe write end and the NUL
  // handle right after CreateProcess: the child has its own (inherited)
  // handles to them, and if the parent kept a copy of the write end open,
  // the reader thread below would never see end-of-file even after the
  // child exits. RAII does this the moment these locals go out of scope,
  // but it is done explicitly here so the ordering is obvious.
  stdout_write.reset();
  nul.reset();
  if (!created) {
    dispatch([this, completion = std::move(completion)]() mutable {
      completion(-1, {}, false);
      end_operation();
    });
    return;
  }
  UniqueHandle thread_handle(process_info.hThread);

  auto process = std::make_shared<UniqueHandle>(process_info.hProcess);
  if (!::AssignProcessToJobObject(job.get(), process->get())) {
    ::TerminateProcess(process->get(), 1);
    ::ResumeThread(thread_handle.get());  // let it die rather than hang
                                          // suspended forever
    dispatch([this, completion = std::move(completion)]() mutable {
      completion(-1, {}, false);
      end_operation();
    });
    return;
  }
  ::ResumeThread(thread_handle.get());
  // Held by the reader thread below, which outlives the child: the job must
  // stay open until the child has exited, because closing it is what kills
  // whatever is still inside.
  auto job_holder = std::make_shared<UniqueHandle>(std::move(job));
  auto timed_out = std::make_shared<std::atomic_bool>(false);
  const std::uint32_t timeout_ms = options.management_timeout_ms;
  // Bounded, self-contained watchdog: it captures only shared_ptr/atomic
  // state (never `this`), so it is safe to detach outright -- it cannot
  // outlive anything it touches, and it never needs Impl to still exist.
  try {
    std::thread([process, timed_out, timeout_ms] {
      if (::WaitForSingleObject(process->get(), timeout_ms) == WAIT_TIMEOUT) {
        timed_out->store(true, std::memory_order_release);
        ::TerminateProcess(process->get(), 1);
      }
    }).detach();
  } catch (...) {
    // Nothing would bound the child, and the reader below waits on it with
    // INFINITE -- so a helper that hung would hang this operation, and with
    // it ~CompanionProcess. Ending the child now keeps the bound.
    timed_out->store(true, std::memory_order_release);
    ::TerminateProcess(process->get(), 1);
  }

  // `completion` is captured by copy, not moved: if the thread cannot be
  // created the lambda is destroyed and a moved-from completion would leave
  // the catch below with nothing to call.
  try {
  std::thread([this, process, job_holder,
              thread_handle = std::move(thread_handle),
              stdout_read = std::move(stdout_read), timed_out,
              completion]() mutable {
    std::string output;
    output.reserve(4096);
    std::array<char, 4096> buffer{};
    for (;;) {
      DWORD read = 0;
      const BOOL ok = ::ReadFile(stdout_read.get(), buffer.data(),
                                 static_cast<DWORD>(buffer.size()), &read,
                                 nullptr);
      if (!ok || read == 0) break;
      const std::size_t capacity = kManagementOutputLimit > output.size()
                                       ? kManagementOutputLimit - output.size()
                                       : 0;
      output.append(buffer.data(), std::min<std::size_t>(read, capacity));
    }
    ::WaitForSingleObject(process->get(), INFINITE);
    DWORD status = 0;
    ::GetExitCodeProcess(process->get(), &status);
    const bool timeout = timed_out->load(std::memory_order_acquire);
    dispatch([this, completion, status, output = std::move(output),
             timeout]() mutable {
      completion(timeout ? -1 : static_cast<int>(status), std::move(output),
                timeout);
      end_operation();
    });
  }).detach();
  } catch (...) {
    // This thread carries the begin_operation() taken at the top of this
    // function and owes the caller a completion. Without both, the caller
    // waits forever and so does ~CompanionProcess.
    ::TerminateProcess(process->get(), 1);
    dispatch([this, completion = std::move(completion)]() mutable {
      completion(-1, {}, false);
      end_operation();
    });
  }
}

CompanionProcess::CompanionProcess(CompanionProcessOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

CompanionProcess::~CompanionProcess() {
  std::shared_ptr<OwnedProcessState> state;
  {
    std::lock_guard lock(impl_->state_mutex);
    state = impl_->owned;
    impl_->owned.reset();
  }
  if (state != nullptr) {
    // Callbacks are cleared before the kill below: dispatch() may still be
    // marshaling a previously-queued stderr/exit notification onto the
    // owner thread, and once this destructor returns nothing may call back
    // into whatever those callbacks' captures depended on.
    state->clear_callbacks();
    // Closing the job object is the actual kill: JOB_OBJECT_LIMIT_KILL_ON_
    // JOB_CLOSE means every process in it, including syncd.exe, is
    // terminated by the kernel the instant the job's last handle closes.
    // This is what guarantees the helper cannot outlive Sync.exe even if
    // Sync.exe itself is killed outright rather than exiting cleanly
    // through this destructor -- the Windows analogue of the macOS
    // supervision guarantee the mac smoke test asserts.
    state->job.reset();
  }
  // Waits for every background thread this Impl has spawned -- the owned
  // helper's stderr reader and exit waiter (if `state` was non-null above,
  // killing the job just unblocked them) as well as any still-running
  // probe()/list_pairings()/revoke_pairing()/terminate() work -- to finish
  // touching *impl_. Each is bounded by the job kill just performed or by its
  // own *_timeout_ms, and it is what guarantees no background thread can
  // use-after-free once this destructor returns.
  //
  // It is NOT unconditionally bounded, and it would be wrong to claim so: an
  // operation completes by being dispatched, so if options.dispatch_to_owner
  // stops running callbacks before this destructor is reached, this waits
  // forever. That is the contract documented on dispatch_to_owner in the
  // header, and app_main.cpp satisfies it by falling back to running them
  // inline once its message loop is gone. A caller that cannot make that
  // guarantee must not destroy a CompanionProcess.
  impl_->drain_operations();
}

std::vector<std::wstring> CompanionProcess::launch_arguments() const {
  std::vector<std::wstring> arguments{L"--publisher", L"spout", L"--publisher",
                                      L"ndi"};
  if (!impl_->options.spout_library_path.empty()) {
    arguments.push_back(L"--spout-library");
    arguments.push_back(impl_->options.spout_library_path);
  }
  return arguments;
}

std::optional<int> CompanionProcess::owned_pid() const noexcept {
  std::lock_guard lock(impl_->state_mutex);
  if (impl_->owned == nullptr) return std::nullopt;
  return static_cast<int>(impl_->owned->pid);
}

bool CompanionProcess::start(StderrCallback stderr_callback,
                             ExitCallback exit_callback, std::string& error) {
  {
    std::lock_guard lock(impl_->state_mutex);
    if (impl_->owned != nullptr) {
      error = "Sync helper is already running.";
      return false;
    }
  }

  UniqueHandle job = create_kill_on_close_job();
  if (!job) {
    error = "Could not create Sync helper's job object.";
    return false;
  }

  SECURITY_ATTRIBUTES inheritable_sa{sizeof(SECURITY_ATTRIBUTES), nullptr,
                                     TRUE};
  HANDLE stderr_read_raw = nullptr;
  HANDLE stderr_write_raw = nullptr;
  if (!::CreatePipe(&stderr_read_raw, &stderr_write_raw, &inheritable_sa, 0)) {
    error = "Could not create Sync helper's stderr pipe.";
    return false;
  }
  UniqueHandle stderr_read(stderr_read_raw);
  UniqueHandle stderr_write(stderr_write_raw);
  // The read end must never be inherited by the child, or the child would
  // hold a redundant handle to it that keeps our reader thread's ReadFile
  // loop from ever seeing end-of-file.
  ::SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0);

  HANDLE nul_raw =
      ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                   FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable_sa,
                   OPEN_EXISTING, 0, nullptr);
  UniqueHandle nul(nul_raw == INVALID_HANDLE_VALUE ? nullptr : nul_raw);

  const std::vector<std::wstring> arguments = launch_arguments();
  std::wstring command_line =
      build_command_line(impl_->options.helper_path, arguments);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = nul.get();
  startup.hStdOutput = nul.get();
  startup.hStdError = stderr_write.get();

  PROCESS_INFORMATION process_info{};
  // CREATE_SUSPENDED: gives us a chance to assign the child to the
  // kill-on-close job object below before it can run at all -- assigning
  // after resuming it would leave a window where a live, unconfined child
  // exists.
  // CREATE_NEW_PROCESS_GROUP: lets terminate() target this child alone
  // with a console control event later, instead of broadcasting to every
  // process attached to our console (which would include Sync.exe itself).
  // Must precede CreateProcessW: the child inherits this console, and
  // GenerateConsoleCtrlEvent needs the caller to have one too.
  ensure_console_for_control_events();

  // Deliberately NOT CREATE_NO_WINDOW. That flag leaves the child with no
  // console at all, and a process without one can never receive a console
  // control event -- which would make terminate()'s graceful path dead code
  // and leave TerminateProcess as the only way to stop the helper. The child
  // inherits the hidden console allocated just above instead, so nothing is
  // displayed to the user either way.
  const DWORD creation_flags = CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP;
  const BOOL created = ::CreateProcessW(
      impl_->options.helper_path.c_str(), command_line.data(), nullptr,
      nullptr, /*bInheritHandles=*/TRUE, creation_flags, nullptr, nullptr,
      &startup, &process_info);
  stderr_write.reset();
  nul.reset();
  if (!created) {
    error = "Could not launch Sync helper.";
    return false;
  }
  UniqueHandle process(process_info.hProcess);
  UniqueHandle thread(process_info.hThread);

  if (!::AssignProcessToJobObject(job.get(), process.get())) {
    error = "Could not confine Sync helper to its job object.";
    ::TerminateProcess(process.get(), 1);
    ::ResumeThread(thread.get());  // let it run its course to exit, not hang
                                   // suspended forever
    return false;
  }
  ::ResumeThread(thread.get());

  auto state = std::make_shared<OwnedProcessState>();
  state->process = std::move(process);
  state->thread = std::move(thread);
  state->job = std::move(job);
  state->stderr_read = std::move(stderr_read);
  state->pid = process_info.dwProcessId;
  state->stderr_callback = std::move(stderr_callback);
  state->exit_callback = std::move(exit_callback);

  Impl* impl = impl_.get();
  HANDLE stderr_read_handle = state->stderr_read.get();
  HANDLE process_handle = state->process.get();
  // A helper with no stderr reader is degraded but still supervised, so a
  // failure here is not fatal to the launch; a helper with no exit waiter is
  // a different matter and is handled below.
  const bool reading_stderr = impl->spawn_tracked([impl, state, stderr_read_handle] {
    std::array<char, 4096> buffer{};
    for (;;) {
      DWORD read = 0;
      const BOOL ok = ::ReadFile(stderr_read_handle, buffer.data(),
                                 static_cast<DWORD>(buffer.size()), &read,
                                 nullptr);
      if (!ok || read == 0) break;
      std::string bytes(buffer.data(), read);
      impl->dispatch([state, bytes = std::move(bytes)] {
        if (auto callback = state->copy_stderr_callback()) callback(bytes);
      });
    }
    // end_operation() is safe to call from any thread (it only touches its
    // own mutex/condition_variable), so this does not need to go through
    // dispatch() the way user-visible callbacks do.
    impl->end_operation();
  });
  if (!reading_stderr) {
    // With the read end open but unread, the helper's first few kilobytes of
    // stderr fill the pipe and its next write blocks its event loop. Closing
    // our end turns that write into a broken pipe the CRT discards instead.
    state->stderr_read.reset();
  }
  const bool watching_exit = impl->spawn_tracked([impl, state,
                                                  process_handle] {
    ::WaitForSingleObject(process_handle, INFINITE);
    DWORD status = 0;
    ::GetExitCodeProcess(process_handle, &status);
    impl->dispatch([impl, state, status] {
      {
        std::lock_guard lock(impl->state_mutex);
        if (impl->owned == state) impl->owned.reset();
      }
      if (auto callback = state->take_exit_callback()) {
        callback(static_cast<int>(status));
      }
      std::vector<CompanionProcess::Completion> completions;
      {
        std::lock_guard lock(state->termination_mutex);
        completions = std::move(state->termination_completions);
        // Anything pushed after this point must be run by terminate()
        // itself; nothing will look at the vector again.
        state->termination_completions_drained = true;
      }
      for (auto& completion : completions) completion();
      impl->end_operation();
    });
  });
  if (!watching_exit) {
    // Without this thread nothing would ever notice the helper exiting, so
    // the tray would show a running helper forever and terminate() would
    // never complete. Refuse the launch -- but kill the child here rather
    // than leaving it to `state` going out of scope, which would not happen:
    // the stderr reader (if it started) holds a reference to `state`, and it
    // is blocked in ReadFile until the child closes its end of the pipe,
    // which only happens when the child dies. Closing the job breaks that
    // cycle; the reader then sees end-of-file and releases `state`.
    state->clear_callbacks();
    state->job.reset();
    error = "Could not supervise Sync helper.";
    return false;
  }

  {
    std::lock_guard lock(impl_->state_mutex);
    impl_->owned = state;
  }
  error.clear();
  return true;
}

void CompanionProcess::probe(ProbeCallback completion) {
  Impl* impl = impl_.get();
  const std::uint16_t port = impl_->options.port;
  const std::uint32_t timeout_ms = impl_->options.health_timeout_ms;
  const bool spawned = impl->spawn_tracked([impl, port, timeout_ms,
                                            completion]() mutable {
    bool http_ok = false;
    DWORD http_status = 0;
    std::string body;
    perform_status_request(port, timeout_ms, http_ok, http_status, body);

    std::optional<HealthSnapshot> health;
    std::string message;
    if (http_ok && http_status == 200) {
      health = parse_health(body);
    }
    if (!health.has_value()) {
      if (http_ok || tcp_port_reachable(port, timeout_ms)) {
        // Assigned field by field rather than with a partial designated
        // initialiser: leaving the remaining members implicit is what
        // -Wmissing-field-initializers flags, and spelling every one of them
        // out here would just be noise around the two that carry meaning.
        HealthSnapshot occupied{};
        occupied.reachable = true;
        occupied.compatible = false;
        health = std::move(occupied);
        message = "TCP " + std::to_string(port) +
                  " is occupied by an incompatible service.";
      } else {
        message = "Sync status was unavailable.";
      }
    }
    impl->dispatch([impl, completion, health, message]() mutable {
      completion(std::move(health), std::move(message));
      impl->end_operation();
    });
  });
  if (!spawned) {
    // A caller that asked for a probe is waiting for exactly one answer, so
    // failing to start the thread still has to produce one -- silently
    // dropping it would leave the tray showing a stale status forever.
    impl->dispatch_tracked([completion = std::move(completion)]() mutable {
      completion(std::nullopt, "Sync status was unavailable.");
    });
  }
}

void CompanionProcess::terminate(Completion completion) {
  std::shared_ptr<OwnedProcessState> state;
  {
    std::lock_guard lock(impl_->state_mutex);
    state = impl_->owned;
  }
  if (state == nullptr) {
    // Tracked, not a bare dispatch: the destructor must wait for this
    // completion like any other, or it can run after ~CompanionProcess has
    // returned and the caller has torn down whatever it captured.
    impl_->dispatch_tracked(std::move(completion));
    return;
  }
  bool already_stopped = false;
  {
    std::lock_guard lock(state->termination_mutex);
    // The helper can exit between reading `owned` above and taking this
    // lock. The exit waiter empties termination_completions and marks it
    // drained, so a completion pushed after that would sit in a vector
    // nothing reads again -- the caller would wait forever for a helper that
    // has already stopped.
    if (state->termination_completions_drained) {
      already_stopped = true;
    } else {
      state->termination_completions.push_back(std::move(completion));
      if (state->termination_requested) return;
      state->termination_requested = true;
    }
  }
  if (already_stopped) {
    // Deliberately outside the lock above: with no dispatcher configured,
    // dispatch() runs the completion inline on this thread, and running
    // caller code while holding termination_mutex would deadlock the moment
    // that code called back into terminate().
    impl_->dispatch_tracked(std::move(completion));
    return;
  }
  // CTRL_BREAK_EVENT is the closest Windows analogue to POSIX SIGTERM for a
  // console-subsystem child: it asks the process to shut down instead of
  // just killing it outright. It reaches only this child (rather than every
  // process sharing our console, which would include Sync.exe itself)
  // because the child was created with CREATE_NEW_PROCESS_GROUP, making its
  // own PID double as its process group id.
  //
  // Requires BOTH processes to have a console, which is why start() allocates
  // a hidden one and does not pass CREATE_NO_WINDOW. The result is
  // deliberately not treated as an error: the watchdog below bounds the
  // shutdown either way, so a failure here costs a clean exit and the
  // termination timeout, never correctness.
  (void)::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, state->pid);

  Impl* impl = impl_.get();
  const std::uint32_t timeout_ms = impl_->options.termination_timeout_ms;
  const bool watching = impl->spawn_tracked([impl, state, timeout_ms] {
    const HANDLE process = state->process.get();
    if (::WaitForSingleObject(process, timeout_ms) == WAIT_TIMEOUT) {
      ::TerminateProcess(process, 1);
    }
    // The actual completion callbacks fire from the exit_waiter thread
    // started in start() once the process is actually gone; this watchdog
    // only has to guarantee that happens within timeout_ms.
    impl->end_operation();
  });
  if (!watching) {
    // Nothing is left to bound the shutdown, and the console event above may
    // not have been honoured, so stopping the helper falls to this thread.
    // Killing it outright is worse than a graceful exit and better than a
    // helper that never stops: the exit waiter still reports it, and the
    // queued completions still run.
    ::TerminateProcess(state->process.get(), 1);
  }
}

void CompanionProcess::list_pairings(PairingsCallback completion) {
  impl_->run_management(
      {L"--list-pairings"},
      [completion = std::move(completion)](int status, std::string output,
                                           bool timed_out) {
        if (timed_out) {
          completion({}, "Sync management command timed out.");
          return;
        }
        if (status != 0 || output.size() > kManagementOutputLimit) {
          completion({}, "Could not list Sync pairings.");
          return;
        }
        PairingsResult parsed = parse_pairings_json(output);
        completion(std::move(parsed.origins), std::move(parsed.error));
      });
}

void CompanionProcess::revoke_pairing(std::string origin,
                                      RevokeCallback completion) {
  const auto normalized = normalize_origin(origin);
  if (!normalized.ok() || normalized.origin.view() != origin) {
    // Tracked, like every other completion: a bare dispatch can still be
    // sitting in the owner's queue when ~CompanionProcess returns, and would
    // then run against whatever the caller had already torn down.
    impl_->dispatch_tracked([completion = std::move(completion)] {
      completion(false, "Pairing origin is invalid.");
    });
    return;
  }
  impl_->run_management(
      {L"--revoke-origin", utf8_to_wide(origin)},
      [completion = std::move(completion)](int status, std::string output,
                                           bool timed_out) {
        if (timed_out) {
          completion(false, "Sync management command timed out.");
          return;
        }
        RevocationResult parsed = classify_revocation(status, output);
        completion(parsed.revoked, std::move(parsed.error));
      });
}

} // namespace noisefactor::sync::companion
