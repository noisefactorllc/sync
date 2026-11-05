#include "../test_harness.hpp"

#include "../../src/syncctl_cli.hpp"

#include <sync/pairing.hpp>
#include <sync/pairing_store.hpp>
#include <sync/platform/linux_control_protocol.hpp>
#include <sync/platform/linux_control_service.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <sstream>
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

control::LinuxControlService make_service(
    const TempDirectory& directory, Management& management,
    control::LinuxRuntimeStatus& status,
    std::uint32_t issuance_wait_ms = 30'000) {
  injected_uid.store(static_cast<std::uint32_t>(::geteuid()));
  return control::LinuxControlService({
      .runtime_directory = directory.path().string(),
      .expected_uid = static_cast<std::uint32_t>(::geteuid()),
      .management = &management,
      .status = &status,
      .peer_uid = peer_uid,
      .issuance_wait_ms = issuance_wait_ms,
  });
}

// Waits for the decision the terminal sends, within the prompt deadline.
pairing::PromptResult wait_for_decision(control::LinuxControlService& service) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = service.poll();
    if (result.available) return result;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return {};
}

struct TerminalPairing {
  pairing::PromptResult decision{};
  int exit = -1;
  std::string output;
  std::string error;
};

// Runs `syncctl pair` against the service and answers its prompt with y.
// `issue` then plays the server's part for the approved generation.
TerminalPairing pair_from_terminal(
    const TempDirectory& directory, control::LinuxControlService& service,
    std::string_view requested_origin,
    const std::function<void(std::uint64_t)>& issue) {
  SYNC_REQUIRE(::setenv("XDG_RUNTIME_DIR", directory.path().c_str(), 1) == 0);
  pairing::PromptRequest request;
  SYNC_REQUIRE(request.assign(7, origin(requested_origin), "Noisedeck"));
  TerminalPairing result;
  std::istringstream input("y\n");
  std::ostringstream output;
  std::ostringstream error;
  std::thread terminal([&] {
    const std::array<std::string_view, 1> arguments{"pair"};
    result.exit = noisefactor::sync::syncctl::execute(
        noisefactor::sync::syncctl::parse(arguments).options, input, output,
        error);
  });
  if (service.begin(request)) {
    result.decision = wait_for_decision(service);
    if (result.decision.available &&
        result.decision.decision == pairing::PromptDecision::Approved) {
      issue(result.decision.generation);
    }
  }
  terminal.join();
  result.output = output.str();
  result.error = error.str();
  return result;
}

// Plays the server's part: commits the token for the approved origin to a
// real store and reports how the commit ended.
void issue_to(noisefactor::sync::PairingStore& store,
              control::LinuxControlService& service, std::uint64_t generation,
              std::string_view requested_origin) {
  pairing::StorePairingAuthority authority(store);
  noisefactor::sync::PairingCommitGate gate;
  const auto issued = authority.issue(origin(requested_origin), gate);
  service.report_issuance(generation, pairing::issuance_failure(issued));
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
  const auto approved = wait_for_decision(service);
  SYNC_REQUIRE(approved.available);
  SYNC_REQUIRE(approved.generation == 9);
  SYNC_REQUIRE(approved.decision == pairing::PromptDecision::Approved);
  // The reply to an approval waits for the issuance result.
  pollfd waiting{.fd = after, .events = POLLIN, .revents = 0};
  SYNC_REQUIRE(::poll(&waiting, 1, 100) == 0);
  service.report_issuance(8, {});
  SYNC_REQUIRE(::poll(&waiting, 1, 100) == 0);
  service.report_issuance(9, {});
  SYNC_REQUIRE(receive_json(after) ==
               R"({"version":1,"type":"decision","status":"paired"})");
  ::close(after);
}

