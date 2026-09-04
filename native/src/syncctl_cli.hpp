#pragma once

#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

#include <sync/origin.hpp>

namespace noisefactor::sync::syncctl {

inline constexpr int kSuccessExit = 0;
inline constexpr int kFailureExit = 1;
inline constexpr int kUsageExit = 2;
inline constexpr int kDurabilityUncertainExit = 3;

enum class Command { Pair, Status, Pairings, Revoke, Doctor, CameraSetup };

struct Options {
  Command command = Command::Status;
  bool json = false;
  NormalizedOrigin origin{};
  std::string user;
};

struct ParseResult {
  bool valid = false;
  Options options{};
  [[nodiscard]] bool ok() const noexcept { return valid; }
};

struct Prompt {
  std::uint64_t generation = 0;
  std::string origin;
  std::string name;
};

[[nodiscard]] auto parse(std::span<const std::string_view> arguments)
    -> ParseResult;
void print_usage(std::ostream& error);
[[nodiscard]] auto escape_terminal_text(std::string_view input) -> std::string;
[[nodiscard]] auto render_pair_prompt(std::string_view json,
                                      std::ostream& output,
                                      Prompt& prompt) -> bool;
[[nodiscard]] auto read_approval(std::istream& input) -> bool;
[[nodiscard]] auto execute(const Options& options, std::istream& input,
                           std::ostream& output, std::ostream& error) -> int;

}  // namespace noisefactor::sync::syncctl
