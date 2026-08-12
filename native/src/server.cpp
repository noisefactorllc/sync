#include <sync/server.hpp>

#include <sync/control.hpp>
#include <sync/frame_receiver.hpp>
#include <sync/origin.hpp>
#include <sync/pairing.hpp>
#include <sync/websocket.hpp>

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <uv.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace noisefactor::sync {
namespace {

constexpr std::size_t kMaximumSenders = 64;
constexpr std::size_t kMaximumSenderOwners = kMaximumSenders;
constexpr std::size_t kManagementConnectionHeadroom = 16;
constexpr std::size_t kMaximumConnections =
    kMaximumSenders + kMaximumSenderOwners + kManagementConnectionHeadroom;
constexpr std::size_t kMaximumPairingCooldowns = 64;
constexpr std::size_t kMaximumControlMessageBytes = 16U * 1024U;
constexpr std::size_t kMaximumDataMessageBytes = 64U * 1024U * 1024U + 64U;
constexpr std::size_t kMaximumQueuedWriteBytes = 64U * 1024U;
constexpr std::size_t kReadBufferBytes = 64U * 1024U;
constexpr std::uint64_t kHttpHeaderDeadlineMs = 1000;
constexpr std::uint64_t kControlHelloDeadlineMs = 1000;
constexpr std::uint64_t kPairingRequestDeadlineMs = 1000;
#if defined(SYNC_PAIRING_PROMPT_DEADLINE_MS)
static_assert(SYNC_PAIRING_PROMPT_DEADLINE_MS > 0);
constexpr std::uint64_t kPairingPromptDeadlineMs =
    SYNC_PAIRING_PROMPT_DEADLINE_MS;
#else
constexpr std::uint64_t kPairingPromptDeadlineMs = 30'000;
#endif
constexpr std::uint64_t kPairingCooldownMs = 1000;
constexpr std::uint64_t kWebSocketCloseDeadlineMs = 750;
constexpr std::uint64_t kDeadlineSweepIntervalMs = 50;

class Server;

enum class ConnectionRole {
  Http,
  ControlUnauthenticated,
  ControlAuthenticated,
  Pairing,
  Data,
};

enum class DeadlineKind {
  None,
  HttpHeader,
  ControlHello,
  PairingRequest,
  PairingPrompt,
  WebSocketClose,
};

enum class AuthorityState {
  None,
  AuthenticationPending,
  PairingIssuePending,
};

struct Connection {
  explicit Connection(Server &owner) : server(owner) {}

  Server &server;
  uv_tcp_t handle{};
  std::size_t slot = kMaximumConnections;
  ConnectionRole role = ConnectionRole::Http;
  bool closing = false;
  bool transport_close_pending = false;
  bool websocket_close_sent = false;
  bool websocket_close_received = false;
  bool websocket_close_write_completed = false;
  bool force_close_after_websocket_write = false;
  bool decoder_terminal = false;
  DeadlineKind deadline_kind = DeadlineKind::None;
  std::uint64_t deadline_ms = 0;
  std::size_t pending_write_bytes = 0;
  std::array<char, kReadBufferBytes> read_buffer{};
  std::string http_buffer;
  std::unique_ptr<websocket::ClientFrameDecoder> decoder;
  std::optional<std::size_t> sender_slot;
  NormalizedOrigin origin{};
  std::uint64_t pairing_generation = 0;
  bool pairing_message_received = false;
  AuthorityState authority_state = AuthorityState::None;
  std::uint64_t authority_generation = 0;
};

struct Sender {
  bool occupied = false;
  std::string id;
  std::string name;
  std::string ticket;
  Connection *owner = nullptr;
  Connection *data = nullptr;
  NormalizedOrigin origin{};
};

struct PairingCooldown {
  NormalizedOrigin origin{};
  std::uint64_t until_ms = 0;
};

class ScopedStringCleanse {
public:
  explicit ScopedStringCleanse(std::string &value) noexcept : value_(value) {}
  ~ScopedStringCleanse() {
    if (!value_.empty())
      OPENSSL_cleanse(value_.data(), value_.size());
  }

  ScopedStringCleanse(const ScopedStringCleanse &) = delete;
  ScopedStringCleanse &operator=(const ScopedStringCleanse &) = delete;

private:
  std::string &value_;
};

class ScopedByteCleanse {
public:
  explicit ScopedByteCleanse(std::span<std::byte> value) noexcept
      : value_(value) {}
  ~ScopedByteCleanse() {
    if (!value_.empty())
      OPENSSL_cleanse(value_.data(), value_.size());
  }

  ScopedByteCleanse(const ScopedByteCleanse &) = delete;
  ScopedByteCleanse &operator=(const ScopedByteCleanse &) = delete;

private:
  std::span<std::byte> value_;
};

class TestPublisher final : public FramePublisher {
public:
  auto open_sender(std::string_view sender_id, std::string_view name) noexcept
      -> bool override {
    if (name == "__sync_test_reject_open__")
      return false;
    if (sender_id.empty() || sender_id.size() > 128 || name.empty() ||
        name.size() > 64 || find(sender_id) != nullptr) {
      return false;
    }
    for (Entry &entry : entries_) {
      if (!entry.occupied) {
        std::copy(sender_id.begin(), sender_id.end(), entry.id.begin());
        std::copy(name.begin(), name.end(), entry.name.begin());
        entry.id_length = sender_id.size();
        entry.name_length = name.size();
        entry.backpressure_once = name == "__sync_test_backpressure_once__";
        entry.occupied = true;
        return true;
      }
    }
    return false;
  }

  void close_sender(std::string_view sender_id) noexcept override {
    Entry* entry = find(sender_id);
    if (entry != nullptr) *entry = Entry{};
  }

  auto publish(std::string_view sender_id, const protocol::FrameView& frame) noexcept
      -> PublishResult override {
    Entry* entry = find(sender_id);
    if (entry == nullptr) return PublishResult::Failed;
    if (entry->backpressure_once) {
      entry->backpressure_once = false;
      return PublishResult::Backpressured;
    }

    std::uint32_t checksum = 0x811c9dc5U;
    for (const std::byte byte : frame.payload) {
      checksum ^= std::to_integer<std::uint8_t>(byte);
      checksum *= 0x01000193U;
    }
    entry->checksum = checksum;
    return PublishResult::Accepted;
  }

  auto diagnostic_checksum(std::string_view sender_id) const noexcept
      -> std::uint64_t override {
    const Entry* entry = find(sender_id);
    return entry == nullptr ? 0 : entry->checksum;
  }

 private:
  struct Entry {
    bool occupied = false;
    std::size_t id_length = 0;
    std::size_t name_length = 0;
    std::array<char, 128> id{};
    std::array<char, 64> name{};
    std::uint32_t checksum = 0;
    bool backpressure_once = false;
  };

  [[nodiscard]] Entry* find(std::string_view sender_id) noexcept {
    if (sender_id.empty() || sender_id.size() > 128) return nullptr;
    for (Entry& entry : entries_) {
      if (entry.occupied && entry.id_length == sender_id.size() &&
          std::string_view(entry.id.data(), entry.id_length) == sender_id) {
        return &entry;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const Entry* find(std::string_view sender_id) const noexcept {
    if (sender_id.empty() || sender_id.size() > 128) return nullptr;
    for (const Entry& entry : entries_) {
      if (entry.occupied && entry.id_length == sender_id.size() &&
          std::string_view(entry.id.data(), entry.id_length) == sender_id) {
        return &entry;
      }
    }
    return nullptr;
  }

  std::array<Entry, kMaximumSenders> entries_{};
};

struct WriteRequest {
  uv_write_t request{};
  Connection *connection = nullptr;
  std::vector<std::byte> bytes;
  bool close_after = false;
  bool websocket_close_write = false;
  bool sensitive = false;
};

struct BasicHttpRequest {
  std::string_view method;
  std::string_view path;
  std::string_view host;
  std::optional<std::string_view> origin;
  std::optional<std::string_view> requested_method;
  bool requested_private_network = false;
};

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
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto l = static_cast<unsigned char>(left[index]);
    const auto r = static_cast<unsigned char>(right[index]);
    if (std::tolower(l) != std::tolower(r)) return false;
  }
  return true;
}

bool valid_header_name(std::string_view name) {
  if (name.empty()) return false;
  for (const unsigned char byte : name) {
    const bool alphanumeric = (byte >= 'A' && byte <= 'Z') ||
                              (byte >= 'a' && byte <= 'z') ||
                              (byte >= '0' && byte <= '9');
    if (!(alphanumeric || byte == '!' || byte == '#' || byte == '$' || byte == '%' ||
          byte == '&' || byte == '\'' || byte == '*' || byte == '+' || byte == '-' ||
          byte == '.' || byte == '^' || byte == '_' || byte == '`' || byte == '|' ||
          byte == '~')) {
      return false;
    }
  }
  return true;
}

bool valid_header_value(std::string_view value) {
  for (const unsigned char byte : value) {
    if ((byte < 0x20 && byte != '\t') || byte == 0x7f) return false;
  }
  return true;
}

std::optional<BasicHttpRequest> parse_basic_http(std::string_view bytes) {
  if (bytes.size() > websocket::kMaximumHttpUpgradeBytes || bytes.size() < 4 ||
      !bytes.ends_with("\r\n\r\n") ||
      bytes.find("\r\n\r\n") != bytes.size() - 4) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (bytes[index] == '\n' && (index == 0 || bytes[index - 1] != '\r')) return std::nullopt;
    if (bytes[index] == '\r' &&
        (index + 1 >= bytes.size() || bytes[index + 1] != '\n')) {
      return std::nullopt;
    }
  }

  const std::size_t first_end = bytes.find("\r\n");
  const std::string_view request_line = bytes.substr(0, first_end);
  const std::size_t first_space = request_line.find(' ');
  const std::size_t second_space = first_space == std::string_view::npos
                                       ? std::string_view::npos
                                       : request_line.find(' ', first_space + 1);
  if (first_space == std::string_view::npos || second_space == std::string_view::npos ||
      request_line.find(' ', second_space + 1) != std::string_view::npos ||
      request_line.substr(second_space + 1) != "HTTP/1.1") {
    return std::nullopt;
  }
  for (const unsigned char byte : request_line) {
    if (byte < 0x20 || byte == 0x7f) return std::nullopt;
  }

  BasicHttpRequest request{
      .method = request_line.substr(0, first_space),
      .path = request_line.substr(first_space + 1, second_space - first_space - 1),
      .host = {},
      .origin = std::nullopt,
      .requested_method = std::nullopt,
      .requested_private_network = false,
  };
  if (request.method.empty() || request.path.empty()) return std::nullopt;

  bool saw_host = false;
  bool saw_origin = false;
  bool saw_requested_method = false;
  bool saw_requested_private_network = false;
  std::size_t position = first_end + 2;
  while (position < bytes.size() - 2) {
    const std::size_t end = bytes.find("\r\n", position);
    if (end == std::string_view::npos) return std::nullopt;
    const std::string_view line = bytes.substr(position, end - position);
    position = end + 2;
    if (line.empty()) break;
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) return std::nullopt;
    const std::string_view name = line.substr(0, colon);
    const std::string_view value = trim(line.substr(colon + 1));
    if (!valid_header_name(name) || !valid_header_value(value)) return std::nullopt;
    if (ascii_iequal(name, "Host")) {
      if (saw_host || value.empty()) return std::nullopt;
      saw_host = true;
      request.host = value;
    } else if (ascii_iequal(name, "Origin")) {
      if (saw_origin || value.empty()) return std::nullopt;
      saw_origin = true;
      request.origin = value;
    } else if (ascii_iequal(name, "Access-Control-Request-Method")) {
      if (saw_requested_method || value.empty()) return std::nullopt;
      saw_requested_method = true;
      request.requested_method = value;
    } else if (ascii_iequal(name, "Access-Control-Request-Private-Network")) {
      if (saw_requested_private_network || value != "true") return std::nullopt;
      saw_requested_private_network = true;
      request.requested_private_network = true;
    }
  }
  return saw_host ? std::optional<BasicHttpRequest>(request) : std::nullopt;
}

std::string random_hex(std::size_t byte_count) {
  if (byte_count == 0 || byte_count > 32) return {};
  std::array<unsigned char, 32> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(byte_count)) != 1) return {};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result(byte_count * 2, '0');
  for (std::size_t index = 0; index < byte_count; ++index) {
    result[index * 2] = kHex[bytes[index] >> 4U];
    result[index * 2 + 1] = kHex[bytes[index] & 0x0fU];
  }
  return result;
}

