#include "../test_harness.hpp"

#include "../../src/platform/macos/companion_process.hpp"

#import <Foundation/Foundation.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using noisefactor::sync::companion::CompanionProcess;
using noisefactor::sync::companion::CompanionProcessOptions;
using noisefactor::sync::companion::parse_pairings_json;
using noisefactor::sync::companion::parse_health_json;
using noisefactor::sync::companion::HealthSnapshot;

class TemporaryFixture {
 public:
  TemporaryFixture() {
    std::array<char, 128> pattern{};
    constexpr std::string_view prefix = "/tmp/sync-companion-process-XXXXXX";
    std::copy(prefix.begin(), prefix.end(), pattern.begin());
    char* created = ::mkdtemp(pattern.data());
    SYNC_REQUIRE(created != nullptr);
    directory_ = std::filesystem::canonical(created);
    executable_ = directory_ / "helper";
    arguments_ = directory_ / "arguments";
  }

  ~TemporaryFixture() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  void write_helper(std::string_view body) {
    std::ofstream stream(executable_);
    stream << "#!/bin/sh\n" << body;
    stream.close();
    SYNC_REQUIRE(::chmod(executable_.c_str(), 0700) == 0);
  }

  [[nodiscard]] std::string helper_path() const { return executable_.string(); }
  [[nodiscard]] std::string arguments_path() const { return arguments_.string(); }

 private:
  std::filesystem::path directory_;
  std::filesystem::path executable_;
  std::filesystem::path arguments_;
};

