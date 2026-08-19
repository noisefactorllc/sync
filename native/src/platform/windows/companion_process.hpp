#pragma once

#include <sync/companion_model.hpp>

#include <cstdint>
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

// Exposed for direct unit testing the same way the macOS companion exposes
// it -- see native/test/windows/companion_process_test.cpp.
[[nodiscard]] PairingsResult parse_pairings_json(std::string_view json);

struct RevocationResult {
  bool revoked = false;
  std::string error;
  [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// syncd exits 3 when a revocation reached the store but could not be
// confirmed durable, so exit status alone cannot classify the outcome.
[[nodiscard]] RevocationResult classify_revocation(int exit_status,
                                                   std::string_view json);

// Strict, bounded parser for the JSON body `syncd`'s /status endpoint
// returns. Unlike the macOS companion (which leans on NSJSONSerialization),
// this repo has no JSON library for native Windows code, so this is a
// small hand-rolled reader in the spirit of native/src/control.cpp's
// parser. It is deliberately exposed here (macOS does not export its
// equivalent) so the parsing logic -- the highest-risk, most bug-prone part
// of this file -- gets direct unit test coverage rather than only being
// exercised indirectly through a live HTTP round trip.
[[nodiscard]] std::optional<HealthSnapshot> parse_health(std::string_view json);

// Resolves syncd.exe's path from the running Sync.exe's own path, replacing
// the file name in place of searching PATH. Exposed for direct testing of
// the path-substitution logic without launching a process. `module_path` is
// the full path to the currently running executable (as GetModuleFileNameW
// would report it, but passed in as UTF-8 here so this stays testable
// without touching Win32 in a unit test).
[[nodiscard]] std::string resolve_helper_path(std::string_view module_path);

struct CompanionProcessOptions {
  // Full path to syncd.exe, resolved via resolve_helper_path from the
  // running Sync.exe's own module path -- never searched for on PATH. See
  // the comment on CompanionProcess::start for why.
  std::wstring helper_path;
  // Full path to the installer-bundled SpoutLibrary.dll, or empty to omit
  // --spout-library and let syncd fall back to its own documented default
  // search location. NDI's runtime is discovered by syncd itself via
  // NDI_RUNTIME_DIR_V6/V5 and its documented install locations, so there is
  // no equivalent --ndi-runtime override here for normal operation.
  std::wstring spout_library_path;
  std::uint16_t port = 53979;
  std::uint32_t health_timeout_ms = 500;
  std::uint32_t management_timeout_ms = 2'000;
  std::uint32_t termination_timeout_ms = 2'000;
  // Marshals a completion callback onto the thread that owns the tray UI
  // (posted through the hidden message-only window's queue in app_main.cpp).
  // CompanionProcess performs all I/O on background threads and never
  // assumes it is called from, or may call back on, any particular thread
  // itself -- this is the seam that lets it hand control back to whichever
  // thread actually owns the UI and the CompanionModel.
  std::function<void(std::function<void()>)> dispatch_to_owner;
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

  [[nodiscard]] std::vector<std::wstring> launch_arguments() const;
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
