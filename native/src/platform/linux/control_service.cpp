#include <sync/platform/linux_control_service.hpp>

#include <sync/platform/linux_control_protocol.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace noisefactor::sync::linux_control {
namespace {

constexpr std::size_t kMaximumProviders = 4;
constexpr std::size_t kMaximumClients = 8;

template <std::size_t Size>
void copy_text(std::array<char, Size>& output, std::string_view input) noexcept {
  output.fill('\0');
  const std::size_t length = std::min(input.size(), Size - 1);
  std::copy_n(input.data(), length, output.data());
}

std::string_view array_text(const auto& value) noexcept {
  const auto end = std::find(value.begin(), value.end(), '\0');
  return {value.data(), static_cast<std::size_t>(end - value.begin())};
}

std::string error_json(std::string_view code) {
  return "{\"version\":1,\"type\":\"error\",\"code\":" +
         encode_linux_control_json_string(code) + "}";
}

std::string prompt_json(const pairing::PromptRequest& request) {
  return "{\"version\":1,\"type\":\"prompt\",\"generation\":" +
         std::to_string(request.generation) + ",\"origin\":" +
         encode_linux_control_json_string(request.origin.view()) +
         ",\"name\":" + encode_linux_control_json_string(request.name()) +
         ",\"deadlineMs\":30000}";
}

std::string status_json(const LinuxRuntimeStatusSource& source) {
  std::array<LinuxProviderStatus, kMaximumProviders> providers{};
  const std::size_t count = source.providers(providers);
  const DaemonMetricsSnapshot metrics = source.metrics();
  const std::uint64_t now_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  std::string output = "{\"version\":1,\"type\":\"status\",\"providers\":[";
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) output.push_back(',');
    const auto& provider = providers[index];
    output += "{\"id\":" + encode_linux_control_json_string(array_text(provider.id)) +
              ",\"selected\":" + (provider.selected ? "true" : "false") +
              ",\"available\":" + (provider.available ? "true" : "false") +
              ",\"healthy\":" + (provider.healthy ? "true" : "false") +
              ",\"reason\":" + encode_linux_control_json_string(array_text(provider.reason)) +
              "}";
  }
  output += "],\"metrics\":{";
  const std::array fields = {
      std::pair{"receivedFrames", metrics.received_frames},
      std::pair{"acceptedFrames", metrics.accepted_frames},
      std::pair{"droppedFrames", metrics.dropped_frames},
      std::pair{"rejectedFrames", metrics.rejected_frames},
      std::pair{"failedFrames", metrics.failed_frames},
      std::pair{"cameraDrivingFrames", metrics.camera_driving_frames},
      std::pair{"cameraWrites", metrics.camera_writes},
      std::pair{"cameraQueueReplacements", metrics.camera_queue_replacements},
      std::pair{"cameraBackpressureDrops", metrics.camera_backpressure_drops},
      std::pair{"cameraWriteFailures", metrics.camera_write_failures},
      std::pair{"cameraReopenAttempts", metrics.camera_reopen_attempts},
      std::pair{"cameraIdleFrames", metrics.camera_idle_frames},
      std::pair{"cameraLastWriteMs", metrics.camera_last_write_ms},
  };
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) output.push_back(',');
    output += "\"" + std::string(fields[index].first) + "\":" +
              std::to_string(fields[index].second);
  }
  output += ",\"cameraSlotCount\":3,\"cameraLastWriteAgeMs\":";
  if (metrics.camera_last_write_ms == 0) {
    output += "null";
  } else {
    output += std::to_string(now_ms >= metrics.camera_last_write_ms
                                 ? now_ms - metrics.camera_last_write_ms
                                 : 0);
  }
  output += "}}";
  return output;
}

std::string pairing_store_error(PairingStoreError error) {
  switch (error) {
    case PairingStoreError::None: return "none";
    case PairingStoreError::InvalidPath: return "invalid_path";
    case PairingStoreError::DirectorySecurity: return "directory_security";
    case PairingStoreError::FileSecurity: return "file_security";
    case PairingStoreError::Io: return "io";
    case PairingStoreError::Corrupt: return "corrupt";
    case PairingStoreError::UnknownVersion: return "unknown_version";
    case PairingStoreError::Capacity: return "capacity";
    case PairingStoreError::RandomFailure: return "random_failure";
    case PairingStoreError::InvalidToken: return "invalid_token";
    case PairingStoreError::Busy: return "busy";
    case PairingStoreError::Canceled: return "canceled";
  }
  return "unknown";
}