std::span<const std::byte> as_bytes(std::string_view value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

std::array<std::byte, 2> close_code(std::uint16_t code) {
  return {static_cast<std::byte>((code >> 8U) & 0xffU),
          static_cast<std::byte>(code & 0xffU)};
}

class Server {
 public:
  Server(const ServerOptions& options, FramePublisher& publisher)
      : options_(options), publisher_(publisher), receiver_(publisher) {
    if (options_.pairing_authority != nullptr) {
      authority_worker_ =
          std::make_unique<pairing::AuthorityWorker>(*options_.pairing_authority);
    }
    for (const ProviderCapability& provider : provider_capabilities()) {
      if (provider.available && provider.selected &&
          provider.direction == ProviderDirection::Send) {
        can_send_ = true;
      }
    }
  }

  int run() {
    if (!initialize()) {
      close_initialized_handles();
      if (loop_initialized_) {
        uv_run(&loop_, UV_RUN_DEFAULT);
        uv_loop_close(&loop_);
      }
      return 1;
    }

    // `loopback` reports the stacks actually bound so a host that degraded to
    // IPv4 says so rather than looking identical to a dual-stack daemon.
    std::cout << "{\"type\":\"ready\",\"port\":" << port_
              << ",\"protocolVersions\":[1],\"loopback\":[\"127.0.0.1\""
              << (ipv6_listening_ ? ",\"::1\"" : "") << "],\"instanceId\":\""
              << instance_id_ << "\"}" << std::endl;
    uv_run(&loop_, UV_RUN_DEFAULT);
    const int close_result = uv_loop_close(&loop_);
    if (close_result != 0) {
      std::cerr << "syncd: event loop did not close cleanly: " << uv_strerror(close_result)
                << '\n';
      return 1;
    }
    return 0;
  }

  void accept_connection(uv_stream_t* listener) {
    if (stopping_) return;
    std::size_t slot = kMaximumConnections;
    for (std::size_t index = 0; index < connections_.size(); ++index) {
      if (connections_[index] == nullptr) {
        slot = index;
        break;
      }
    }

    auto* connection = new (std::nothrow) Connection(*this);
    if (connection == nullptr) return;
    if (uv_tcp_init(&loop_, &connection->handle) != 0) {
      delete connection;
      return;
    }
    connection->handle.data = connection;
    if (uv_accept(listener, reinterpret_cast<uv_stream_t*>(&connection->handle)) != 0) {
      uv_close(reinterpret_cast<uv_handle_t*>(&connection->handle), [](uv_handle_t* handle) {
        delete static_cast<Connection*>(handle->data);
      });
      return;
    }
    if (slot == kMaximumConnections) {
      uv_close(reinterpret_cast<uv_handle_t*>(&connection->handle), [](uv_handle_t* handle) {
        delete static_cast<Connection*>(handle->data);
      });
      return;
    }

    connection->slot = slot;
    connections_[slot] = connection;
    set_deadline(*connection, DeadlineKind::HttpHeader, kHttpHeaderDeadlineMs);
    uv_tcp_nodelay(&connection->handle, 1);
    const int read_result = uv_read_start(
        reinterpret_cast<uv_stream_t*>(&connection->handle), allocate_read_buffer, on_read);
    if (read_result != 0) close_connection(*connection);
  }

  void consume(Connection& connection, std::span<const std::byte> bytes) {
    if (connection.closing || connection.transport_close_pending) return;
    if (connection.role != ConnectionRole::Http) {
      consume_websocket(connection, bytes);
      return;
    }

    connection.http_buffer.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::size_t header_end = connection.http_buffer.find("\r\n\r\n");
    // Only the request head counts against the header budget. Bytes past the
    // terminator belong to the WebSocket stream a client may pipeline behind
    // its upgrade, and rejecting those as oversized headers drops valid
    // connections.
    const std::size_t header_bytes =
        header_end == std::string::npos ? connection.http_buffer.size() : header_end + 4;
    if (header_bytes > websocket::kMaximumHttpUpgradeBytes) {
      connection.http_buffer.clear();
      send_http(connection, 431, "Request Header Fields Too Large",
                "{\"error\":\"headers_too_large\"}");
      return;
    }
    if (header_end == std::string::npos) return;
    std::vector<std::byte> remainder;
    remainder.reserve(connection.http_buffer.size() - header_bytes);
    const auto* first = reinterpret_cast<const std::byte*>(connection.http_buffer.data());
    remainder.insert(remainder.end(), first + header_bytes, first + connection.http_buffer.size());
    const std::string header = connection.http_buffer.substr(0, header_bytes);
    connection.http_buffer.clear();
    handle_http(connection, header);
    if (!connection.closing && !connection.transport_close_pending &&
        connection.role != ConnectionRole::Http && !remainder.empty()) {
      consume_websocket(connection, remainder);
    }
  }

  void close_connection(Connection &connection) {
    if (connection.closing)
      return;
    connection.closing = true;
    connection.deadline_kind = DeadlineKind::None;
    connection.deadline_ms = 0;
    invalidate_authority(connection);
    uv_read_stop(reinterpret_cast<uv_stream_t *>(&connection.handle));

    if (connection.role == ConnectionRole::Pairing &&
        connection.pairing_generation != 0 &&
        connection.pairing_generation == active_pairing_generation_) {
      if (options_.pairing_prompt != nullptr) {
        options_.pairing_prompt->cancel(active_pairing_generation_);
      }
      active_pairing_generation_ = 0;
      connection.pairing_generation = 0;
      apply_pairing_cooldown(connection.origin, uv_now(&loop_));
    }

    if (connection.role == ConnectionRole::ControlAuthenticated ||
        connection.role == ConnectionRole::ControlUnauthenticated) {
      remove_owned_senders(connection, false);
    } else if (connection.role == ConnectionRole::Data && connection.sender_slot.has_value()) {
      Sender& sender = senders_[*connection.sender_slot];
      if (sender.occupied && sender.data == &connection) sender.data = nullptr;
    }

    uv_close(reinterpret_cast<uv_handle_t*>(&connection.handle), [](uv_handle_t* handle) {
      auto* closed = static_cast<Connection*>(handle->data);
      closed->server.connection_closed(*closed);
      delete closed;
    });
  }

  void connection_closed(Connection& connection) {
    if (connection.slot < connections_.size() && connections_[connection.slot] == &connection) {
      connections_[connection.slot] = nullptr;
    }
  }

  void begin_shutdown() {
    if (stopping_) return;
    stopping_ = true;
    if (active_pairing_generation_ != 0 && options_.pairing_prompt != nullptr) {
      options_.pairing_prompt->cancel(active_pairing_generation_);
      active_pairing_generation_ = 0;
    }
    close_initialized_handles();
    const auto snapshot = connections_;
    for (Connection* connection : snapshot) {
      if (connection != nullptr) close_connection(*connection);
    }
  }

 private:
  static void on_listener_connection(uv_stream_t* listener, int status) {
    if (status < 0) return;
    static_cast<Server*>(listener->data)->accept_connection(listener);
  }

  static void on_signal(uv_signal_t* signal, int) {
    static_cast<Server*>(signal->data)->begin_shutdown();
  }

  static void on_authority_ready(uv_async_t* handle) {
    auto* server = static_cast<Server*>(handle->data);
    server->poll_pairing_prompt();
    server->poll_authority_results();
  }

  static void on_deadline_sweep(uv_timer_t* timer) {
    auto *server = static_cast<Server *>(timer->data);
    if (server->options_.platform_event_pump != nullptr) {
      server->options_.platform_event_pump(
          server->options_.platform_event_pump_context);
    }
    server->sweep_deadlines();
  }

  static void allocate_read_buffer(uv_handle_t* handle,
                                   std::size_t,
                                   uv_buf_t* buffer) {
    auto* connection = static_cast<Connection*>(handle->data);
    *buffer = uv_buf_init(connection->read_buffer.data(),
                          static_cast<unsigned int>(connection->read_buffer.size()));
  }

  static void on_read(uv_stream_t* stream, ssize_t count, const uv_buf_t* buffer) {
    auto* connection = static_cast<Connection*>(stream->data);
    if (count > 0 && buffer->base != nullptr) {
      try {
        connection->server.consume(
            *connection,
            {reinterpret_cast<const std::byte*>(buffer->base), static_cast<std::size_t>(count)});
      } catch (const std::exception&) {
        connection->server.close_connection(*connection);
      }
    } else if (count < 0) {
      connection->server.close_connection(*connection);
    }
  }

  static void on_write(uv_write_t *request, int status) {
    auto *write = static_cast<WriteRequest *>(request->data);
    Connection *connection = write->connection;
    if (connection->pending_write_bytes >= write->bytes.size()) {
      connection->pending_write_bytes -= write->bytes.size();
    } else {
      connection->pending_write_bytes = 0;
    }
    const bool close_after = write->close_after;
    const bool websocket_close_write = write->websocket_close_write;
    if (write->sensitive && !write->bytes.empty()) {
      OPENSSL_cleanse(write->bytes.data(), write->bytes.size());
    }
    delete write;
    if (status < 0) {
      connection->server.close_connection(*connection);
      return;
    }
    if (websocket_close_write) {
      connection->websocket_close_write_completed = true;
      if (connection->websocket_close_received ||
          connection->force_close_after_websocket_write) {
        connection->server.close_connection(*connection);
        return;
      }
    }
    if (close_after)
      connection->server.close_connection(*connection);
  }

  bool initialize() {
    if (uv_loop_init(&loop_) != 0) {
      std::cerr << "syncd: failed to initialize event loop\n";
      return false;
    }
    loop_initialized_ = true;
    instance_id_ = random_hex(16);
    if (instance_id_.empty()) {
      std::cerr << "syncd: failed to generate instance identifier\n";
      return false;
    }
    welcome_body_ = control::encode_welcome(
        1, kProductVersion, instance_id_, provider_capabilities());
    health_body_ = control::encode_health(
        kProductVersion, instance_id_, provider_capabilities());

    sockaddr_in address4{};
    if (uv_ip4_addr("127.0.0.1", options_.port, &address4) != 0 ||
        uv_tcp_init(&loop_, &listener4_) != 0) {
      std::cerr << "syncd: failed to initialize IPv4 listener\n";
      return false;
    }
    listener4_initialized_ = true;
    listener4_.data = this;
    int result = uv_tcp_bind(&listener4_, reinterpret_cast<const sockaddr*>(&address4), 0);
    if (result != 0) {
      std::cerr << "syncd: failed to bind IPv4 loopback: " << uv_strerror(result) << '\n';
      return false;
    }
    sockaddr_storage selected{};
    int selected_length = sizeof(selected);
    result = uv_tcp_getsockname(&listener4_, reinterpret_cast<sockaddr*>(&selected), &selected_length);
    if (result != 0 || selected.ss_family != AF_INET) {
      std::cerr << "syncd: failed to read selected loopback port\n";
      return false;
    }
    port_ = ntohs(reinterpret_cast<const sockaddr_in*>(&selected)->sin_port);

    // IPv4 loopback is the contract. IPv6 loopback is served when the host
    // offers it, but a host with IPv6 disabled must still get a working daemon
    // rather than a process that refuses to start.
    sockaddr_in6 address6{};
    bool ipv6_ready = uv_ip6_addr("::1", port_, &address6) == 0 &&
                      uv_tcp_init(&loop_, &listener6_) == 0;
    if (ipv6_ready) {
      listener6_initialized_ = true;
      listener6_.data = this;
      result =
          uv_tcp_bind(&listener6_, reinterpret_cast<const sockaddr*>(&address6), UV_TCP_IPV6ONLY);
      if (result != 0) {
        std::cerr << "syncd: IPv6 loopback unavailable, serving IPv4 only: "
                  << uv_strerror(result) << '\n';
        ipv6_ready = false;
      }
    } else {
      std::cerr << "syncd: IPv6 loopback unavailable, serving IPv4 only\n";
    }
    result = uv_listen(reinterpret_cast<uv_stream_t*>(&listener4_), 64, on_listener_connection);
    if (result != 0) {
      std::cerr << "syncd: failed to listen on IPv4 loopback: " << uv_strerror(result) << '\n';
      return false;
    }
    if (ipv6_ready) {
      result = uv_listen(reinterpret_cast<uv_stream_t*>(&listener6_), 64, on_listener_connection);
      if (result != 0) {
        std::cerr << "syncd: IPv6 loopback unavailable, serving IPv4 only: "
                  << uv_strerror(result) << '\n';
        ipv6_ready = false;
      }
    }
    ipv6_listening_ = ipv6_ready;

    if (uv_signal_init(&loop_, &signal_int_) != 0) return false;
    signal_int_initialized_ = true;
    signal_int_.data = this;
    if (uv_signal_start(&signal_int_, on_signal, SIGINT) != 0) return false;
    if (uv_signal_init(&loop_, &signal_term_) != 0) return false;
    signal_term_initialized_ = true;
    signal_term_.data = this;
    if (uv_signal_start(&signal_term_, on_signal, SIGTERM) != 0) return false;
    // Authority results are produced on the worker thread. Waking the loop the
    // moment one lands keeps hello-to-welcome off the sweep interval; the
    // sweep remains as the deadline and prompt-polling path.
    if (uv_async_init(&loop_, &authority_async_, on_authority_ready) != 0) return false;
    authority_async_initialized_ = true;
    authority_async_.data = this;
    uv_unref(reinterpret_cast<uv_handle_t*>(&authority_async_));
    if (authority_worker_ != nullptr) {
      authority_worker_->set_result_notifier(
          [](void* context) noexcept {
            uv_async_send(&static_cast<Server*>(context)->authority_async_);
          },
          this);
    }
    if (uv_timer_init(&loop_, &deadline_timer_) != 0) return false;
    deadline_timer_initialized_ = true;
    deadline_timer_.data = this;
    if (uv_timer_start(&deadline_timer_,
                       on_deadline_sweep,
                       kDeadlineSweepIntervalMs,
                       kDeadlineSweepIntervalMs) != 0) {
      return false;
    }
    return true;
  }

  void close_initialized_handles() {
    // Retire the notifier before the async handle goes away; the worker
    // publishes results under its own lock, so clearing here cannot race a
    // uv_async_send already on its way to a closing handle.
    if (authority_worker_ != nullptr) {
      authority_worker_->set_result_notifier(nullptr, nullptr);
    }
    auto close_handle = [](uv_handle_t* handle) {
      if (!uv_is_closing(handle)) uv_close(handle, nullptr);
    };
    if (authority_async_initialized_) {
      close_handle(reinterpret_cast<uv_handle_t*>(&authority_async_));
    }
    if (listener4_initialized_) close_handle(reinterpret_cast<uv_handle_t*>(&listener4_));
    if (listener6_initialized_) close_handle(reinterpret_cast<uv_handle_t*>(&listener6_));
    if (signal_int_initialized_) {
      uv_signal_stop(&signal_int_);
      close_handle(reinterpret_cast<uv_handle_t*>(&signal_int_));
    }
    if (signal_term_initialized_) {
      uv_signal_stop(&signal_term_);
      close_handle(reinterpret_cast<uv_handle_t*>(&signal_term_));
    }
    if (deadline_timer_initialized_) {
      uv_timer_stop(&deadline_timer_);
      close_handle(reinterpret_cast<uv_handle_t*>(&deadline_timer_));
    }
  }

  bool valid_host(std::string_view host) const {
    const std::string port = std::to_string(port_);
    return host == "127.0.0.1:" + port || host == "[::1]:" + port;
  }

  [[nodiscard]] auto provider_capabilities() const noexcept
      -> std::span<const ProviderCapability> {
    return {options_.providers.data(), options_.provider_count};
  }

  [[nodiscard]] std::size_t active_sender_count() const noexcept {
    std::size_t count = 0;
    for (const Sender& sender : senders_) {
      if (sender.occupied) ++count;
    }
    return count;
  }

  void set_deadline(Connection &connection, DeadlineKind kind,
                    std::uint64_t duration_ms) noexcept {
    connection.deadline_kind = kind;
    connection.deadline_ms = uv_now(&loop_) + duration_ms;
  }

  void clear_deadline(Connection &connection) noexcept {
    connection.deadline_kind = DeadlineKind::None;
    connection.deadline_ms = 0;
  }

  void sweep_deadlines() noexcept {
    const std::uint64_t now = uv_now(&loop_);
    for (Connection *connection : connections_) {
      if (connection == nullptr || connection->closing ||
          connection->deadline_kind == DeadlineKind::None ||
          connection->deadline_ms > now) {
        continue;
      }
      const DeadlineKind expired = connection->deadline_kind;
      clear_deadline(*connection);
      if (expired == DeadlineKind::HttpHeader ||
          expired == DeadlineKind::WebSocketClose) {
        close_connection(*connection);
      } else if (expired == DeadlineKind::ControlHello) {
        try {
          start_websocket_close(*connection, 1008);
        } catch (const std::exception &) {
          close_connection(*connection);
        }
      } else if (expired == DeadlineKind::PairingRequest) {
        apply_pairing_cooldown(connection->origin, now);
        try {
          queue_pairing_error(*connection, "pairing_timeout",
                              "Pairing request timed out");
        } catch (...) {
          close_connection(*connection);
        }
      } else if (expired == DeadlineKind::PairingPrompt) {
        if (connection->authority_state ==
            AuthorityState::PairingIssuePending) {
          invalidate_authority(*connection);
        } else {
          if (connection->pairing_generation == active_pairing_generation_ &&
              options_.pairing_prompt != nullptr) {
            options_.pairing_prompt->cancel(active_pairing_generation_);
            active_pairing_generation_ = 0;
          }
          connection->pairing_generation = 0;
        }
        apply_pairing_cooldown(connection->origin, now);
        try {
          queue_pairing_error(*connection, "pairing_timeout",
                              "Pairing prompt timed out");
        } catch (...) {
          close_connection(*connection);
        }
      }
    }
    poll_pairing_prompt();
    poll_authority_results();
  }

  void queue_pairing_error(Connection &connection, std::string_view code,
                           std::string_view message) {
    const std::string encoded = pairing::encode_error(code, message);
    if (!encoded.empty())
      queue_text(connection, encoded);
    start_websocket_close(connection, 1008);
  }

  void poll_pairing_prompt() noexcept {
    if (options_.pairing_prompt == nullptr)
      return;
    const pairing::PromptResult result = options_.pairing_prompt->poll();
    uv_update_time(&loop_);
    const std::uint64_t observed_at = uv_now(&loop_);
    if (!result.available || result.generation == 0 ||
        result.generation != active_pairing_generation_)
      return;
    Connection *target = nullptr;
    for (Connection *connection : connections_) {
      if (connection != nullptr && !connection->closing &&
          connection->role == ConnectionRole::Pairing &&
          connection->pairing_generation == result.generation) {
        target = connection;
        break;
      }
    }
    active_pairing_generation_ = 0;
    if (target == nullptr)
      return;
    target->pairing_generation = 0;
    if (target->deadline_kind != DeadlineKind::PairingPrompt ||
        target->deadline_ms <= observed_at) {
      clear_deadline(*target);
      apply_pairing_cooldown(target->origin, observed_at);
      try {
        queue_pairing_error(*target, "pairing_timeout",
                            "Pairing prompt timed out");
      } catch (...) {
        close_connection(*target);
      }
      return;
    }
    apply_pairing_cooldown(target->origin, observed_at);
    try {
      if (result.decision == pairing::PromptDecision::Denied) {
        clear_deadline(*target);
        queue_pairing_error(*target, "pairing_denied", "Pairing denied");
        return;
      }
      if (result.decision == pairing::PromptDecision::TimedOut) {
        clear_deadline(*target);
        queue_pairing_error(*target, "pairing_timeout",
                            "Pairing prompt timed out");
        return;
      }
      if (result.decision != pairing::PromptDecision::Approved) {
        clear_deadline(*target);
        queue_pairing_error(*target, "prompt_failure",
                            "Pairing prompt returned an invalid decision");
        return;
      }
      const std::uint64_t generation = next_authority_generation();
      if (authority_worker_ == nullptr ||
          !authority_worker_->submit_issue(generation, target->origin)) {
        clear_deadline(*target);
        queue_pairing_error(*target, "store_failure", "Pairing store failed");
        return;
      }
      target->authority_state = AuthorityState::PairingIssuePending;
      target->authority_generation = generation;
    } catch (...) {
      close_connection(*target);
    }
  }

  [[nodiscard]] std::uint64_t next_authority_generation() noexcept {
    ++next_authority_generation_;
    if (next_authority_generation_ == 0)
      ++next_authority_generation_;
    return next_authority_generation_;
  }

  void invalidate_authority(Connection &connection) noexcept {
    if (connection.authority_generation != 0 && authority_worker_ != nullptr)
      (void)authority_worker_->cancel(connection.authority_generation);
    connection.authority_state = AuthorityState::None;
    connection.authority_generation = 0;
  }

  [[nodiscard]] Connection *find_authority_target(
      const pairing::AuthorityResult &result) noexcept {
    const AuthorityState expected =
        result.operation == pairing::AuthorityOperation::Authenticate
            ? AuthorityState::AuthenticationPending
            : AuthorityState::PairingIssuePending;
    for (Connection *connection : connections_) {
      if (connection != nullptr && !connection->closing &&
          !connection->transport_close_pending &&
          connection->authority_state == expected &&
          connection->authority_generation == result.generation) {
        return connection;
      }
    }
    return nullptr;
  }

  void poll_authority_results() noexcept {
    if (authority_worker_ == nullptr)
      return;
    pairing::AuthorityResult result;
    while (authority_worker_->poll(result)) {
      uv_update_time(&loop_);
      const std::uint64_t observed_at = uv_now(&loop_);
      Connection *target = find_authority_target(result);
      if (target == nullptr)
        continue;
      const AuthorityState state = target->authority_state;
      invalidate_authority(*target);
      const DeadlineKind expected_deadline =
          state == AuthorityState::AuthenticationPending
              ? DeadlineKind::ControlHello
              : DeadlineKind::PairingPrompt;
      if (target->deadline_kind != expected_deadline ||
          target->deadline_ms <= observed_at) {
        continue;
      }
      try {
        if (state == AuthorityState::AuthenticationPending) {
          if (target->role != ConnectionRole::ControlUnauthenticated)
            continue;
          if (result.authentication.error == PairingStoreError::None &&
              result.authentication.authenticated) {
            target->role = ConnectionRole::ControlAuthenticated;
            clear_deadline(*target);
            queue_text(*target, welcome_body_);
          } else {
            queue_error_and_close(*target, "authentication_failed",
                                  "Invalid token or protocol version");
          }
          continue;
        }
        if (target->role != ConnectionRole::Pairing)
          continue;
        clear_deadline(*target);
        PairingIssueResult &issued = result.issuance;
        if (issued.commit == PairingCommitState::CommittedDurable) {
          std::string paired = pairing::encode_paired(1, issued.token.view());
          ScopedStringCleanse paired_cleanser(paired);
          if (paired.empty()) {
            queue_pairing_error(*target, "store_failure",
                                "Pairing store failed");
          } else {
            queue_text(*target, paired, true);
            start_websocket_close(*target, 1000);
          }
        } else if (issued.commit ==
                   PairingCommitState::CommittedDurabilityUncertain) {
          queue_pairing_error(
              *target, "store_durability_uncertain",
              "Pairing committed without durability confirmation");
        } else {
          const auto code = issued.error == PairingStoreError::Capacity
                                ? std::string_view("origin_limit")
                                : std::string_view("store_failure");
          queue_pairing_error(*target, code, "Pairing store failed");
        }
      } catch (...) {
        close_connection(*target);
      }
    }
  }

  [[nodiscard]] bool pairing_cooling_down(const NormalizedOrigin &origin,
                                          std::uint64_t now) const noexcept {
    if (global_pairing_cooldown_until_ > now)
      return true;
    for (const PairingCooldown &cooldown : pairing_cooldowns_) {
      if (cooldown.until_ms > now && cooldown.origin == origin)
        return true;
    }
    return false;
  }

  void apply_pairing_cooldown(const NormalizedOrigin &origin,
                              std::uint64_t now) noexcept {
    if (origin.empty())
      return;
    PairingCooldown *available = nullptr;
    for (PairingCooldown &cooldown : pairing_cooldowns_) {
      if (cooldown.origin == origin) {
        cooldown.until_ms = now + kPairingCooldownMs;
        return;
      }
      if (available == nullptr &&
          (cooldown.origin.empty() || cooldown.until_ms <= now)) {
        available = &cooldown;
      }
    }
    if (available == nullptr) {
      global_pairing_cooldown_until_ = now + kPairingCooldownMs;
      return;
    }
    available->origin = origin;
    available->until_ms = now + kPairingCooldownMs;
  }

  bool queue_write(Connection &connection, std::vector<std::byte> bytes,
                   bool close_after, bool websocket_close_write = false,
                   bool sensitive = false) {
    if (connection.closing || connection.transport_close_pending || bytes.empty() ||
        bytes.size() > kMaximumQueuedWriteBytes - connection.pending_write_bytes) {
      if (sensitive && !bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
      close_connection(connection);
      return false;
    }
    auto* write = new (std::nothrow) WriteRequest;
    if (write == nullptr) {
      if (sensitive) OPENSSL_cleanse(bytes.data(), bytes.size());
      close_connection(connection);
      return false;
    }
    write->connection = &connection;
    write->bytes = std::move(bytes);
    write->close_after = close_after;
    write->websocket_close_write = websocket_close_write;
    write->sensitive = sensitive;
    write->request.data = write;
    uv_buf_t buffer =
        uv_buf_init(reinterpret_cast<char *>(write->bytes.data()),
                    static_cast<unsigned int>(write->bytes.size()));
    connection.pending_write_bytes += write->bytes.size();
    const int result = uv_write(
        &write->request, reinterpret_cast<uv_stream_t *>(&connection.handle),
        &buffer, 1, on_write);
    if (result != 0) {
      connection.pending_write_bytes -= write->bytes.size();
      if (write->sensitive)
        OPENSSL_cleanse(write->bytes.data(), write->bytes.size());
      delete write;
      close_connection(connection);
      return false;
    }
    if (websocket_close_write) {
      connection.websocket_close_sent = true;
      connection.websocket_close_write_completed = false;
      if (!close_after) {
        set_deadline(connection, DeadlineKind::WebSocketClose,
                     kWebSocketCloseDeadlineMs);
      }
    }
    if (close_after) {
      connection.transport_close_pending = true;
      clear_deadline(connection);
      uv_read_stop(reinterpret_cast<uv_stream_t *>(&connection.handle));
    }
    return true;
  }

  bool queue_text(Connection &connection, std::string_view text,
                  bool sensitive = false) {
    return queue_write(
        connection,
        websocket::encode_server_frame(websocket::Opcode::Text, as_bytes(text)),
        false, false, sensitive);
  }

  void start_websocket_close(Connection &connection, std::uint16_t code,
                             std::span<const std::byte> echo_payload = {},
                             bool fail_after_write = false) {
    if (connection.closing || connection.transport_close_pending ||
        connection.websocket_close_sent) {
      return;
    }
    invalidate_authority(connection);
    if (connection.role == ConnectionRole::Pairing &&
        connection.pairing_generation != 0 &&
        connection.pairing_generation == active_pairing_generation_) {
      if (options_.pairing_prompt != nullptr) {
        options_.pairing_prompt->cancel(active_pairing_generation_);
      }
      active_pairing_generation_ = 0;
      connection.pairing_generation = 0;
      apply_pairing_cooldown(connection.origin, uv_now(&loop_));
    }
    if (connection.role == ConnectionRole::ControlAuthenticated ||
        connection.role == ConnectionRole::ControlUnauthenticated) {
      remove_owned_senders(connection, true);
    }
    const auto fallback = close_code(code);
    const auto payload = echo_payload.empty()
                             ? std::span<const std::byte>(fallback)
                             : echo_payload;
    queue_write(
        connection,
        websocket::encode_server_frame(websocket::Opcode::Close, payload),
        fail_after_write, true);
  }

  void queue_error_and_close(Connection& connection,
                             std::string_view code,
                             std::string_view message) {
    if (connection.closing || connection.transport_close_pending ||
        connection.websocket_close_sent) {
      return;
    }
    if (connection.role == ConnectionRole::ControlAuthenticated ||
        connection.role == ConnectionRole::ControlUnauthenticated) {
      remove_owned_senders(connection, true);
    }
    const std::string error = control::encode_error(code, message);
    std::vector<std::byte> output =
        websocket::encode_server_frame(websocket::Opcode::Text, as_bytes(error));
    const auto close_payload = close_code(1008);
    std::vector<std::byte> close =
        websocket::encode_server_frame(websocket::Opcode::Close, close_payload);
    output.insert(output.end(), close.begin(), close.end());
    queue_write(connection, std::move(output), false, true);
  }

  void send_http(Connection& connection,
                 int status,
                 std::string_view reason,
                 std::string_view body,
                 std::optional<std::string_view> cors = std::nullopt) {
    std::string response = "HTTP/1.1 " + std::to_string(status) + " ";
    response.append(reason);
    response.append("\r\nContent-Type: application/json\r\nContent-Length: ");
    response.append(std::to_string(body.size()));
    response.append("\r\nConnection: close\r\n");
    if (cors.has_value()) {
      response.append("Access-Control-Allow-Origin: ");
      response.append(*cors);
      response.append("\r\n");
      response.append("Vary: Origin\r\n");
    }
    response.append("\r\n");
    response.append(body);
    std::vector<std::byte> bytes(response.size());
    std::memcpy(bytes.data(), response.data(), response.size());
    queue_write(connection, std::move(bytes), true);
  }

  void send_preflight(Connection &connection, std::string_view origin,
                      bool private_network) {
    std::string response =
        "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: close\r\n"
        "Access-Control-Allow-Origin: ";
    response.append(origin);
    response.append("\r\nAccess-Control-Allow-Methods: GET\r\n"
                    "Vary: Origin, Access-Control-Request-Method, "
                    "Access-Control-Request-Private-Network\r\n");
    if (private_network)
      response.append("Access-Control-Allow-Private-Network: true\r\n");
    response.append("\r\n");
    std::vector<std::byte> bytes(response.size());
    std::memcpy(bytes.data(), response.data(), response.size());
    queue_write(connection, std::move(bytes), true);
  }

  void handle_http(Connection &connection, std::string_view header) {
    const auto basic = parse_basic_http(header);
    if (!basic.has_value()) {
      send_http(connection, 400, "Bad Request", "{\"error\":\"bad_request\"}");
      return;
    }
    if (!valid_host(basic->host)) {
      send_http(connection, 403, "Forbidden", "{\"error\":\"forbidden\"}");
      return;
    }
    if (basic->path == "/status") {
      if (basic->origin.has_value()) {
        send_http(connection, 403, "Forbidden", "{\"error\":\"forbidden\"}");
        return;
      }
      if (basic->method != "GET" || basic->requested_method.has_value() ||
          basic->requested_private_network) {
        send_http(connection, 405, "Method Not Allowed",
                  "{\"error\":\"method_not_allowed\"}");
        return;
      }
      const std::string status_body = control::encode_status(
          kProductVersion, instance_id_, provider_capabilities(),
          active_sender_count());
      send_http(connection, 200, "OK", status_body);
      return;
    }
    if (basic->path == "/health") {
      if (!basic->origin.has_value()) {
        if (basic->method == "OPTIONS") {
          send_http(connection, 403, "Forbidden", "{\"error\":\"forbidden\"}");
          return;
        }
      } else if (!normalize_origin(*basic->origin).ok()) {
        send_http(connection, 403, "Forbidden", "{\"error\":\"forbidden\"}");
        return;
      }
      if (basic->method == "OPTIONS") {
        if (!basic->origin.has_value() ||
            !basic->requested_method.has_value() ||
            *basic->requested_method != "GET") {
          send_http(connection, 403, "Forbidden", "{\"error\":\"forbidden\"}");
          return;
        }
        send_preflight(connection, *basic->origin,
                       basic->requested_private_network);
        return;
      }
      if (basic->method != "GET" || basic->requested_method.has_value() ||
          basic->requested_private_network) {
        send_http(connection, 405, "Method Not Allowed",
                  "{\"error\":\"method_not_allowed\"}");
        return;
      }
      send_http(connection, 200, "OK", health_body_,
                basic->origin.has_value() ? basic->origin : std::nullopt);
      return;
    }
    if (basic->method != "GET") {
      send_http(connection, 405, "Method Not Allowed",
                "{\"error\":\"method_not_allowed\"}");
      return;
    }

    const auto parsed = websocket::parse_upgrade_request(header);
    if (!parsed.request.has_value()) {
      send_http(connection, 400, "Bad Request", "{\"error\":\"bad_upgrade\"}");
      return;
    }
    const websocket::UpgradeRequest &request = *parsed.request;
    const auto normalized_origin = normalize_origin(request.origin);
    if (!normalized_origin.ok()) {
      send_http(connection, 403, "Forbidden", "{\"error\":\"forbidden\"}");
      return;
    }
    const bool dynamic_pairing = options_.pairing_authority != nullptr && options_.pairing_prompt != nullptr;
    if (!dynamic_pairing && request.origin != options_.allowed_origin) {
      send_http(connection, 403, "Forbidden", "{\"error\":\"forbidden\"}");
      return;
    }
    connection.origin = normalized_origin.origin;
    if (request.path == "/pair") {
      if (!dynamic_pairing || !request.subprotocols.empty()) {
        send_http(connection, 403, "Forbidden", "{\"error\":\"pairing_unavailable\"}");
        return;
      }
      complete_upgrade(connection, request, std::nullopt, ConnectionRole::Pairing);
      return;
    }
    if (request.path == "/control") {
      if (!request.subprotocols.empty()) {
        send_http(connection, 400, "Bad Request", "{\"error\":\"unexpected_subprotocol\"}");
        return;
      }
      complete_upgrade(connection, request, std::nullopt, ConnectionRole::ControlUnauthenticated);
      return;
    }

    Sender* sender = find_sender_by_path(request.path);
    if (sender == nullptr || request.subprotocols.size() != 1 ||
        sender->ticket.empty() || sender->data != nullptr ||
        sender->owner == nullptr || sender->owner->closing ||
        sender->owner->transport_close_pending ||
        sender->owner->websocket_close_sent ||
        sender->owner->websocket_close_received ||
        sender->origin != normalized_origin.origin) {
      send_http(connection, 401, "Unauthorized",
                "{\"error\":\"unauthorized_sender\"}");
      return;
    }
    const std::string expected = "sync.sender." + sender->ticket;
    const std::string_view presented = request.subprotocols.front();
    // The ticket is a bearer credential for this sender's data socket, so the
    // comparison must not leak a prefix match through timing.
    if (presented.size() != expected.size() ||
        CRYPTO_memcmp(presented.data(), expected.data(), expected.size()) != 0) {
      send_http(connection, 401, "Unauthorized",
                "{\"error\":\"unauthorized_sender\"}");
      return;
    }
    const std::size_t sender_index =
        static_cast<std::size_t>(sender - senders_.data());
    if (complete_upgrade(connection, request, expected, ConnectionRole::Data)) {
      sender->ticket.clear();
      sender->data = &connection;
      connection.sender_slot = sender_index;
    }
  }

  bool complete_upgrade(Connection &connection,
                        const websocket::UpgradeRequest &request,
                        std::optional<std::string_view> selected_protocol,
                        ConnectionRole role) {
    const std::string accept = websocket::websocket_accept_key(request.key);
    if (accept.empty()) {
      send_http(connection, 400, "Bad Request", "{\"error\":\"bad_key\"}");
      return false;
    }
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Accept: ";
    response.append(accept);
    response.append("\r\n");
    if (selected_protocol.has_value()) {
      response.append("Sec-WebSocket-Protocol: ");
      response.append(*selected_protocol);
      response.append("\r\n");
    }
    response.append("\r\n");
    std::vector<std::byte> bytes(response.size());
    std::memcpy(bytes.data(), response.data(), response.size());
    const std::size_t maximum_message_bytes =
        role == ConnectionRole::Data      ? kMaximumDataMessageBytes
        : role == ConnectionRole::Pairing ? pairing::kMaximumPairingMessageBytes
                                          : kMaximumControlMessageBytes;
    const websocket::PayloadSensitivity sensitivity =
        role == ConnectionRole::Data
            ? websocket::PayloadSensitivity::NonSensitive
            : websocket::PayloadSensitivity::Sensitive;
    auto decoder = std::make_unique<websocket::ClientFrameDecoder>(
        maximum_message_bytes, sensitivity);
    if (!queue_write(connection, std::move(bytes), false))
      return false;
    connection.role = role;
    connection.decoder = std::move(decoder);
    if (role == ConnectionRole::ControlUnauthenticated) {
      set_deadline(connection, DeadlineKind::ControlHello,
                   kControlHelloDeadlineMs);
    } else if (role == ConnectionRole::Pairing) {
      set_deadline(connection, DeadlineKind::PairingRequest,
                   kPairingRequestDeadlineMs);
    } else {
      clear_deadline(connection);
    }
    return true;
  }

  void consume_websocket(Connection &connection,
                         std::span<const std::byte> bytes) {
    if (connection.decoder == nullptr) {
      close_connection(connection);
      return;
    }
    std::vector<websocket::Message> messages;
    const websocket::PayloadSensitivity sensitivity =
        connection.role == ConnectionRole::Data
            ? websocket::PayloadSensitivity::NonSensitive
            : websocket::PayloadSensitivity::Sensitive;
    websocket::MessagePayloadGuard message_payload_guard(messages,
                                                          sensitivity);
    const websocket::DecodeError error =
        connection.decoder->feed(bytes, messages);
    if (error != websocket::DecodeError::None) {
      connection.decoder_terminal = true;
      if (connection.role == ConnectionRole::Pairing &&
          connection.pairing_generation != 0 &&
          connection.pairing_generation == active_pairing_generation_) {
        if (options_.pairing_prompt != nullptr) {
          options_.pairing_prompt->cancel(active_pairing_generation_);
        }
        active_pairing_generation_ = 0;
        connection.pairing_generation = 0;
        apply_pairing_cooldown(connection.origin, uv_now(&loop_));
      }
      if (connection.role == ConnectionRole::Pairing &&
          error == websocket::DecodeError::MessageTooLarge &&
          !connection.websocket_close_sent) {
        queue_pairing_error(connection, "request_too_large",
                            "Pairing request exceeds 1024 bytes");
        return;
      }
      if (connection.websocket_close_sent) {
        connection.force_close_after_websocket_write = true;
        if (connection.websocket_close_write_completed)
          close_connection(connection);
      } else {
        // RFC 6455 section 7.4.1: inconsistent payload data closes 1007, every
        // other framing violation closes 1002.
        const std::uint16_t code =
            error == websocket::DecodeError::InvalidTextPayload ? 1007 : 1002;
        start_websocket_close(connection, code, {}, true);
      }
      return;
    }
    for (websocket::Message &message : messages) {
      if (connection.closing || connection.transport_close_pending ||
          connection.websocket_close_received) {
        return;
      }
      if (message.opcode == websocket::Opcode::Close) {
        handle_peer_close(connection, message.payload);
        return;
      }
      if (connection.websocket_close_sent) {
        continue;
      }
      if (message.opcode == websocket::Opcode::Ping) {
        queue_write(connection,
                    websocket::encode_server_frame(websocket::Opcode::Pong,
                                                   message.payload),
                    false);
      } else if (message.opcode == websocket::Opcode::Pong) {
        continue;
      } else if (connection.role == ConnectionRole::ControlUnauthenticated ||
                 connection.role == ConnectionRole::ControlAuthenticated) {
        if (message.opcode != websocket::Opcode::Text) {
          start_websocket_close(connection, 1003);
        } else {
          handle_control_message(connection, message.payload);
        }
      } else if (connection.role == ConnectionRole::Pairing) {
        if (message.opcode != websocket::Opcode::Text) {
          start_websocket_close(connection, 1003);
        } else {
          handle_pairing_message(connection, message.payload);
        }
      } else if (connection.role == ConnectionRole::Data) {
        if (message.opcode != websocket::Opcode::Binary ||
            !connection.sender_slot.has_value()) {
          start_websocket_close(connection, 1003);
        } else {
          handle_data_message(connection, message.payload);
          connection.decoder->recycle_payload(message.payload);
        }
      }
    }
  }

  void handle_peer_close(Connection &connection,
                         std::span<const std::byte> payload) {
    if (connection.websocket_close_received)
      return;
    connection.websocket_close_received = true;
    invalidate_authority(connection);
    if (connection.role == ConnectionRole::Pairing &&
        connection.pairing_generation != 0 &&
        connection.pairing_generation == active_pairing_generation_) {
      if (options_.pairing_prompt != nullptr)
        options_.pairing_prompt->cancel(active_pairing_generation_);
      active_pairing_generation_ = 0;
      connection.pairing_generation = 0;
      apply_pairing_cooldown(connection.origin, uv_now(&loop_));
    }
    if (connection.role == ConnectionRole::ControlAuthenticated ||
        connection.role == ConnectionRole::ControlUnauthenticated) {
      remove_owned_senders(connection, true);
    }
    if (connection.websocket_close_sent) {
      if (connection.websocket_close_write_completed)
        close_connection(connection);
      return;
    }
    queue_write(
        connection,
        websocket::encode_server_frame(websocket::Opcode::Close, payload),
        false, true);
  }

  void handle_control_message(Connection &connection,
                              std::span<std::byte> payload) {
    ScopedByteCleanse payload_cleanser(payload);
    const std::string_view json(reinterpret_cast<const char *>(payload.data()),
                                payload.size());
    control::ParseResult parsed = control::parse_message(json);
    if (connection.role == ConnectionRole::ControlUnauthenticated) {
      if (connection.authority_state ==
          AuthorityState::AuthenticationPending) {
        if (parsed.message.has_value())
          parsed.message->clear_sensitive();
        return;
      }
      bool supports_v1 = false;
      if (parsed.message.has_value() &&
          parsed.message->type == control::MessageType::Hello) {
        supports_v1 = std::find(parsed.message->protocol_versions.begin(),
                                parsed.message->protocol_versions.end(),
                                1) != parsed.message->protocol_versions.end();
      }
      bool authenticated = false;
      if (parsed.message.has_value() &&
          parsed.message->type == control::MessageType::Hello && supports_v1) {
        if (options_.pairing_authority != nullptr) {
          const std::uint64_t generation = next_authority_generation();
          const bool submitted = authority_worker_ != nullptr &&
                                 authority_worker_->submit_authenticate(
                                     generation, connection.origin,
                                     parsed.message->token);
          parsed.message->clear_sensitive();
          if (!submitted) {
            queue_error_and_close(connection, "authentication_failed",
                                  "Invalid token or protocol version");
            return;
          }
          connection.authority_state = AuthorityState::AuthenticationPending;
          connection.authority_generation = generation;
          return;
        } else {
          authenticated = parsed.message->token == options_.test_token;
        }
      }
      if (!authenticated) {
        if (parsed.message.has_value())
          parsed.message->clear_sensitive();
        queue_error_and_close(connection, "authentication_failed",
                              "Invalid token or protocol version");
        return;
      }
      parsed.message->clear_sensitive();
      connection.role = ConnectionRole::ControlAuthenticated;
      clear_deadline(connection);
      queue_text(connection, welcome_body_);
      return;
    }

    if (!parsed.message.has_value()) {
      queue_text(connection, control::encode_error(
                                 "bad_request", "Malformed control message"));
      return;
    }
    const control::ControlMessage &message = *parsed.message;
    switch (message.type) {
    case control::MessageType::Hello:
      queue_text(connection, control::encode_error(
                                 "out_of_order",
                                 "Hello is only valid as the first message"));
      break;
    case control::MessageType::CreateSender:
      create_sender(connection, message.name);
      break;
    case control::MessageType::GetStats:
      send_stats(connection, message.sender_id);
      break;
    case control::MessageType::CloseSender:
      close_sender(connection, message.sender_id);
      break;
    }
  }

  void handle_pairing_message(Connection &connection,
                              std::span<const std::byte> payload) {
    if (connection.pairing_message_received) {
      queue_pairing_error(connection, "duplicate_request",
                          "Only one pairing request is allowed");
      return;
    }
    connection.pairing_message_received = true;
    const std::string_view json(reinterpret_cast<const char *>(payload.data()),
                                payload.size());
    const pairing::ParseResult parsed = pairing::parse_request(json);
    if (!parsed.ok()) {
      queue_pairing_error(connection, "bad_request",
                          "Malformed pairing request");
      return;
    }
    const std::uint64_t now = uv_now(&loop_);
    if (pairing_cooling_down(connection.origin, now)) {
      queue_pairing_error(connection, "pairing_cooldown",
                          "Pairing is cooling down");
      return;
    }
    if (active_pairing_generation_ != 0) {
      apply_pairing_cooldown(connection.origin, now);
      queue_pairing_error(connection, "prompt_saturated", "Another pairing prompt is active");
      return;
    }
    ++next_pairing_generation_;
    if (next_pairing_generation_ == 0) ++next_pairing_generation_;
    pairing::PromptRequest request;
    if (!request.assign(next_pairing_generation_, connection.origin, parsed.request.name()) ||
        options_.pairing_prompt == nullptr || !options_.pairing_prompt->begin(request)) {
      apply_pairing_cooldown(connection.origin, now);
      queue_pairing_error(connection, "prompt_saturated", "Pairing prompt is unavailable");
      return;
    }
    active_pairing_generation_ = next_pairing_generation_;
    connection.pairing_generation = active_pairing_generation_;
    set_deadline(connection, DeadlineKind::PairingPrompt, kPairingPromptDeadlineMs);
  }

  void handle_data_message(Connection& connection, std::span<const std::byte> payload) {
    Sender& sender = senders_[*connection.sender_slot];
    if (!sender.occupied || sender.data != &connection) {
      start_websocket_close(connection, 1008);
      return;
    }
    const ReceiveResult result = receiver_.receive(sender.id, payload);
    if (result.status == ReceiveStatus::RejectedMalformed ||
        result.status == ReceiveStatus::RejectedSender) {
      start_websocket_close(connection, 1002);
    } else if (result.status == ReceiveStatus::PublishFailed) {
      start_websocket_close(connection, 1011);
    }
  }

  void create_sender(Connection& owner, std::string_view name) {
    if (!can_send_) {
      queue_text(owner,
                 control::encode_error("publisher_unavailable", "Publisher is unavailable"));
      return;
    }
    for (const Sender& sender : senders_) {
      if (sender.occupied && sender.owner == &owner && sender.name == name) {
        queue_text(owner, control::encode_error("duplicate_sender", "Sender name already exists"));
        return;
      }
    }
    Sender* target = nullptr;
    for (Sender& sender : senders_) {
      if (!sender.occupied) {
        target = &sender;
        break;
      }
    }
    if (target == nullptr) {
      queue_text(owner, control::encode_error("sender_limit", "Sender limit reached"));
      return;
    }

    std::string id;
    for (int attempt = 0; attempt < 8; ++attempt) {
      id = random_hex(16);
      if (!id.empty() && find_sender(id) == nullptr) break;
      id.clear();
    }
    if (id.empty()) {
      queue_text(owner, control::encode_error("internal_error", "Random generation failed"));
      return;
    }

    std::string owned_name(name);
    if (!publisher_.open_sender(id, owned_name)) {
      queue_text(owner,
                 control::encode_error("publisher_unavailable", "Publisher is unavailable"));
      return;
    }

    std::string ticket = random_hex(16);
    if (ticket.empty()) {
      publisher_.close_sender(id);
      queue_text(owner, control::encode_error("internal_error", "Random generation failed"));
      return;
    }

    target->id = std::move(id);
    target->name = std::move(owned_name);
    target->ticket = std::move(ticket);
    target->owner = &owner;
    target->data = nullptr;
    target->occupied = true;
    target->origin = owner.origin;
    const std::string path = "/senders/" + target->id;
    queue_text(owner,
               control::encode_sender_created(target->id, target->name, path, target->ticket));
  }

  void send_stats(Connection& owner, std::string_view sender_id) {
    Sender* sender = find_sender(sender_id);
    if (sender == nullptr || sender->owner != &owner) {
      queue_text(owner, control::encode_error("sender_not_found", "Sender does not exist"));
      return;
    }
    control::SenderStatsPayload output;
    if (const SenderStats* stats = receiver_.stats(sender->id)) {
      output.accepted = stats->accepted;
      output.dropped = stats->dropped;
      output.rejected = stats->rejected;
      output.failed = stats->failed;
      output.last_sequence = stats->last_sequence;
      output.last_presentation_time_us = stats->last_presentation_time_us;
    }
    output.checksum = publisher_.diagnostic_checksum(sender->id);
    queue_text(owner, control::encode_stats(sender->id, output));
  }

  void close_sender(Connection& owner, std::string_view sender_id) {
    Sender* sender = find_sender(sender_id);
    if (sender == nullptr || sender->owner != &owner) {
      queue_text(owner, control::encode_error("sender_not_found", "Sender does not exist"));
      return;
    }
    const std::string id = sender->id;
    const std::size_t slot = static_cast<std::size_t>(sender - senders_.data());
    remove_sender(slot, true);
    queue_text(owner, control::encode_sender_closed(id));
  }

  void remove_sender(std::size_t slot, bool graceful_data_close) {
    Sender& sender = senders_[slot];
    if (!sender.occupied) return;
    Connection* data = sender.data;
    (void)receiver_.remove_sender(sender.id);
    publisher_.close_sender(sender.id);
    sender.occupied = false;
    sender.id.clear();
    sender.name.clear();
    sender.ticket.clear();
    sender.owner = nullptr;
    sender.data = nullptr;
    sender.origin = {};
    if (data != nullptr && !data->closing) {
      if (graceful_data_close) {
        try {
          start_websocket_close(*data, 1000);
        } catch (const std::exception&) {
          close_connection(*data);
        }
      } else {
        close_connection(*data);
      }
    }
  }

  void remove_owned_senders(Connection& owner, bool graceful_data_close) {
    for (std::size_t index = 0; index < senders_.size(); ++index) {
      if (senders_[index].occupied && senders_[index].owner == &owner) {
        remove_sender(index, graceful_data_close);
      }
    }
  }

  Sender *find_sender(std::string_view id) {
    for (Sender &sender : senders_) {
      if (sender.occupied && sender.id == id)
        return &sender;
    }
    return nullptr;
  }

  Sender *find_sender_by_path(std::string_view path) {
    constexpr std::string_view prefix = "/senders/";
    if (!path.starts_with(prefix) || path.size() == prefix.size() ||
        path.find('?') != path.npos) {
      return nullptr;
    }
    return find_sender(path.substr(prefix.size()));
  }

  ServerOptions options_;
  uv_loop_t loop_{};
  uv_tcp_t listener4_{};
  uv_tcp_t listener6_{};
  uv_signal_t signal_int_{};
  uv_signal_t signal_term_{};
  uv_timer_t deadline_timer_{};
  uv_async_t authority_async_{};
  bool authority_async_initialized_ = false;
  bool loop_initialized_ = false;
  bool listener4_initialized_ = false;
  bool listener6_initialized_ = false;
  bool signal_int_initialized_ = false;
  bool signal_term_initialized_ = false;
  bool deadline_timer_initialized_ = false;
  bool stopping_ = false;
  bool ipv6_listening_ = false;
  std::uint16_t port_ = 0;
  std::string instance_id_;
  std::array<Connection *, kMaximumConnections> connections_{};
  std::array<Sender, kMaximumSenders> senders_{};
  FramePublisher &publisher_;
  FrameReceiver receiver_;
  std::unique_ptr<pairing::AuthorityWorker> authority_worker_;
  bool can_send_ = false;
  std::string welcome_body_;
  std::string health_body_;
  std::uint64_t next_pairing_generation_ = 0;
  std::uint64_t active_pairing_generation_ = 0;
  std::uint64_t next_authority_generation_ = 0;
  std::array<PairingCooldown, kMaximumPairingCooldowns> pairing_cooldowns_{};
  std::uint64_t global_pairing_cooldown_until_ = 0;
};

bool valid_server_configuration(const ServerOptions& options,
                                const FramePublisher* external_publisher) noexcept {
  const bool dynamic_pairing = options.pairing_authority != nullptr || options.pairing_prompt != nullptr;
  if ((options.pairing_authority == nullptr) != (options.pairing_prompt == nullptr) ||
      (dynamic_pairing && (!options.allowed_origin.empty() || !options.test_token.empty())) ||
      (!dynamic_pairing && (options.allowed_origin.empty() || options.test_token.empty()))) {
    return false;
  }
  if ((options.test_receiver && external_publisher != nullptr) ||
      (!options.test_receiver && external_publisher == nullptr) ||
      options.provider_count == 0 ||
      options.provider_count > kMaximumProviderCapabilities) {
    return false;
  }

  for (std::size_t index = 0; index < options.provider_count; ++index) {
    const ProviderCapability& provider = options.providers[index];
    if (provider.id.empty() || provider.id.size() > kMaximumProviderIdBytes ||
        (provider.direction != ProviderDirection::Send &&
         provider.direction != ProviderDirection::Receive)) {
      return false;
    }
    for (std::size_t prior = 0; prior < index; ++prior) {
      if (options.providers[prior].id == provider.id) return false;
    }
  }
  return true;
}

} // namespace

int run_server(const ServerOptions& options, FramePublisher* publisher) {
  try {
    if (!valid_server_configuration(options, publisher)) {
      std::cerr << "syncd: invalid publisher or provider configuration\n";
      return 1;
    }
    if (options.test_receiver) {
      TestPublisher test_publisher;
      Server server(options, test_publisher);
      return server.run();
    }
    Server server(options, *publisher);
    return server.run();
  } catch (const std::exception& error) {
    std::cerr << "syncd: fatal error: " << error.what() << '\n';
    return 1;
  }
}

} // namespace noisefactor::sync
