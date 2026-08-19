#include "test_harness.hpp"

#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include <sync/frame_receiver.hpp>
#include <sync/pairing.hpp>
#include <sync/server.hpp>

namespace {

class CallerOwnedPublisher final : public noisefactor::sync::FramePublisher {
public:
  explicit CallerOwnedPublisher(bool &destroyed) : destroyed_(destroyed) {}
  ~CallerOwnedPublisher() override { destroyed_ = true; }

  auto open_sender(std::string_view, std::string_view) noexcept
      -> bool override {
    ++open_calls;
    return true;
  }

  auto publish(std::string_view,
               const noisefactor::sync::protocol::FrameView &) noexcept
      -> noisefactor::sync::PublishResult override {
    return noisefactor::sync::PublishResult::Accepted;
  }

  std::size_t open_calls = 0;

private:
  bool &destroyed_;
};

class FatalPublisher final : public noisefactor::sync::FramePublisher {
public:
  auto publish(std::string_view,
               const noisefactor::sync::protocol::FrameView &) noexcept
      -> noisefactor::sync::PublishResult override {
    return noisefactor::sync::PublishResult::Accepted;
  }

  auto poll_failure(std::uint64_t) noexcept
      -> std::optional<noisefactor::sync::ProviderFailure> override {
    ++poll_calls;
    return noisefactor::sync::ProviderFailure{
        .kind = noisefactor::sync::ProviderFailureKind::MetalCommandFailed,
        .native_status = 5,
        .native_error_code = -9,
    };
  }

  std::size_t poll_calls = 0;
};

class StubPairingAuthority final
    : public noisefactor::sync::pairing::PairingAuthority {
public:
  auto issue(const noisefactor::sync::NormalizedOrigin &,
             noisefactor::sync::PairingCommitGate &) noexcept
      -> noisefactor::sync::PairingIssueResult override {
    return {};
  }

  auto authenticate(const noisefactor::sync::NormalizedOrigin &,
                    std::string_view) noexcept
      -> noisefactor::sync::PairingAuthenticationResult override {
    return {};
  }
};

class StubPairingPrompt final
    : public noisefactor::sync::pairing::PairingPrompt {
public:
  auto begin(const noisefactor::sync::pairing::PromptRequest &) noexcept
      -> bool override {
    return false;
  }

  auto poll() noexcept -> noisefactor::sync::pairing::PromptResult override {
    return {};
  }

  void cancel(std::uint64_t) noexcept override {}
};

auto base_options() -> noisefactor::sync::ServerOptions {
  noisefactor::sync::ServerOptions options;
  options.allowed_origin = "https://client.example";
  options.test_token = "test-token";
  options.providers[0] = {
      .id = "test",
      .direction = noisefactor::sync::ProviderDirection::Send,
      .available = true,
      .selected = true,
  };
  options.provider_count = 1;
  return options;
}

class ScopedStreamCapture {
public:
  ScopedStreamCapture()
      : old_stdout_(std::cout.rdbuf(stdout_.rdbuf())),
        old_stderr_(std::cerr.rdbuf(stderr_.rdbuf())) {}
  ~ScopedStreamCapture() {
    std::cout.rdbuf(old_stdout_);
    std::cerr.rdbuf(old_stderr_);
  }

  [[nodiscard]] auto stdout_text() const -> std::string {
    return stdout_.str();
  }
  [[nodiscard]] auto stderr_text() const -> std::string {
    return stderr_.str();
  }

private:
  std::ostringstream stdout_;
  std::ostringstream stderr_;
  std::streambuf *old_stdout_;
  std::streambuf *old_stderr_;
};

// Stopping the server from inside its own event pump needs a signal the
// loop will actually receive, and on Windows there is no such signal a
// process can send to itself in-process. A CRT raise(SIGTERM) does not
// reach libuv there at all -- it runs the CRT default action and
// terminates the process with exit code 3 -- and the one event that does
// reach the loop, CTRL_BREAK_EVENT, is delivered to every process sharing
// the console, which under ctest means the test runner and the shell that
// launched it. Neither is usable here, so these two tests are POSIX-only.
//
// The Windows equivalent is covered where it actually matters:
// scripts/smoke-windows-app.ps1 quits the tray app and asserts the managed
// helper goes with it, which exercises the real CTRL_BREAK_EVENT ->
// SIGBREAK shutdown path end to end against the shipped binaries.
#if !defined(_WIN32)
void stop_after_platform_pump(void *context) noexcept {
  auto *calls = static_cast<std::atomic<std::size_t> *>(context);
  if (calls->fetch_add(1, std::memory_order_relaxed) == 0) {
    std::raise(SIGTERM);
  }
}

void stop_after_second_platform_pump(void *context) noexcept {
  auto *calls = static_cast<std::atomic<std::size_t> *>(context);
  if (calls->fetch_add(1, std::memory_order_relaxed) == 1) {
    std::raise(SIGTERM);
  }
}
#endif

} // namespace

