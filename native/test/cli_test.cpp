#include "test_harness.hpp"

#include "../src/cli.hpp"

#include <sync/origin.hpp>
#include <sync/pairing_store.hpp>

#include <array>
#include <filesystem>
#include <initializer_list>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

namespace cli = noisefactor::sync::cli;

cli::ParseResult parse(std::initializer_list<std::string_view> arguments) {
  const std::vector<std::string_view> values(arguments);
  return cli::parse(values);
}

noisefactor::sync::NormalizedOrigin origin(std::string_view value) {
  const auto result = noisefactor::sync::normalize_origin(value);
  SYNC_REQUIRE(result.ok());
  return result.origin;
}

class TemporaryStore {
 public:
  TemporaryStore() {
    std::array<char, 128> pattern{};
    constexpr std::string_view prefix = "/tmp/sync-cli-test-XXXXXX";
    std::copy(prefix.begin(), prefix.end(), pattern.begin());
    char* created = ::mkdtemp(pattern.data());
    SYNC_REQUIRE(created != nullptr);
    directory_ = std::filesystem::canonical(created).string();
    path_ = directory_ + "/pairings.v1";
  }

  ~TemporaryStore() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
  }

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string directory_;
  std::string path_;
};

std::string issue(TemporaryStore& temporary, std::string_view origin_text) {
  noisefactor::sync::PairingStore store;
  SYNC_REQUIRE(store.open({.path = temporary.path()}) ==
               noisefactor::sync::PairingStoreError::None);
  const auto issued = store.issue(origin(origin_text));
  SYNC_REQUIRE(issued.error == noisefactor::sync::PairingStoreError::None);
  SYNC_REQUIRE(issued.commit ==
               noisefactor::sync::PairingCommitState::CommittedDurable);
  return std::string(issued.token.view());
}

cli::Options management_options(cli::Mode mode,
                                std::string_view origin_text = {}) {
  cli::Options options;
  options.mode = mode;
  if (!origin_text.empty()) options.revoke_origin = origin(origin_text);
  return options;
}

SYNC_TEST(cli_no_arguments_selects_production_defaults) {
  const auto parsed = parse({});
  SYNC_REQUIRE(parsed.ok());
  SYNC_REQUIRE(parsed.options.mode == cli::Mode::Production);
  SYNC_REQUIRE(parsed.options.port == 53979);
  SYNC_REQUIRE(parsed.options.publisher == "syphon");
  SYNC_REQUIRE(!parsed.options.test_receiver);
  SYNC_REQUIRE(parsed.options.allowed_origin.empty());
  SYNC_REQUIRE(parsed.options.test_token.empty());
}

SYNC_TEST(cli_production_port_and_syphon_selection_are_strict) {
  const auto selected = parse({"--port", "54001", "--publisher", "syphon",
                               "--syphon-framework", "/tmp/Syphon.framework"});
  SYNC_REQUIRE(selected.ok());
  SYNC_REQUIRE(selected.options.mode == cli::Mode::Production);
  SYNC_REQUIRE(selected.options.port == 54001);
  SYNC_REQUIRE(selected.options.publisher == "syphon");
  SYNC_REQUIRE(selected.options.syphon_framework_path ==
               "/tmp/Syphon.framework");

  for (const auto& invalid : {
           parse({"--port", "0"}),
           parse({"--port", "65536"}),
           parse({"--port", "053979"}),
           parse({"--port", "53979", "--port", "53980"}),
           parse({"--publisher", "unknown"}),
           parse({"--publisher", "syphon", "--publisher", "syphon"}),
           parse({"--syphon-framework", "/tmp/Syphon.framework"}),
           parse({"--publisher", "syphon", "--syphon-framework", ""}),
       }) {
    SYNC_REQUIRE(!invalid.ok());
  }
}

SYNC_TEST(cli_static_test_mode_preserves_the_exact_legacy_shape) {
  const auto receiver = parse({"--port", "0", "--test-origin",
                               "https://client.example", "--test-token",
                               "test-token", "--test-receiver"});
  SYNC_REQUIRE(receiver.ok());
  SYNC_REQUIRE(receiver.options.mode == cli::Mode::StaticTest);
  SYNC_REQUIRE(receiver.options.port == 0);
  SYNC_REQUIRE(receiver.options.test_receiver);

  const auto publisher = parse({"--port", "54321", "--test-origin",
                                "https://client.example", "--test-token",
                                "test-token", "--publisher", "syphon",
                                "--syphon-framework",
                                "/tmp/Syphon.framework"});
  SYNC_REQUIRE(publisher.ok());
  SYNC_REQUIRE(publisher.options.mode == cli::Mode::StaticTest);
  SYNC_REQUIRE(!publisher.options.test_receiver);

  for (const auto& invalid : {
           parse({"--test-origin", "https://client.example"}),
           parse({"--test-token", "test-token"}),
           parse({"--test-receiver"}),
           parse({"--port", "0", "--test-origin", "https://client.example",
                  "--test-token", "test-token"}),
           parse({"--port", "0", "--test-origin", "https://client.example",
                  "--test-token", "test-token", "--test-receiver",
                  "--publisher", "syphon"}),
       }) {
    SYNC_REQUIRE(!invalid.ok());
  }
}

