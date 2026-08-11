#pragma once

#include <sync/platform/companion_model.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace noisefactor::sync::companion {

struct PairingsResult {
  std::vector<std::string> origins;
  std::string error;
  [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

[[nodiscard]] PairingsResult parse_pairings_json(std::string_view json);

struct CompanionProcessOptions {
  std::string helper_path;
  std::string framework_path;
  std::string endpoint = "http://127.0.0.1:53979";
  double health_timeout_seconds = 0.5;
  double management_timeout_seconds = 2.0;
  double termination_timeout_seconds = 2.0;
};

class CompanionProcess {
 public:
  using StderrCallback = std::function<void(std::string_view)>;
  using ExitCallback = std::function<void(int)>;
  using ProbeCallback =
      std::function<void(std::optional<HealthSnapshot>, std::string)>;
  using PairingsCallback =
      std::function<void(std::vector<std::string>, std::string)>;
  using RevokeCallback = std::function<void(bool, std::string)>;
  using Completion = std::function<void()>;

  explicit CompanionProcess(CompanionProcessOptions options);
  ~CompanionProcess();
  CompanionProcess(const CompanionProcess&) = delete;
  CompanionProcess& operator=(const CompanionProcess&) = delete;

  [[nodiscard]] std::vector<std::string> launch_arguments() const;
  [[nodiscard]] std::optional<int> owned_pid() const noexcept;

  bool start(StderrCallback stderr_callback, ExitCallback exit_callback,
             std::string& error);
  void probe(ProbeCallback completion);
  void terminate(Completion completion);
  void list_pairings(PairingsCallback completion);
  void revoke_pairing(std::string origin, RevokeCallback completion);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace noisefactor::sync::companion