std::uint32_t kernel_peer_uid(int descriptor) noexcept {
  ucred credentials{};
  socklen_t length = sizeof(credentials);
  if (::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials,
                   &length) != 0 || length != sizeof(credentials)) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  return static_cast<std::uint32_t>(credentials.uid);
}

}  // namespace

struct LinuxRuntimeStatus::State {
  mutable std::mutex mutex;
  std::array<LinuxProviderStatus, kMaximumProviders> providers{};
  std::size_t count = 0;
};

LinuxRuntimeStatus::LinuxRuntimeStatus(const DaemonMetrics& metrics) noexcept
    : state_(new (std::nothrow) State()), metrics_(metrics) {}

LinuxRuntimeStatus::~LinuxRuntimeStatus() noexcept = default;

void LinuxRuntimeStatus::set_provider(std::string_view id, bool selected,
                                      bool available, bool healthy,
                                      std::string_view reason) noexcept {
  if (state_ == nullptr || id.empty() || id.size() >= 16 ||
      reason.size() >= 160) {
    return;
  }
  std::lock_guard lock(state_->mutex);
  std::size_t index = state_->count;
  for (std::size_t candidate = 0; candidate < state_->count; ++candidate) {
    if (array_text(state_->providers[candidate].id) == id) {
      index = candidate;
      break;
    }
  }
  if (index == state_->count) {
    if (state_->count == state_->providers.size()) return;
    ++state_->count;
  }
  LinuxProviderStatus value{};
  copy_text(value.id, id);
  value.selected = selected;
  value.available = available;
  value.healthy = healthy;
  copy_text(value.reason, reason);
  state_->providers[index] = value;
}

auto LinuxRuntimeStatus::providers(
    std::span<LinuxProviderStatus> output) const noexcept -> std::size_t {
  if (state_ == nullptr) return 0;
  std::lock_guard lock(state_->mutex);
  const std::size_t count = std::min(output.size(), state_->count);
  std::copy_n(state_->providers.begin(), count, output.begin());
  return count;
}

auto LinuxRuntimeStatus::metrics() const noexcept -> DaemonMetricsSnapshot {
  return metrics_.snapshot();
}

struct LinuxControlService::Impl {
  struct Client {
    int descriptor = -1;
    std::array<std::byte, 4> prefix{};
    std::size_t prefix_size = 0;
    std::vector<std::byte> payload;
    std::size_t payload_size = 0;
    std::vector<std::byte> output;
    std::size_t output_offset = 0;
    bool close_after_output = false;
    bool pair_owner = false;
    bool prompt_sent = false;
  };

  explicit Impl(Options value) : options(value) {
    runtime_directory.assign(value.runtime_directory);
  }

  Options options;
  std::string runtime_directory;
  std::string socket_path;
  int listener = -1;
  std::array<int, 2> wake_pipe{{-1, -1}};
  std::thread worker;
  std::atomic<bool> stopping{false};
  bool started = false;
  dev_t socket_device = 0;
  ino_t socket_inode = 0;

  std::mutex prompt_mutex;
  pairing::PromptRequest active_request{};
  bool active_prompt = false;
  pairing::PromptResult prompt_result{};
  std::uint64_t canceled_generation = 0;
  int pair_owner_descriptor = -1;

  void wake() noexcept {
    if (wake_pipe[1] < 0) return;
    const std::byte value{1};
    const auto ignored = ::write(wake_pipe[1], &value, 1);
    (void)ignored;
  }

  void queue(Client& client, std::string json, bool close_after) {
    client.output = encode_linux_control_frame(json);
    client.output_offset = 0;
    client.close_after_output = close_after;
  }

  void deny_owner(int descriptor) noexcept {
    std::lock_guard lock(prompt_mutex);
    if (pair_owner_descriptor != descriptor) return;
    pair_owner_descriptor = -1;
    if (active_prompt) {
      prompt_result = {.available = true,
                       .generation = active_request.generation,
                       .decision = pairing::PromptDecision::Denied};
      active_prompt = false;
    }
  }

  void close_client(Client& client) noexcept {
    deny_owner(client.descriptor);
    if (client.descriptor >= 0) ::close(client.descriptor);
    client.descriptor = -1;
  }

  void reset_input(Client& client) {
    client.prefix_size = 0;
    client.payload.clear();
    client.payload_size = 0;
  }