SYNC_TEST(syncctl_pair_prints_paired_only_after_a_durable_commit) {
  TempDirectory directory;
  Management management;
  DaemonMetrics metrics;
  control::LinuxRuntimeStatus status(metrics);
  auto service = make_service(directory, management, status);
  SYNC_REQUIRE(service.start());
  noisefactor::sync::PairingStore store;
  SYNC_REQUIRE(store.open({.path = (directory.path() / "state" / "pairings.v1")
                                       .string()}) == PairingStoreError::None);

  const auto paired = pair_from_terminal(
      directory, service, "https://visuals.example",
      [&](std::uint64_t generation) {
        issue_to(store, service, generation, "https://visuals.example");
      });
  SYNC_REQUIRE(paired.decision.generation == 7);
  SYNC_REQUIRE(paired.exit == noisefactor::sync::syncctl::kSuccessExit);
  SYNC_REQUIRE(paired.output.ends_with("[y/N] paired\n"));
  SYNC_REQUIRE(paired.error.empty());
}

SYNC_TEST(syncctl_pair_reports_a_full_store_and_exits_nonzero) {
  TempDirectory directory;
  Management management;
  DaemonMetrics metrics;
  control::LinuxRuntimeStatus status(metrics);
  auto service = make_service(directory, management, status);
  SYNC_REQUIRE(service.start());
  noisefactor::sync::PairingStore store;
  SYNC_REQUIRE(store.open({.path = (directory.path() / "state" / "pairings.v1")
                                       .string()}) == PairingStoreError::None);
  for (std::size_t index = 0;
       index < noisefactor::sync::kMaximumPairingOrigins; ++index) {
    SYNC_REQUIRE(store.issue(origin("https://host" + std::to_string(index) +
                                    ".example"))
                     .error == PairingStoreError::None);
  }

  const auto refused = pair_from_terminal(
      directory, service, "https://overflow.example",
      [&](std::uint64_t generation) {
        issue_to(store, service, generation, "https://overflow.example");
      });
  SYNC_REQUIRE(refused.decision.decision == pairing::PromptDecision::Approved);
  SYNC_REQUIRE(refused.exit == noisefactor::sync::syncctl::kFailureExit);
  SYNC_REQUIRE(refused.output.find("paired") == std::string::npos);
  SYNC_REQUIRE(refused.error == "syncctl: pairing failed: origin_limit\n");
}

SYNC_TEST(syncctl_pair_reports_an_uncertain_commit_and_exits_nonzero) {
  TempDirectory directory;
  Management management;
  DaemonMetrics metrics;
  control::LinuxRuntimeStatus status(metrics);
  auto service = make_service(directory, management, status);
  SYNC_REQUIRE(service.start());
  noisefactor::sync::PairingStore store;
  SYNC_REQUIRE(
      store.open({.path = (directory.path() / "state" / "pairings.v1").string(),
                  .fail_point = noisefactor::sync::PairingStoreFailPoint::
                      AfterRenameBeforeDirectorySync}) ==
      PairingStoreError::None);

  const auto uncertain = pair_from_terminal(
      directory, service, "https://visuals.example",
      [&](std::uint64_t generation) {
        issue_to(store, service, generation, "https://visuals.example");
      });
  SYNC_REQUIRE(uncertain.decision.decision ==
               pairing::PromptDecision::Approved);
  SYNC_REQUIRE(uncertain.exit == noisefactor::sync::syncctl::kFailureExit);
  SYNC_REQUIRE(uncertain.output.find("paired") == std::string::npos);
  SYNC_REQUIRE(uncertain.error ==
               "syncctl: pairing failed: store_durability_uncertain\n");
}

