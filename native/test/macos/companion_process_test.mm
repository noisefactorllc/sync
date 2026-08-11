#include "../test_harness.hpp"

#include "../../src/platform/macos/companion_process.hpp"

#import <Foundation/Foundation.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

namespace {

using noisefactor::sync::companion::CompanionProcess;
using noisefactor::sync::companion::CompanionProcessOptions;
using noisefactor::sync::companion::parse_pairings_json;

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
      "--publisher", "syphon", "--syphon-framework",
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

} // namespace
