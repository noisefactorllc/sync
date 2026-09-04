#include "../test_harness.hpp"

#include <sync/platform/linux_control_protocol.hpp>
#include <sync/platform/linux_control_service.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

namespace control = noisefactor::sync::linux_control;
namespace pairing = noisefactor::sync::pairing;
using noisefactor::sync::DaemonMetrics;
using noisefactor::sync::NormalizedOrigin;
using noisefactor::sync::PairingCommitState;
using noisefactor::sync::PairingListResult;
using noisefactor::sync::PairingRevocationResult;
using noisefactor::sync::PairingStoreError;

class TempDirectory {
 public:
  TempDirectory() {
    std::array<char, 256> pattern{};
    const std::string seed =
        (std::filesystem::temp_directory_path() / "sync-control-test-XXXXXX")
            .string();
    SYNC_REQUIRE(seed.size() < pattern.size());
    std::copy(seed.begin(), seed.end(), pattern.begin());
    char* made = ::mkdtemp(pattern.data());
    SYNC_REQUIRE(made != nullptr);
    path_ = std::filesystem::canonical(made);
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] auto path() const -> const std::filesystem::path& {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

NormalizedOrigin origin(std::string_view text) {
  const auto parsed = noisefactor::sync::normalize_origin(text);
  SYNC_REQUIRE(parsed.ok());
  return parsed.origin;
}

class Management final : public pairing::PairingManagement {
 public:
  auto list(std::span<NormalizedOrigin> output) noexcept
      -> PairingListResult override {
    ++list_calls;
    if (!output.empty()) output[0] = origin("https://visuals.example");
    return {.error = PairingStoreError::None, .count = 1};
  }
  auto revoke(const NormalizedOrigin& value) noexcept
      -> PairingRevocationResult override {
    ++revoke_calls;
    revoked.assign(value.view());
    return {.error = PairingStoreError::None,
            .revoked = true,
            .commit = PairingCommitState::CommittedDurable};
  }
  std::atomic<unsigned> list_calls{0};
  std::atomic<unsigned> revoke_calls{0};
  std::string revoked;
};

std::atomic<std::uint32_t> injected_uid{0};
std::uint32_t peer_uid(int) noexcept { return injected_uid.load(); }

int connect_to(std::string_view path) {
  const int descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  SYNC_REQUIRE(descriptor >= 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  SYNC_REQUIRE(path.size() < sizeof(address.sun_path));
  std::copy(path.begin(), path.end(), address.sun_path);
  SYNC_REQUIRE(::connect(descriptor, reinterpret_cast<sockaddr*>(&address),
                         sizeof(address)) == 0);
  return descriptor;
}

void write_all(int descriptor, std::span<const std::byte> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto written = ::send(descriptor, bytes.data() + offset,
                                bytes.size() - offset, MSG_NOSIGNAL);
    SYNC_REQUIRE(written > 0);
    offset += static_cast<std::size_t>(written);
  }
}

void send_json(int descriptor, std::string_view json) {
  write_all(descriptor, control::encode_linux_control_frame(json));
}

std::string receive_json(int descriptor) {
  pollfd ready{.fd = descriptor, .events = POLLIN, .revents = 0};
  SYNC_REQUIRE(::poll(&ready, 1, 2000) == 1);
  std::array<std::byte, 4> prefix{};
  SYNC_REQUIRE(::recv(descriptor, prefix.data(), prefix.size(), MSG_WAITALL) ==
               static_cast<ssize_t>(prefix.size()));
  const std::uint32_t length =
      (std::to_integer<std::uint32_t>(prefix[0]) << 24U) |
      (std::to_integer<std::uint32_t>(prefix[1]) << 16U) |
      (std::to_integer<std::uint32_t>(prefix[2]) << 8U) |
      std::to_integer<std::uint32_t>(prefix[3]);
  SYNC_REQUIRE(length > 0 &&
               length <= control::kMaximumLinuxControlMessageBytes);
  std::string output(length, '\0');
  SYNC_REQUIRE(::recv(descriptor, output.data(), output.size(), MSG_WAITALL) ==
               static_cast<ssize_t>(output.size()));
  return output;
}

control::LinuxControlService make_service(const TempDirectory& directory,
                                           Management& management,
                                           control::LinuxRuntimeStatus& status) {
  injected_uid.store(static_cast<std::uint32_t>(::geteuid()));
  return control::LinuxControlService({
      .runtime_directory = directory.path().string(),
      .expected_uid = static_cast<std::uint32_t>(::geteuid()),
      .management = &management,
      .status = &status,
      .peer_uid = peer_uid,
  });
}

}  // namespace

SYNC_TEST(linux_control_service_creates_private_socket_and_cleans_it_up) {
  TempDirectory directory;
  Management management;
  DaemonMetrics metrics;
  control::LinuxRuntimeStatus status(metrics);
  std::string socket_path;
  {
    auto service = make_service(directory, management, status);
    SYNC_REQUIRE(service.start());
    socket_path = service.socket_path();
    struct stat directory_status {};
    struct stat socket_status {};
    SYNC_REQUIRE(::lstat((directory.path() / "noisedeck-sync").c_str(),
                         &directory_status) == 0);
    SYNC_REQUIRE((directory_status.st_mode & 0777) == 0700);
    SYNC_REQUIRE(::lstat(socket_path.c_str(), &socket_status) == 0);
    SYNC_REQUIRE(S_ISSOCK(socket_status.st_mode));
    SYNC_REQUIRE((socket_status.st_mode & 0777) == 0600);
  }
  SYNC_REQUIRE(::access(socket_path.c_str(), F_OK) != 0);
}

SYNC_TEST(linux_control_service_enforces_peer_uid_before_dispatch) {
  TempDirectory directory;
  Management management;
  DaemonMetrics metrics;
  control::LinuxRuntimeStatus status(metrics);
  auto service = make_service(directory, management, status);
  SYNC_REQUIRE(service.start());
  injected_uid.store(static_cast<std::uint32_t>(::geteuid()) + 1U);
  const int client = connect_to(service.socket_path());
  SYNC_REQUIRE(receive_json(client).find("peer_not_authorized") !=
               std::string::npos);
  ::close(client);
  SYNC_REQUIRE(management.list_calls.load() == 0);
}

SYNC_TEST(linux_control_service_keeps_poll_events_bound_to_their_clients) {
  TempDirectory directory;
  Management management;
  DaemonMetrics metrics;
  control::LinuxRuntimeStatus status(metrics);
  auto service = make_service(directory, management, status);
  SYNC_REQUIRE(service.start());

  const int disconnecting = connect_to(service.socket_path());
  const int active = connect_to(service.socket_path());
  ::close(disconnecting);
  send_json(active, R"({"version":1,"command":"status"})");
  const std::string response = receive_json(active);
  SYNC_REQUIRE(response.find(R"("cameraSlotCount":3)") != std::string::npos);
  ::close(active);
}

SYNC_TEST(linux_control_service_rendezvous_accepts_only_current_owner_decision) {
  TempDirectory directory;
  Management management;
  DaemonMetrics metrics;
  control::LinuxRuntimeStatus status(metrics);
  auto service = make_service(directory, management, status);
  SYNC_REQUIRE(service.start());

  const int first = connect_to(service.socket_path());
  send_json(first, R"({"version":1,"command":"pair"})");
  const int busy = connect_to(service.socket_path());
  send_json(busy, R"({"version":1,"command":"pair"})");
  SYNC_REQUIRE(receive_json(busy).find("prompt_client_busy") !=
               std::string::npos);
  ::close(busy);

  pairing::PromptRequest request;
  SYNC_REQUIRE(request.assign(7, origin("https://visuals.example"),
                              "Noisedeck"));
  SYNC_REQUIRE(service.begin(request));
  const std::string prompt = receive_json(first);
  SYNC_REQUIRE(prompt.find(R"("type":"prompt")") != std::string::npos);
  SYNC_REQUIRE(prompt.find(R"("generation":7)") != std::string::npos);
  SYNC_REQUIRE(prompt.find("https://visuals.example") != std::string::npos);

  send_json(first,
            R"({"version":1,"command":"decision","generation":8,"approved":true})");
  SYNC_REQUIRE(receive_json(first).find("stale_generation") !=
               std::string::npos);
  ::close(first);
  const auto denied = service.poll();
  SYNC_REQUIRE(denied.available);
  SYNC_REQUIRE(denied.generation == 7);
  SYNC_REQUIRE(denied.decision == pairing::PromptDecision::Denied);

  const int after = connect_to(service.socket_path());
  send_json(after, R"({"version":1,"command":"pair"})");
  SYNC_REQUIRE(request.assign(9, origin("https://visuals.example"), "Deck"));
  SYNC_REQUIRE(service.begin(request));
  SYNC_REQUIRE(receive_json(after).find(R"("generation":9)") !=
               std::string::npos);
  send_json(after,
            R"({"version":1,"command":"decision","generation":9,"approved":true})");
  SYNC_REQUIRE(receive_json(after).find(R"("status":"accepted")") !=
               std::string::npos);
  ::close(after);
  const auto approved = service.poll();
  SYNC_REQUIRE(approved.available);
  SYNC_REQUIRE(approved.generation == 9);
  SYNC_REQUIRE(approved.decision == pairing::PromptDecision::Approved);
}

SYNC_TEST(linux_control_service_does_not_deliver_an_unowned_cancel_to_the_next_client) {
  TempDirectory directory;
  Management management;
  DaemonMetrics metrics;
  control::LinuxRuntimeStatus status(metrics);
  auto service = make_service(directory, management, status);
  SYNC_REQUIRE(service.start());

  pairing::PromptRequest request;
  SYNC_REQUIRE(request.assign(7, origin("https://visuals.example"),
                              "Noisedeck"));
  SYNC_REQUIRE(service.begin(request));
  service.cancel(7);

  const int next = connect_to(service.socket_path());
  send_json(next, R"({"version":1,"command":"pair"})");
  pollfd stale{.fd = next, .events = POLLIN, .revents = 0};
  SYNC_REQUIRE(::poll(&stale, 1, 100) == 0);

  SYNC_REQUIRE(request.assign(8, origin("https://visuals.example"), "Deck"));
  SYNC_REQUIRE(service.begin(request));
  const std::string prompt = receive_json(next);
  SYNC_REQUIRE(prompt.find(R"("generation":8)") != std::string::npos);
  ::close(next);
}

SYNC_TEST(linux_control_service_dispatches_status_list_and_revoke_once) {
  TempDirectory directory;
  Management management;
  DaemonMetrics metrics;
  metrics.note_camera_driving_frame();
  control::LinuxRuntimeStatus status(metrics);
  status.set_provider("camera", true, true, false, "recovering");
  auto service = make_service(directory, management, status);
  SYNC_REQUIRE(service.start());

  auto request = [&](std::string_view json) {
    const int client = connect_to(service.socket_path());
    send_json(client, json);
    const std::string response = receive_json(client);
    ::close(client);
    return response;
  };
  const auto status_json =
      request(R"({"version":1,"command":"status"})");
  SYNC_REQUIRE(status_json.find(R"("id":"camera")") != std::string::npos);
  SYNC_REQUIRE(status_json.find(R"("cameraDrivingFrames":1)") !=
               std::string::npos);
  SYNC_REQUIRE(status_json.find(R"("cameraSlotCount":3)") !=
               std::string::npos);
  const auto pairings =
      request(R"({"version":1,"command":"pairings"})");
  SYNC_REQUIRE(pairings.find("https://visuals.example") != std::string::npos);
  const auto revoked = request(
      R"({"version":1,"command":"revoke","origin":"https://visuals.example"})");
  SYNC_REQUIRE(revoked.find(R"("status":"revoked")") != std::string::npos);
  SYNC_REQUIRE(management.list_calls.load() == 1);
  SYNC_REQUIRE(management.revoke_calls.load() == 1);
  SYNC_REQUIRE(management.revoked == "https://visuals.example");
}

SYNC_TEST(linux_runtime_status_returns_coherent_concurrent_snapshots) {
  DaemonMetrics metrics;
  control::LinuxRuntimeStatus status(metrics);
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (unsigned index = 0; index < 10000; ++index) {
      status.set_provider("camera", true, (index % 2) == 0,
                          (index % 2) == 0,
                          (index % 2) == 0 ? "ready" : "recovering");
    }
    done.store(true);
  });
  while (!done.load()) {
    std::array<control::LinuxProviderStatus, 4> providers{};
    const std::size_t count = status.providers(providers);
    if (count == 0) continue;
    SYNC_REQUIRE(providers[0].available == providers[0].healthy);
    const std::string_view reason(providers[0].reason.data());
    SYNC_REQUIRE((providers[0].available && reason == "ready") ||
                 (!providers[0].available && reason == "recovering"));
  }
  writer.join();
}