  void handle_request(Client& client, std::string_view json) {
    const auto decoded = decode_linux_control_request(json);
    if (!decoded.valid) {
      queue(client, error_json(decoded.error_code), true);
      return;
    }
    const auto& request = decoded.request;
    if (client.pair_owner && request.command != LinuxControlCommand::Decision) {
      queue(client, error_json("invalid_pair_state"), true);
      return;
    }
    switch (request.command) {
      case LinuxControlCommand::Pair: {
        std::lock_guard lock(prompt_mutex);
        if (pair_owner_descriptor >= 0) {
          queue(client, error_json("prompt_client_busy"), true);
          return;
        }
        pair_owner_descriptor = client.descriptor;
        client.pair_owner = true;
        reset_input(client);
        return;
      }
      case LinuxControlCommand::Decision: {
        std::lock_guard lock(prompt_mutex);
        if (!client.pair_owner || !client.prompt_sent || !active_prompt ||
            request.generation != active_request.generation) {
          queue(client, error_json("stale_generation"), true);
          return;
        }
        prompt_result = {
            .available = true,
            .generation = active_request.generation,
            .decision = request.approved ? pairing::PromptDecision::Approved
                                         : pairing::PromptDecision::Denied,
        };
        active_prompt = false;
        pair_owner_descriptor = -1;
        client.pair_owner = false;
        queue(client,
              "{\"version\":1,\"type\":\"decision\",\"status\":\"accepted\"}",
              true);
        return;
      }
      case LinuxControlCommand::Status:
        queue(client, status_json(*options.status), true);
        return;
      case LinuxControlCommand::Doctor:
        queue(client, error_json("command_not_built"), true);
        return;
      case LinuxControlCommand::Pairings: {
        std::array<NormalizedOrigin, kMaximumPairingOrigins> origins{};
        const PairingListResult result = options.management->list(origins);
        if (result.error != PairingStoreError::None ||
            result.count > origins.size()) {
          queue(client, error_json(pairing_store_error(result.error)), true);
          return;
        }
        std::string response =
            "{\"version\":1,\"type\":\"pairings\",\"origins\":[";
        for (std::size_t index = 0; index < result.count; ++index) {
          if (index != 0) response.push_back(',');
          response += encode_linux_control_json_string(origins[index].view());
        }
        response += "]}";
        queue(client, std::move(response), true);
        return;
      }
      case LinuxControlCommand::Revoke: {
        const PairingRevocationResult result =
            options.management->revoke(request.origin);
        if (result.error != PairingStoreError::None) {
          queue(client, error_json(pairing_store_error(result.error)), true);
          return;
        }
        const std::string_view state =
            result.commit == PairingCommitState::CommittedDurabilityUncertain
                ? "durability_uncertain"
                : (result.revoked ? "revoked" : "not_found");
        queue(client,
              "{\"version\":1,\"type\":\"revocation\",\"origin\":" +
                  encode_linux_control_json_string(request.origin.view()) +
                  ",\"status\":" + encode_linux_control_json_string(state) +
                  "}",
              true);
        return;
      }
    }
  }

  bool read_client(Client& client) {
    while (true) {
      if (client.prefix_size < client.prefix.size()) {
        const auto received = ::recv(
            client.descriptor, client.prefix.data() + client.prefix_size,
            client.prefix.size() - client.prefix_size, 0);
        if (received == 0) return false;
        if (received < 0) {
          return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        }
        client.prefix_size += static_cast<std::size_t>(received);
        if (client.prefix_size < client.prefix.size()) continue;
        const std::uint32_t length =
            (std::to_integer<std::uint32_t>(client.prefix[0]) << 24U) |
            (std::to_integer<std::uint32_t>(client.prefix[1]) << 16U) |
            (std::to_integer<std::uint32_t>(client.prefix[2]) << 8U) |
            std::to_integer<std::uint32_t>(client.prefix[3]);
        if (length == 0 || length > kMaximumLinuxControlMessageBytes) {
          queue(client, error_json("invalid_length"), true);
          return true;
        }
        client.payload.resize(length);
      }
      const auto received = ::recv(
          client.descriptor, client.payload.data() + client.payload_size,
          client.payload.size() - client.payload_size, 0);
      if (received == 0) return false;
      if (received < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
      }
      client.payload_size += static_cast<std::size_t>(received);
      if (client.payload_size < client.payload.size()) continue;
      const std::string_view message(
          reinterpret_cast<const char*>(client.payload.data()),
          client.payload.size());
      handle_request(client, message);
      return true;
    }
  }