SYNC_TEST(cli_management_modes_are_normalized_mutually_exclusive_and_argument_free) {
  const auto list = parse({"--list-pairings"});
  SYNC_REQUIRE(list.ok());
  SYNC_REQUIRE(list.options.mode == cli::Mode::ListPairings);

  const auto revoke = parse({"--revoke-origin", "HTTPS://Example.COM:443"});
  SYNC_REQUIRE(revoke.ok());
  SYNC_REQUIRE(revoke.options.mode == cli::Mode::RevokeOrigin);
  SYNC_REQUIRE(revoke.options.revoke_origin.view() == "https://example.com");

  for (const auto& invalid : {
           parse({"--list-pairings", "--revoke-origin",
                  "https://example.com"}),
           parse({"--list-pairings", "--port", "53979"}),
           parse({"--revoke-origin"}),
           parse({"--revoke-origin", "http://remote.example"}),
           parse({"--revoke-origin", "https://example.com", "--publisher",
                  "syphon"}),
           parse({"--list-pairings", "--list-pairings"}),
       }) {
    SYNC_REQUIRE(!invalid.ok());
  }
}

SYNC_TEST(cli_rejects_unknown_duplicate_missing_and_secret_injection_shapes) {
  for (const auto& invalid : {
           parse({"--unknown"}),
           parse({"--port"}),
           parse({"--publisher"}),
           parse({"--test-origin", "https://client.example",
                  "--test-origin", "https://client.example"}),
           parse({"--test-token", "one", "--test-token", "two"}),
           parse({"--test-origin", "https://client.example", "--test-token",
                  "test-token", "--publisher", "syphon"}),
       }) {
    SYNC_REQUIRE(!invalid.ok());
  }
}

SYNC_TEST(cli_management_lists_normalized_origins_without_token_or_hash_output) {
  TemporaryStore temporary;
  const std::string first_token = issue(temporary, "https://one.example");
  const std::string second_token = issue(temporary, "app://noisedeck");
  std::ostringstream output;
  std::ostringstream error;
  const cli::ManagementOverride override{.store_path = temporary.path()};

  const int status = cli::run_management(
      management_options(cli::Mode::ListPairings), output, error, &override);
  SYNC_REQUIRE(status == 0);
  SYNC_REQUIRE(error.str().empty());
  SYNC_REQUIRE(output.str() ==
               "{\"type\":\"pairings\",\"origins\":[\"https://one.example\","
               "\"app://noisedeck\"]}\n");
  SYNC_REQUIRE(output.str().find(first_token) == std::string::npos);
  SYNC_REQUIRE(output.str().find(second_token) == std::string::npos);
  SYNC_REQUIRE(!std::regex_search(output.str(), std::regex("[a-f0-9]{64}")));
}

SYNC_TEST(cli_management_revoke_distinguishes_durable_not_found_and_uncertain) {
  {
    TemporaryStore temporary;
    const std::string token = issue(temporary, "https://one.example");
    std::ostringstream output;
    std::ostringstream error;
    const cli::ManagementOverride override{.store_path = temporary.path()};
    const int status = cli::run_management(
        management_options(cli::Mode::RevokeOrigin, "https://one.example"),
        output, error, &override);
    SYNC_REQUIRE(status == 0);
    SYNC_REQUIRE(error.str().empty());
    SYNC_REQUIRE(output.str() ==
                 "{\"type\":\"revocation\",\"origin\":"
                 "\"https://one.example\",\"status\":\"revoked\"}\n");
    SYNC_REQUIRE(output.str().find(token) == std::string::npos);

    output.str({});
    output.clear();
    const int absent = cli::run_management(
        management_options(cli::Mode::RevokeOrigin, "https://one.example"),
        output, error, &override);
    SYNC_REQUIRE(absent == 0);
    SYNC_REQUIRE(output.str().find("\"status\":\"not_found\"") !=
                 std::string::npos);
  }
  {
    TemporaryStore temporary;
    (void)issue(temporary, "https://uncertain.example");
    std::ostringstream output;
    std::ostringstream error;
    const cli::ManagementOverride override{
        .store_path = temporary.path(),
        .fail_point = noisefactor::sync::PairingStoreFailPoint::
            AfterRenameBeforeDirectorySync};
    const int status = cli::run_management(
        management_options(cli::Mode::RevokeOrigin,
                           "https://uncertain.example"),
        output, error, &override);
    SYNC_REQUIRE(status == cli::kDurabilityUncertainExit);
    SYNC_REQUIRE(error.str().empty());
    SYNC_REQUIRE(output.str().find(
                     "\"status\":\"revoked_durability_uncertain\"") !=
                 std::string::npos);
  }
}

SYNC_TEST(cli_management_not_committed_is_an_error_and_preserves_the_record) {
  TemporaryStore temporary;
  const std::string token = issue(temporary, "https://preserved.example");
  std::ostringstream output;
  std::ostringstream error;
  const cli::ManagementOverride override{
      .store_path = temporary.path(),
      .fail_point = noisefactor::sync::PairingStoreFailPoint::BeforeRename};
  const int status = cli::run_management(
      management_options(cli::Mode::RevokeOrigin,
                         "https://preserved.example"),
      output, error, &override);
  SYNC_REQUIRE(status == cli::kFailureExit);
  SYNC_REQUIRE(output.str().empty());
  SYNC_REQUIRE(error.str().find("revoke_not_committed") != std::string::npos);
  SYNC_REQUIRE(error.str().find(token) == std::string::npos);

  noisefactor::sync::PairingStore reopened;
  SYNC_REQUIRE(reopened.open({.path = temporary.path()}) ==
               noisefactor::sync::PairingStoreError::None);
  const auto authenticated =
      reopened.authenticate(origin("https://preserved.example"), token);
  SYNC_REQUIRE(authenticated.error ==
               noisefactor::sync::PairingStoreError::None);
  SYNC_REQUIRE(authenticated.authenticated);
}

}  // namespace