SYNC_TEST(syncctl_pair_fails_when_the_issuance_result_never_arrives_or_is_canceled) {
  TempDirectory directory;
  Management management;
  DaemonMetrics metrics;
  control::LinuxRuntimeStatus status(metrics);
  auto service = make_service(directory, management, status, 200);
  SYNC_REQUIRE(service.start());

  const auto unanswered = pair_from_terminal(
      directory, service, "https://visuals.example", [](std::uint64_t) {});
  SYNC_REQUIRE(unanswered.decision.decision ==
               pairing::PromptDecision::Approved);
  SYNC_REQUIRE(unanswered.exit == noisefactor::sync::syncctl::kFailureExit);
  SYNC_REQUIRE(unanswered.output.find("paired") == std::string::npos);
  SYNC_REQUIRE(unanswered.error ==
               "syncctl: pairing failed: pairing_timeout\n");

  const auto canceled = pair_from_terminal(
      directory, service, "https://visuals.example",
      [&](std::uint64_t generation) { service.cancel(generation); });
  SYNC_REQUIRE(canceled.exit == noisefactor::sync::syncctl::kFailureExit);
  SYNC_REQUIRE(canceled.output.find("paired") == std::string::npos);
  SYNC_REQUIRE(canceled.error ==
               "syncctl: pairing failed: pairing_canceled\n");
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

namespace {
class UpdateMaintenance final : public control::LinuxUpdateMaintenanceSource {
 public:
  bool ready() noexcept override { return true; }
  auto reserve(std::uint64_t generation, std::string_view digest) noexcept
      -> control::LinuxUpdateReservation override {
    if (generation != 7 || digest.size() != 64) return {};
    ++reservations;
    return {.token=19, .reason={}};
  }
  void cancel(std::uint64_t token) noexcept override {
    if (token == 19) ++cancellations;
  }
  std::atomic<unsigned> reservations{0};
  std::atomic<unsigned> cancellations{0};
};
constexpr std::string_view reserve_request =
    R"({"version":1,"command":"update-reserve","generation":7,"digest":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"})";
}

SYNC_TEST(linux_update_root_reservation_is_released_when_channel_closes) {
  TempDirectory directory;
  Management management;
  DaemonMetrics metrics;
  control::LinuxRuntimeStatus status(metrics);
  UpdateMaintenance updates;
  auto service = control::LinuxControlService({
      .runtime_directory=directory.path().string(),
      .expected_uid=static_cast<std::uint32_t>(::geteuid()),
      .management=&management, .status=&status, .updates=&updates,
      .peer_uid=peer_uid,
  });
  SYNC_REQUIRE(service.start());
  injected_uid.store(0);
  int probe=connect_to(service.socket_path());
  send_json(probe,R"({"version":1,"command":"update-probe"})");
  SYNC_REQUIRE(receive_json(probe).find("\"status\":\"idle\"") != std::string::npos);
  ::close(probe);
  SYNC_REQUIRE(updates.reservations.load()==0);
  probe=connect_to(service.socket_path());
  send_json(probe,R"({"version":1,"command":"update-status"})");
  SYNC_REQUIRE(receive_json(probe).find("\"runningVersion\"") != std::string::npos);
  ::close(probe);
  const int client=connect_to(service.socket_path());
  send_json(client,reserve_request);
  SYNC_REQUIRE(receive_json(client).find("\"token\":19") != std::string::npos);
  SYNC_REQUIRE(updates.reservations.load()==1);
  ::close(client);
  for (int i=0; i<100 && updates.cancellations.load()==0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  SYNC_REQUIRE(updates.cancellations.load()==1);
}

SYNC_TEST(linux_update_nonroot_cannot_reserve_and_missing_interlock_fails_closed) {
  TempDirectory directory;
  Management management;
  DaemonMetrics metrics;
  control::LinuxRuntimeStatus status(metrics);
  auto service=make_service(directory,management,status);
  SYNC_REQUIRE(service.start());
  injected_uid.store(static_cast<std::uint32_t>(::geteuid())+1U);
  int client=connect_to(service.socket_path());
  SYNC_REQUIRE(receive_json(client).find("peer_not_authorized") != std::string::npos);
  ::close(client);
  injected_uid.store(0);
  client=connect_to(service.socket_path());
  send_json(client,reserve_request);
  SYNC_REQUIRE(receive_json(client).find("interlock_unavailable") != std::string::npos);
  ::close(client);
}