  bool write_client(Client& client) noexcept {
    while (client.output_offset < client.output.size()) {
      const auto written =
          ::send(client.descriptor, client.output.data() + client.output_offset,
                 client.output.size() - client.output_offset, MSG_NOSIGNAL);
      if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
          return true;
        }
        return false;
      }
      if (written == 0) return false;
      client.output_offset += static_cast<std::size_t>(written);
    }
    client.output.clear();
    client.output_offset = 0;
    return !client.close_after_output;
  }

  void reconcile_prompt(std::vector<Client>& clients) {
    std::lock_guard lock(prompt_mutex);
    for (auto& client : clients) {
      if (client.descriptor != pair_owner_descriptor) continue;
      if (canceled_generation != 0) {
        client.pair_owner = false;
        pair_owner_descriptor = -1;
        queue(client, error_json("prompt_canceled"), true);
        canceled_generation = 0;
      } else if (active_prompt && !client.prompt_sent &&
                 client.output.empty()) {
        client.prompt_sent = true;
        queue(client, prompt_json(active_request), false);
      }
      break;
    }
  }

  void accept_clients(std::vector<Client>& clients) {
    while (true) {
      const int descriptor =
          ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (descriptor < 0) {
        if (errno == EINTR) continue;
        return;
      }
      const auto credential =
          options.peer_uid != nullptr ? options.peer_uid(descriptor)
                                      : kernel_peer_uid(descriptor);
      if (clients.size() == kMaximumClients) {
        const auto response = encode_linux_control_frame(
            error_json("too_many_clients"));
        const auto ignored = ::send(descriptor, response.data(), response.size(),
                                    MSG_NOSIGNAL);
        (void)ignored;
        ::close(descriptor);
        continue;
      }
      Client client;
      client.descriptor = descriptor;
      if (credential != options.expected_uid) {
        queue(client, error_json("peer_not_authorized"), true);
      }
      clients.push_back(std::move(client));
    }
  }

  void run() noexcept {
    std::vector<Client> clients;
    clients.reserve(kMaximumClients);
    while (!stopping.load(std::memory_order_acquire)) {
      reconcile_prompt(clients);
      std::array<pollfd, kMaximumClients + 2> descriptors{};
      descriptors[0] = {.fd = listener, .events = POLLIN, .revents = 0};
      descriptors[1] = {
          .fd = wake_pipe[0], .events = POLLIN, .revents = 0};
      for (std::size_t index = 0; index < clients.size(); ++index) {
        descriptors[index + 2] = {
            .fd = clients[index].descriptor,
            .events = static_cast<short>(POLLIN |
                                         (clients[index].output.empty()
                                              ? 0
                                              : POLLOUT)),
            .revents = 0,
        };
      }
      const std::size_t polled_client_count = clients.size();
      const int ready = ::poll(descriptors.data(), polled_client_count + 2, -1);
      if (ready < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if ((descriptors[1].revents & POLLIN) != 0) {
        std::array<std::byte, 64> discarded{};
        while (::read(wake_pipe[0], discarded.data(), discarded.size()) > 0) {
        }
      }
      if (stopping.load(std::memory_order_acquire)) break;
      if ((descriptors[0].revents & POLLIN) != 0) accept_clients(clients);

      std::array<bool, kMaximumClients> remove_client{};
      for (std::size_t index = 0; index < polled_client_count; ++index) {
        const short events = descriptors[index + 2].revents;
        bool keep = true;
        if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) keep = false;
        if (keep && (events & POLLOUT) != 0) keep = write_client(clients[index]);
        if (keep && (events & POLLIN) != 0 && clients[index].output.empty()) {
          keep = read_client(clients[index]);
        }
        if (!keep) {
          close_client(clients[index]);
          remove_client[index] = true;
        }
      }
      for (std::size_t remaining = polled_client_count; remaining > 0;
           --remaining) {
        const std::size_t index = remaining - 1;
        if (remove_client[index]) {
          clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(index));
        }
      }
    }
    for (auto& client : clients) close_client(client);
  }
};

LinuxControlService::LinuxControlService(Options options)
    : impl_(std::make_unique<Impl>(options)) {}

LinuxControlService::~LinuxControlService() noexcept {
  if (impl_ == nullptr) return;
  impl_->stopping.store(true, std::memory_order_release);
  impl_->wake();
  if (impl_->worker.joinable()) impl_->worker.join();
  if (impl_->listener >= 0) ::close(impl_->listener);
  for (const int descriptor : impl_->wake_pipe) {
    if (descriptor >= 0) ::close(descriptor);
  }
  if (!impl_->socket_path.empty()) {
    struct stat status {};
    if (::lstat(impl_->socket_path.c_str(), &status) == 0 &&
        status.st_dev == impl_->socket_device &&
        status.st_ino == impl_->socket_inode && S_ISSOCK(status.st_mode)) {
      ::unlink(impl_->socket_path.c_str());
    }
  }
}

