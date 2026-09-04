#include "../test_harness.hpp"

#include "../../src/syncctl_cli.hpp"

#include <array>
#include <sstream>
#include <string>
#include <string_view>

namespace {

namespace syncctl = noisefactor::sync::syncctl;

template <std::size_t Size>
auto parse(const std::array<std::string_view, Size>& arguments) {
  return syncctl::parse(arguments);
}

}  // namespace

SYNC_TEST(syncctl_cli_accepts_the_stable_command_grammar) {
  SYNC_REQUIRE(parse(std::array<std::string_view, 1>{"pair"}).ok());
  SYNC_REQUIRE(parse(std::array<std::string_view, 1>{"status"}).ok());
  SYNC_REQUIRE(parse(std::array<std::string_view, 2>{"status", "--json"})
                   .options.json);
  SYNC_REQUIRE(parse(std::array<std::string_view, 1>{"pairings"}).ok());
  SYNC_REQUIRE(parse(std::array<std::string_view, 2>{"pairings", "--json"})
                   .ok());
  const auto revoke = parse(std::array<std::string_view, 3>{
      "revoke", "HTTPS://Visuals.Example:443", "--json"});
  SYNC_REQUIRE(revoke.ok());
  SYNC_REQUIRE(revoke.options.origin.view() == "https://visuals.example");
  SYNC_REQUIRE(parse(std::array<std::string_view, 1>{"doctor"}).ok());
  const auto setup = parse(std::array<std::string_view, 4>{
      "camera", "setup", "--user", "artist_1"});
  SYNC_REQUIRE(setup.ok());
  SYNC_REQUIRE(setup.options.user == "artist_1");
}

SYNC_TEST(syncctl_cli_rejects_missing_duplicate_and_mixed_shapes) {
  SYNC_REQUIRE(!parse(std::array<std::string_view, 0>{}).ok());
  SYNC_REQUIRE(!parse(std::array<std::string_view, 2>{"pair", "--json"}).ok());
  SYNC_REQUIRE(
      !parse(std::array<std::string_view, 3>{"status", "--json", "--json"})
           .ok());
  SYNC_REQUIRE(!parse(std::array<std::string_view, 1>{"revoke"}).ok());
  SYNC_REQUIRE(!parse(std::array<std::string_view, 2>{"revoke", "bad"}).ok());
  SYNC_REQUIRE(!parse(std::array<std::string_view, 3>{"camera", "setup",
                                                     "--user"})
                    .ok());
  SYNC_REQUIRE(!parse(std::array<std::string_view, 4>{
                         "camera", "setup", "--user", "bad user"})
                    .ok());
  SYNC_REQUIRE(!parse(std::array<std::string_view, 4>{
                         "camera", "setup", "--user", "-root"})
                    .ok());
}

SYNC_TEST(syncctl_pair_prompt_is_terminal_safe_and_affirmative_only) {
  const std::string json =
      R"({"version":1,"type":"prompt","generation":7,"origin":"https://visuals.example","name":"Noisedeck\u001b\n\"\\","deadlineMs":30000})";
  syncctl::Prompt prompt;
  std::ostringstream output;
  SYNC_REQUIRE(syncctl::render_pair_prompt(json, output, prompt));
  SYNC_REQUIRE(prompt.generation == 7);
  SYNC_REQUIRE(output.str() ==
               "Origin: https://visuals.example\n"
               "Name: Noisedeck\\x1b\\n\\\"\\\\\n"
               "Allow this browser to pair? [y/N] ");

  for (const std::string value : {"y\n", "Y\n"}) {
    std::istringstream input(value);
    SYNC_REQUIRE(syncctl::read_approval(input));
  }
  for (const std::string value : {"\n", "yes\n", "n\n", "y extra\n"}) {
    std::istringstream input(value);
    SYNC_REQUIRE(!syncctl::read_approval(input));
  }
  std::istringstream eof;
  SYNC_REQUIRE(!syncctl::read_approval(eof));
  std::istringstream failed("y\n");
  failed.setstate(std::ios::badbit);
  SYNC_REQUIRE(!syncctl::read_approval(failed));
}

SYNC_TEST(syncctl_pair_prompt_rejects_wrong_shape_and_bounds) {
  for (const std::string_view json : {
           R"({"version":2,"type":"prompt","generation":7,"origin":"https://visuals.example","name":"Deck","deadlineMs":30000})",
           R"({"version":1,"type":"prompt","generation":0,"origin":"https://visuals.example","name":"Deck","deadlineMs":30000})",
           R"({"version":1,"type":"status","generation":7,"origin":"https://visuals.example","name":"Deck","deadlineMs":30000})",
           R"({"version":1,"type":"prompt","generation":7,"origin":"bad","name":"Deck","deadlineMs":30000})",
           R"({"version":1,"type":"prompt","generation":7,"origin":"https://visuals.example","name":"Deck"})",
       }) {
    syncctl::Prompt prompt;
    std::ostringstream output;
    SYNC_REQUIRE(!syncctl::render_pair_prompt(json, output, prompt));
    SYNC_REQUIRE(output.str().empty());
  }
}