class TemporaryHttpServer {
 public:
  explicit TemporaryHttpServer(bool sends_http = true) {
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    SYNC_REQUIRE(socket_ >= 0);
    int reuse = 1;
    SYNC_REQUIRE(::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                              sizeof(reuse)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    SYNC_REQUIRE(::bind(socket_, reinterpret_cast<sockaddr*>(&address),
                        sizeof(address)) == 0);
    socklen_t size = sizeof(address);
    SYNC_REQUIRE(::getsockname(socket_, reinterpret_cast<sockaddr*>(&address),
                               &size) == 0);
    port_ = ntohs(address.sin_port);
    SYNC_REQUIRE(::listen(socket_, 1) == 0);
    worker_ = std::thread([this, sends_http] {
      constexpr std::string_view http_response =
          "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
          "Content-Length: 8\r\nConnection: close\r\n\r\nnot sync";
      constexpr std::string_view malformed_response = "not http\r\n";
      const std::string_view response =
          sends_http ? http_response : malformed_response;
      while (true) {
        const int client = ::accept(socket_, nullptr, nullptr);
        if (client < 0) return;
        std::array<char, 4096> request{};
        if (::recv(client, request.data(), request.size(), 0) > 0) {
          (void)::send(client, response.data(), response.size(), 0);
        }
        ::close(client);
        if (sends_http) return;
      }
    });
  }

  ~TemporaryHttpServer() {
    if (socket_ >= 0) {
      ::shutdown(socket_, SHUT_RDWR);
      ::close(socket_);
    }
    if (worker_.joinable()) worker_.join();
  }

  [[nodiscard]] std::string endpoint() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

 private:
  int socket_ = -1;
  std::uint16_t port_ = 0;
  std::thread worker_;
};

bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    @autoreleasepool {
      [[NSRunLoop currentRunLoop]
          runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
  }
  return predicate();
}

SYNC_TEST(companion_process_uses_exact_bundle_paths_and_drains_stderr) {
  TemporaryFixture fixture;
  fixture.write_helper(
      "printf '%s\\n' \"$@\" > " + fixture.arguments_path() + "\n"
      "printf 'ready on stderr\\n' >&2\n"
      "trap 'exit 0' TERM\n"
      "while :; do sleep 1; done\n");

  CompanionProcess process({
      .helper_path = fixture.helper_path(),
      .framework_path = "/private/Sync.app/Contents/Frameworks/Syphon.framework",
  });
  const auto arguments = process.launch_arguments();
  SYNC_REQUIRE(arguments == std::vector<std::string>({
      "--publisher", "syphon", "--publisher", "camera", "--syphon-framework",
      "/private/Sync.app/Contents/Frameworks/Syphon.framework"}));

  std::string captured_stderr;
  std::string error;
  SYNC_REQUIRE(process.start(
      [&](std::string_view bytes) { captured_stderr.append(bytes); },
      [](int) {}, error));
  SYNC_REQUIRE(error.empty());
  SYNC_REQUIRE(wait_until([&] {
    return captured_stderr.find("ready on stderr") != std::string::npos &&
           std::filesystem::exists(fixture.arguments_path());
  }));
  SYNC_REQUIRE(process.owned_pid().has_value());

  bool terminated = false;
  process.terminate([&] { terminated = true; });
  SYNC_REQUIRE(wait_until([&] { return terminated; }));
  SYNC_REQUIRE(!process.owned_pid().has_value());
}

SYNC_TEST(companion_process_pairing_parser_requires_normalized_origins) {
  const auto valid = parse_pairings_json(
      R"({"type":"pairings","origins":["https://one.example","http://localhost:8080"]})");
  SYNC_REQUIRE(valid.ok());
  SYNC_REQUIRE(valid.origins.size() == 2);

  const auto noncanonical = parse_pairings_json(
      R"({"type":"pairings","origins":["HTTPS://ONE.EXAMPLE:443"]})");
  SYNC_REQUIRE(!noncanonical.ok());
  const auto malformed = parse_pairings_json(R"({"origins":[]})");
  SYNC_REQUIRE(!malformed.ok());
}

SYNC_TEST(companion_process_revocation_requires_a_durable_committed_record) {
  using noisefactor::sync::companion::classify_revocation;
  const std::string_view revoked =
      R"({"type":"revocation","origin":"https://one.example","status":"revoked"})";

  const auto durable = classify_revocation(0, revoked);
  SYNC_REQUIRE(durable.ok());
  SYNC_REQUIRE(durable.revoked);

  const auto absent = classify_revocation(
      0,
      R"({"type":"revocation","origin":"https://one.example","status":"not_found"})");
  SYNC_REQUIRE(absent.ok());
  SYNC_REQUIRE(absent.revoked);

  // syncd exits 3 when the record reached the store but was never confirmed
  // durable. Reporting that as a clean revocation tells the operator a pairing
  // is gone when a crash could bring it back.
  const auto uncertain = classify_revocation(
      3,
      R"({"type":"revocation","origin":"https://one.example","status":"revoked_durability_uncertain"})");
  SYNC_REQUIRE(!uncertain.revoked);
  SYNC_REQUIRE(!uncertain.ok());

  // A durable-looking body cannot launder a nonzero exit, and a zero exit
  // cannot launder an uncertain body.
  SYNC_REQUIRE(!classify_revocation(3, revoked).revoked);
  SYNC_REQUIRE(
      !classify_revocation(
           0,
           R"({"type":"revocation","origin":"https://one.example","status":"revoked_durability_uncertain"})")
           .revoked);

  SYNC_REQUIRE(!classify_revocation(1, revoked).revoked);
  SYNC_REQUIRE(!classify_revocation(0, "not json").revoked);
  SYNC_REQUIRE(!classify_revocation(0, R"({"type":"revocation"})").revoked);
  SYNC_REQUIRE(
      !classify_revocation(
           0,
           R"({"type":"revocation","origin":"https://one.example","status":"invented"})")
           .revoked);
  SYNC_REQUIRE(
      !classify_revocation(
           0,
           R"({"type":"revocation","origin":"https://one.example","status":"revoked","extra":1})")
           .revoked);
}

SYNC_TEST(companion_process_bounds_management_and_never_owns_external_processes) {
  TemporaryFixture fixture;
  fixture.write_helper("exec sleep 5\n");
  CompanionProcess process({
      .helper_path = fixture.helper_path(),
      .framework_path = "/tmp/Syphon.framework",
      .management_timeout_seconds = 0.05,
  });
  SYNC_REQUIRE(!process.owned_pid().has_value());

  bool listed = false;
  std::string error;
  process.list_pairings([&](std::vector<std::string>, std::string result_error) {
    error = std::move(result_error);
    listed = true;
  });
  SYNC_REQUIRE(wait_until([&] { return listed; }));
  SYNC_REQUIRE(error == "Sync management command timed out.");

  bool terminated = false;
  process.terminate([&] { terminated = true; });
  SYNC_REQUIRE(wait_until([&] { return terminated; }));
  SYNC_REQUIRE(!process.owned_pid().has_value());
}

SYNC_TEST(companion_process_drains_large_management_stdout_before_exit) {
  TemporaryFixture fixture;
  fixture.write_helper(
      "i=0\n"
      "while [ \"$i\" -lt 4096 ]; do\n"
      "  printf 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'\n"
      "  i=$((i + 1))\n"
      "done\n");
  CompanionProcess process({
      .helper_path = fixture.helper_path(),
      .framework_path = "/tmp/Syphon.framework",
      .management_timeout_seconds = 1.0,
  });

  bool completed = false;
  std::string error;
  process.list_pairings([&](std::vector<std::string>, std::string result_error) {
    error = std::move(result_error);
    completed = true;
  });

  SYNC_REQUIRE(wait_until([&] { return completed; }));
  SYNC_REQUIRE(error == "Sync returned malformed pairing data.");
}

SYNC_TEST(companion_process_drains_large_management_stderr_before_exit) {
  TemporaryFixture fixture;
  fixture.write_helper(
      "i=0\n"
      "while [ \"$i\" -lt 4096 ]; do\n"
      "  printf 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx' >&2\n"
      "  i=$((i + 1))\n"
      "done\n"
      "printf '%s\\n' '{\"type\":\"pairings\",\"origins\":[]}'\n");
  CompanionProcess process({
      .helper_path = fixture.helper_path(),
      .framework_path = "/tmp/Syphon.framework",
      .management_timeout_seconds = 1.0,
  });

  bool completed = false;
  std::vector<std::string> origins;
  std::string error;
  process.list_pairings(
      [&](std::vector<std::string> result_origins, std::string result_error) {
        origins = std::move(result_origins);
        error = std::move(result_error);
        completed = true;
      });

  SYNC_REQUIRE(wait_until([&] { return completed; }));
  SYNC_REQUIRE(error.empty());
  SYNC_REQUIRE(origins.empty());
}

SYNC_TEST(companion_process_reports_an_unrelated_listener_as_a_port_conflict) {
  TemporaryHttpServer server;
  CompanionProcess process({
      .helper_path = "/bin/false",
      .framework_path = "/tmp/Syphon.framework",
      .endpoint = server.endpoint(),
      .health_timeout_seconds = 1.0,
  });

  bool completed = false;
  std::optional<noisefactor::sync::companion::HealthSnapshot> observed;
  process.probe([&](auto health, std::string) {
    observed = std::move(health);
    completed = true;
  });
  SYNC_REQUIRE(wait_until([&] { return completed; }));
  SYNC_REQUIRE(observed.has_value());
  SYNC_REQUIRE(observed->reachable);
  SYNC_REQUIRE(!observed->compatible);
}

SYNC_TEST(companion_process_reports_a_raw_tcp_listener_as_a_port_conflict) {
  TemporaryHttpServer server(false);
  CompanionProcess process({
      .helper_path = "/bin/false",
      .framework_path = "/tmp/Syphon.framework",
      .endpoint = server.endpoint(),
      .health_timeout_seconds = 1.0,
  });

  bool completed = false;
  std::optional<noisefactor::sync::companion::HealthSnapshot> observed;
  process.probe([&](auto health, std::string) {
    observed = std::move(health);
    completed = true;
  });
  SYNC_REQUIRE(wait_until([&] { return completed; }));
  SYNC_REQUIRE(observed.has_value());
  SYNC_REQUIRE(observed->reachable);
  SYNC_REQUIRE(!observed->compatible);
}

SYNC_TEST(companion_process_sequences_exit_before_restart_completion) {
  TemporaryFixture fixture;
  fixture.write_helper(
      "trap 'exit 0' TERM\n"
      "while :; do sleep 1; done\n");
  CompanionProcess process({
      .helper_path = fixture.helper_path(),
      .framework_path = "/tmp/Syphon.framework",
  });

  std::vector<std::string> events;
  std::string error;
  SYNC_REQUIRE(process.start(
      [](std::string_view) {},
      [&](int) { events.push_back("old exit"); }, error));
  const int old_pid = *process.owned_pid();

  bool replacement_started = false;
  process.terminate([&] {
    events.push_back("termination complete");
    SYNC_REQUIRE(process.start(
        [](std::string_view) {},
        [&](int) { events.push_back("replacement exit"); }, error));
    replacement_started = true;
  });
  SYNC_REQUIRE(wait_until([&] { return replacement_started; }));
  SYNC_REQUIRE(process.owned_pid().has_value());
  SYNC_REQUIRE(*process.owned_pid() != old_pid);
  SYNC_REQUIRE(wait_until([&] { return events.size() >= 2; }));
  SYNC_REQUIRE(events ==
               std::vector<std::string>({"old exit", "termination complete"}));

  bool replacement_stopped = false;
  process.terminate([&] { replacement_stopped = true; });
  SYNC_REQUIRE(wait_until([&] { return replacement_stopped; }));
  SYNC_REQUIRE(events == std::vector<std::string>(
                             {"old exit", "termination complete",
                              "replacement exit"}));
}

SYNC_TEST(companion_process_releases_an_unexpected_exit_before_relaunch) {
  TemporaryFixture fixture;
  fixture.write_helper("exit 17\n");
  CompanionProcess process({
      .helper_path = fixture.helper_path(),
      .framework_path = "/tmp/Syphon.framework",
  });

  std::vector<int> exits;
  std::string error;
  SYNC_REQUIRE(process.start(
      [](std::string_view) {},
      [&](int status) { exits.push_back(status); }, error));
  SYNC_REQUIRE(wait_until([&] { return exits.size() == 1; }));
  SYNC_REQUIRE(exits == std::vector<int>({17}));
  SYNC_REQUIRE(!process.owned_pid().has_value());

  fixture.write_helper(
      "trap 'exit 0' TERM\n"
      "while :; do sleep 1; done\n");
  SYNC_REQUIRE(process.start(
      [](std::string_view) {},
      [&](int status) { exits.push_back(status); }, error));
  SYNC_REQUIRE(process.owned_pid().has_value());

  bool stopped = false;
  process.terminate([&] { stopped = true; });
  SYNC_REQUIRE(wait_until([&] { return stopped; }));
  SYNC_REQUIRE(exits.size() == 2);
  SYNC_REQUIRE(exits.front() == 17);
  SYNC_REQUIRE(!process.owned_pid().has_value());
}

// A minimal well-formed /status body with a caller-supplied providers array.
std::string health_body(const std::string& providers) {
  return std::string("{\"product\":\"Sync\",\"status\":\"ok\",") +
         "\"version\":\"0.2.0\",\"protocolVersions\":[1]," +
         "\"instanceId\":\"abc\",\"activeSenders\":0," +
         "\"capabilities\":{\"send\":true,\"receive\":false,\"providers\":[" +
         providers + "]}}";
}

std::string provider(const std::string& id, bool available, bool selected,
                     const std::string& direction = "send") {
  return std::string("{\"id\":\"") + id + "\",\"direction\":\"" + direction +
         "\",\"available\":" + (available ? "true" : "false") +
         ",\"selected\":" + (selected ? "true" : "false") + "}";
}

SYNC_TEST(macos_health_collects_every_available_and_selected_send_provider) {
  const auto parsed = parse_health_json(
      health_body(provider("syphon", true, true) + "," + provider("ndi", true, true)),
      true);
  SYNC_REQUIRE(parsed.has_value());
  SYNC_REQUIRE(parsed->reachable);
  SYNC_REQUIRE(parsed->compatible);
  SYNC_REQUIRE(parsed->providers.size() == 2);
  SYNC_REQUIRE(parsed->providers.contains("syphon"));
  SYNC_REQUIRE(parsed->providers.contains("ndi"));
  SYNC_REQUIRE(parsed->providers.summary() == "Syphon, NDI");
}

SYNC_TEST(macos_health_excludes_unavailable_unselected_and_receive_providers) {
  const auto parsed = parse_health_json(
      health_body(provider("syphon", true, true) + "," +
                  provider("spout", false, true) + "," +
                  provider("ndi", true, false) + "," +
                  provider("inbound", true, true, "receive")),
      true);
  SYNC_REQUIRE(parsed.has_value());
  SYNC_REQUIRE(parsed->providers.size() == 1);
  SYNC_REQUIRE(parsed->providers.contains("syphon"));
  SYNC_REQUIRE(!parsed->providers.contains("spout"));
  SYNC_REQUIRE(!parsed->providers.contains("ndi"));
  SYNC_REQUIRE(!parsed->providers.contains("inbound"));
}

// A missing or non-boolean `selected` is a rejection, not a default: a helper
// that does not say whether it is publishing is not one to guess about.
SYNC_TEST(macos_health_rejects_a_provider_without_a_boolean_selected_field) {
  const std::string missing =
      "{\"id\":\"syphon\",\"direction\":\"send\",\"available\":true}";
  SYNC_REQUIRE(parse_health_json(health_body(missing), true).has_value() == false ||
               parse_health_json(health_body(missing), true)->providers.empty());

  const std::string wrong_type =
      "{\"id\":\"syphon\",\"direction\":\"send\",\"available\":true,"
      "\"selected\":\"yes\"}";
  SYNC_REQUIRE(parse_health_json(health_body(wrong_type), true).has_value() == false ||
               parse_health_json(health_body(wrong_type), true)->providers.empty());
}

// More providers than the snapshot can hold must reject the whole payload
// rather than silently reporting a truncated set.
SYNC_TEST(macos_health_rejects_more_providers_than_the_bound_allows) {
  std::string providers;
  for (int index = 0; index < 5; ++index) {
    if (index != 0) providers += ",";
    providers += provider("p" + std::to_string(index), true, true);
  }
  SYNC_REQUIRE(!parse_health_json(health_body(providers), true).has_value());
}

SYNC_TEST(macos_health_rejects_wrong_product_status_and_protocol) {
  const std::string ok = provider("syphon", true, true);
  SYNC_REQUIRE(parse_health_json(health_body(ok), true).has_value());

  std::string body = health_body(ok);
  const auto replaced = [](std::string text, const std::string& from,
                           const std::string& to) {
    const auto at = text.find(from);
    return at == std::string::npos ? text : text.replace(at, from.size(), to);
  };
  SYNC_REQUIRE(!parse_health_json(replaced(body, "\"Sync\"", "\"Other\""), true)
                    .has_value());
  SYNC_REQUIRE(!parse_health_json(replaced(body, "\"ok\"", "\"degraded\""), true)
                    .has_value());
  SYNC_REQUIRE(!parse_health_json(replaced(body, "[1]", "[2]"), true).has_value());
  SYNC_REQUIRE(!parse_health_json("not json at all", true).has_value());
  SYNC_REQUIRE(!parse_health_json("", true).has_value());
}

SYNC_TEST(macos_health_validates_active_sender_counts) {
  const std::string ok = provider("syphon", true, true);
  const auto with_senders = [&](const std::string& value) {
    std::string body = health_body(ok);
    const auto at = body.find("\"activeSenders\":0");
    return body.replace(at, std::string("\"activeSenders\":0").size(),
                        "\"activeSenders\":" + value);
  };
  SYNC_REQUIRE(parse_health_json(with_senders("64"), true).has_value());
  SYNC_REQUIRE(!parse_health_json(with_senders("65"), true).has_value());
  SYNC_REQUIRE(!parse_health_json(with_senders("-1"), true).has_value());
  SYNC_REQUIRE(!parse_health_json(with_senders("1.5"), true).has_value());
  SYNC_REQUIRE(!parse_health_json(with_senders("\"many\""), true).has_value());
}

} // namespace