auto LinuxControlService::start() noexcept -> bool {
  if (impl_ == nullptr || impl_->started || impl_->options.management == nullptr ||
      impl_->options.status == nullptr || impl_->runtime_directory.empty() ||
      impl_->runtime_directory.front() != '/') {
    return false;
  }
  try {
    const int parent = ::open(impl_->runtime_directory.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent < 0) return false;
    bool made_directory = false;
    if (::mkdirat(parent, "noisedeck-sync", 0700) == 0) {
      made_directory = true;
    } else if (errno != EEXIST) {
      ::close(parent);
      return false;
    }
    const int directory = ::openat(parent, "noisedeck-sync",
                                   O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                       O_NOFOLLOW);
    ::close(parent);
    if (directory < 0) return false;
    if (made_directory) (void)::fchmod(directory, 0700);
    struct stat directory_status {};
    const bool directory_ok =
        ::fstat(directory, &directory_status) == 0 &&
        S_ISDIR(directory_status.st_mode) &&
        static_cast<std::uint32_t>(directory_status.st_uid) ==
            impl_->options.expected_uid &&
        (directory_status.st_mode & 0777) == 0700;
    if (!directory_ok) {
      ::close(directory);
      return false;
    }
    impl_->socket_path = impl_->runtime_directory +
                         "/noisedeck-sync/control.sock";
    if (impl_->socket_path.size() >= sizeof(sockaddr_un{}.sun_path)) {
      ::close(directory);
      return false;
    }
    struct stat existing {};
    if (::fstatat(directory, "control.sock", &existing,
                  AT_SYMLINK_NOFOLLOW) == 0) {
      if (!S_ISSOCK(existing.st_mode) ||
          static_cast<std::uint32_t>(existing.st_uid) !=
              impl_->options.expected_uid ||
          ::unlinkat(directory, "control.sock", 0) != 0) {
        ::close(directory);
        return false;
      }
    } else if (errno != ENOENT) {
      ::close(directory);
      return false;
    }
    impl_->listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                               0);
    if (impl_->listener < 0) {
      ::close(directory);
      return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(impl_->socket_path.begin(), impl_->socket_path.end(),
              address.sun_path);
    if (::bind(impl_->listener, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0 ||
        ::fchmodat(directory, "control.sock", 0600, 0) != 0 ||
        ::listen(impl_->listener, static_cast<int>(kMaximumClients)) != 0) {
      ::close(directory);
      return false;
    }
    struct stat socket_status {};
    if (::fstatat(directory, "control.sock", &socket_status,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISSOCK(socket_status.st_mode) ||
        (socket_status.st_mode & 0777) != 0600) {
      ::close(directory);
      return false;
    }
    impl_->socket_device = socket_status.st_dev;
    impl_->socket_inode = socket_status.st_ino;
    ::close(directory);
    if (::pipe2(impl_->wake_pipe.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
      return false;
    }
    impl_->worker = std::thread([state = impl_.get()] { state->run(); });
    impl_->started = true;
    return true;
  } catch (...) {
    return false;
  }
}

auto LinuxControlService::socket_path() const noexcept -> std::string_view {
  return impl_ == nullptr ? std::string_view{} : impl_->socket_path;
}

auto LinuxControlService::begin(
    const pairing::PromptRequest& request) noexcept -> bool {
  if (impl_ == nullptr || !impl_->started || request.generation == 0) {
    return false;
  }
  {
    std::lock_guard lock(impl_->prompt_mutex);
    if (impl_->active_prompt) return false;
    impl_->active_request = request;
    impl_->active_prompt = true;
    impl_->prompt_result = {};
    impl_->canceled_generation = 0;
  }
  impl_->wake();
  return true;
}

auto LinuxControlService::poll() noexcept -> pairing::PromptResult {
  if (impl_ == nullptr) return {};
  std::lock_guard lock(impl_->prompt_mutex);
  const pairing::PromptResult result = impl_->prompt_result;
  impl_->prompt_result.available = false;
  return result;
}

void LinuxControlService::cancel(std::uint64_t generation) noexcept {
  if (impl_ == nullptr || generation == 0) return;
  {
    std::lock_guard lock(impl_->prompt_mutex);
    if (!impl_->active_prompt || impl_->active_request.generation != generation) {
      return;
    }
    impl_->prompt_result = {.available = true,
                            .generation = generation,
                            .decision = pairing::PromptDecision::Denied};
    impl_->active_prompt = false;
    impl_->canceled_generation =
        impl_->pair_owner_descriptor >= 0 ? generation : 0;
  }
  impl_->wake();
}

}  // namespace noisefactor::sync::linux_control
