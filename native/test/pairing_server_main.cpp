#include <sync/pairing.hpp>
#include <sync/pairing_store.hpp>
#include <sync/server.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

class FakePrompt final : public noisefactor::sync::pairing::PairingPrompt {
public:
  enum class Mode { Approve, Deny, Timeout, Hang, LateApprove, Saturate };
  explicit FakePrompt(Mode mode) : mode_(mode) {}
  bool begin(const noisefactor::sync::pairing::PromptRequest &request) noexcept
      override {
    if (mode_ == Mode::Saturate || active_)
      return false;
    generation_ = request.generation;
    active_ = true;
    started_ = std::chrono::steady_clock::now();
    return true;
  }
  noisefactor::sync::pairing::PromptResult poll() noexcept override {
    if (!active_ || mode_ == Mode::Hang)
      return {};
    if (mode_ == Mode::LateApprove &&
        std::chrono::steady_clock::now() - started_ <
            std::chrono::milliseconds(540))
      return {};
    active_ = false;
    return {.available = true,
            .generation = generation_,
            .decision =
                mode_ == Mode::Approve || mode_ == Mode::LateApprove
                    ? noisefactor::sync::pairing::PromptDecision::Approved
                : mode_ == Mode::Timeout
                    ? noisefactor::sync::pairing::PromptDecision::TimedOut
                    : noisefactor::sync::pairing::PromptDecision::Denied};
  }
  void cancel(std::uint64_t generation) noexcept override {
    if (active_ && generation == generation_)
      active_ = false;
  }

private:
  Mode mode_;
  std::uint64_t generation_ = 0;
  bool active_ = false;
  std::chrono::steady_clock::time_point started_{};
};

class HoldingStoreAuthority final
    : public noisefactor::sync::pairing::PairingAuthority {
public:
  enum class Mode {
    None,
    HoldFirstAuthentication,
    HoldSecondAuthentication,
    HoldFirstIssue
  };

  HoldingStoreAuthority(noisefactor::sync::PairingStore &store, Mode mode,
                        std::string release_path)
      : authority_(store), mode_(mode), release_path_(std::move(release_path)) {}

  auto issue(const noisefactor::sync::NormalizedOrigin &origin,
             noisefactor::sync::PairingCommitGate &gate) noexcept
      -> noisefactor::sync::PairingIssueResult override {
    ++issue_calls_;
    if (mode_ == Mode::HoldFirstIssue && issue_calls_ == 1)
      wait_for_release();
    return authority_.issue(origin, gate);
  }

  auto authenticate(const noisefactor::sync::NormalizedOrigin &origin,
                    std::string_view token) noexcept
      -> noisefactor::sync::PairingAuthenticationResult override {
    ++authentication_calls_;
    write_authentication_count();
    if ((mode_ == Mode::HoldFirstAuthentication &&
         authentication_calls_ == 1) ||
        (mode_ == Mode::HoldSecondAuthentication &&
         authentication_calls_ == 2))
      wait_for_release();
    return authority_.authenticate(origin, token);
  }

private:
  void wait_for_release() noexcept {
    for (;;) {
      std::error_code error;
      if (std::filesystem::exists(release_path_, error) && !error)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  void write_authentication_count() noexcept {
    std::ofstream output(release_path_ + ".auth-count",
                         std::ios::out | std::ios::trunc);
    output << authentication_calls_;
  }

  noisefactor::sync::pairing::StorePairingAuthority authority_;
  Mode mode_;
  std::string release_path_;
  std::size_t authentication_calls_ = 0;
  std::size_t issue_calls_ = 0;
};

class FileCommitHook final : public noisefactor::sync::PairingStoreCommitHook {
public:
  explicit FileCommitHook(std::string release_path)
      : release_path_(std::move(release_path)) {}

  void before_commit() noexcept override {
    std::ofstream entered(release_path_ + ".entered",
                          std::ios::out | std::ios::trunc);
    entered << "entered";
    entered.close();
    for (;;) {
      std::error_code error;
      if (std::filesystem::exists(release_path_, error) && !error)
        return;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

private:
  std::string release_path_;
};

} // namespace

int main(int argc, char **argv) {
  if (argc == 4) {
    const std::string_view operation(argv[2]);
    if (operation != "revoke" && operation != "count")
      return 2;
    noisefactor::sync::PairingStore store;
    if (store.open({.path = argv[1]}) !=
        noisefactor::sync::PairingStoreError::None)
      return 1;
    if (operation == "count") {
      std::array<noisefactor::sync::NormalizedOrigin,
                 noisefactor::sync::kMaximumPairingOrigins>
          origins{};
      const auto listed = store.list(origins);
      if (listed.error != noisefactor::sync::PairingStoreError::None)
        return 1;
      return listed.count == static_cast<std::size_t>(std::stoul(argv[3])) ? 0
                                                                           : 1;
    }
    const auto normalized = noisefactor::sync::normalize_origin(argv[3]);
    if (!normalized.ok())
      return 1;
    const auto revoked = store.revoke(normalized.origin);
    return revoked.error == noisefactor::sync::PairingStoreError::None &&
                   revoked.revoked &&
                   revoked.commit ==
                       noisefactor::sync::PairingCommitState::CommittedDurable
               ? 0
               : 1;
  }
  if (argc != 3)
    return 2;
  const std::string_view mode_text(argv[2]);
  FakePrompt::Mode mode = FakePrompt::Mode::Approve;
  HoldingStoreAuthority::Mode authority_mode = HoldingStoreAuthority::Mode::None;
  bool hold_precommit = false;
  auto fail_point = noisefactor::sync::PairingStoreFailPoint::None;
  if (mode_text == "deny")
    mode = FakePrompt::Mode::Deny;
  else if (mode_text == "timeout")
    mode = FakePrompt::Mode::Timeout;
  else if (mode_text == "hang")
    mode = FakePrompt::Mode::Hang;
  else if (mode_text == "late-approve")
    mode = FakePrompt::Mode::LateApprove;
  else if (mode_text == "saturate")
    mode = FakePrompt::Mode::Saturate;
  else if (mode_text == "uncertain")
    fail_point = noisefactor::sync::PairingStoreFailPoint::
        AfterRenameBeforeDirectorySync;
  else if (mode_text == "hold-auth")
    authority_mode = HoldingStoreAuthority::Mode::HoldSecondAuthentication;
  else if (mode_text == "hold-first-auth")
    authority_mode = HoldingStoreAuthority::Mode::HoldFirstAuthentication;
  else if (mode_text == "hold-issue")
    authority_mode = HoldingStoreAuthority::Mode::HoldFirstIssue;
  else if (mode_text == "hold-precommit")
    hold_precommit = true;
  else if (mode_text != "approve")
    return 2;

  noisefactor::sync::PairingStore store;
  const std::string release_path = std::string(argv[1]) + ".release";
  FileCommitHook commit_hook(release_path);
  if (store.open({.path = argv[1],
                  .fail_point = fail_point,
                  .commit_hook = hold_precommit ? &commit_hook : nullptr}) !=
      noisefactor::sync::PairingStoreError::None)
    return 1;
  HoldingStoreAuthority authority(store, authority_mode,
                                  release_path);
  FakePrompt prompt(mode);
  noisefactor::sync::ServerOptions options;
  options.port = 0;
  options.test_receiver = true;
  options.providers[0] = {.id = "test",
                          .direction =
                              noisefactor::sync::ProviderDirection::Send,
                          .available = true,
                          .selected = true};
  options.provider_count = 1;
  options.pairing_authority = &authority;
  options.pairing_prompt = &prompt;
  return noisefactor::sync::run_server(options);
}