#if !defined(_WIN32)
SYNC_TEST(server_invokes_the_configured_platform_event_pump) {
  std::atomic<std::size_t> calls{0};
  auto options = base_options();
  options.test_receiver = true;
  options.platform_event_pump = stop_after_platform_pump;
  options.platform_event_pump_context = &calls;

  ScopedStreamCapture capture;
  SYNC_REQUIRE(noisefactor::sync::run_server(options) == 0);
  SYNC_REQUIRE(calls.load(std::memory_order_relaxed) >= 1);
}
#endif

SYNC_TEST(
    server_rejects_invalid_publisher_modes_before_ready_and_never_owns_external) {
  bool destroyed = false;
  {
    CallerOwnedPublisher publisher(destroyed);
    auto options = base_options();
    options.test_receiver = true;
    ScopedStreamCapture capture;
    SYNC_REQUIRE(noisefactor::sync::run_server(options, &publisher) == 1);
    SYNC_REQUIRE(capture.stdout_text().empty());
    SYNC_REQUIRE(publisher.open_calls == 0);
    SYNC_REQUIRE(!destroyed);
  }
  SYNC_REQUIRE(destroyed);

  auto options = base_options();
  options.test_receiver = false;
  ScopedStreamCapture capture;
  SYNC_REQUIRE(noisefactor::sync::run_server(options, nullptr) == 1);
  SYNC_REQUIRE(capture.stdout_text().empty());
}

SYNC_TEST(server_rejects_invalid_provider_models_before_ready) {
  bool destroyed = false;
  CallerOwnedPublisher publisher(destroyed);

  for (int invalid_case = 0; invalid_case < 5; ++invalid_case) {
    auto options = base_options();
    if (invalid_case == 0) {
      options.provider_count = 0;
    } else if (invalid_case == 1) {
      options.provider_count =
          noisefactor::sync::kMaximumProviderCapabilities + 1;
    } else if (invalid_case == 2) {
      options.providers[0].id.clear();
    } else if (invalid_case == 3) {
      options.provider_count = 2;
      options.providers[1] = options.providers[0];
    } else {
      options.providers[0].direction =
          static_cast<noisefactor::sync::ProviderDirection>(99);
    }
    ScopedStreamCapture capture;
    SYNC_REQUIRE(noisefactor::sync::run_server(options, &publisher) == 1);
    SYNC_REQUIRE(capture.stdout_text().empty());
  }
  SYNC_REQUIRE(!destroyed);
}

SYNC_TEST(server_rejects_half_configured_or_mixed_pairing_authority_modes) {
  bool destroyed = false;
  CallerOwnedPublisher publisher(destroyed);
  StubPairingAuthority authority;
  StubPairingPrompt prompt;

  for (int invalid_case = 0; invalid_case < 4; ++invalid_case) {
    auto options = base_options();
    if (invalid_case == 0) {
      options.pairing_authority = &authority;
    } else if (invalid_case == 1) {
      options.pairing_prompt = &prompt;
    } else {
      options.pairing_authority = &authority;
      options.pairing_prompt = &prompt;
      if (invalid_case == 2) {
        options.allowed_origin.clear();
      } else {
        options.test_token.clear();
      }
    }
    ScopedStreamCapture capture;
    SYNC_REQUIRE(noisefactor::sync::run_server(options, &publisher) == 1);
    SYNC_REQUIRE(capture.stdout_text().empty());
  }

  auto static_options = base_options();
  static_options.allowed_origin.clear();
  static_options.test_token.clear();
  ScopedStreamCapture capture;
  SYNC_REQUIRE(noisefactor::sync::run_server(static_options, &publisher) == 1);
  SYNC_REQUIRE(capture.stdout_text().empty());
  SYNC_REQUIRE(!destroyed);
}

#if !defined(_WIN32)
SYNC_TEST(server_exits_nonzero_once_after_a_fatal_provider_failure) {
  FatalPublisher publisher;
  std::atomic<std::size_t> pump_calls{0};
  auto options = base_options();
  options.platform_event_pump = stop_after_second_platform_pump;
  options.platform_event_pump_context = &pump_calls;

  ScopedStreamCapture capture;
  SYNC_REQUIRE(noisefactor::sync::run_server(options, &publisher) == 1);
  SYNC_REQUIRE(publisher.poll_calls == 1);
  SYNC_REQUIRE(pump_calls.load(std::memory_order_relaxed) == 1);
  SYNC_REQUIRE(capture.stdout_text().starts_with(
      "{\"type\":\"ready\",\"port\":"));
  SYNC_REQUIRE(capture.stderr_text() ==
      "syncd: fatal provider failure: metal_command_failed status=5 error=-9\n");
}
#endif
