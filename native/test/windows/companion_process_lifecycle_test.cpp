// Lifecycle and concurrency tests for CompanionProcess.
//
// companion_process_test.cpp covers the pure functions (parsing, path
// resolution, revocation classification). This file covers the part that is
// not pure: launching a child, confining it to a job object, pumping its
// stderr, stopping it gracefully, killing it when it refuses, and -- the
// guarantee the whole class rests on -- never letting a background thread
// touch the object after its destructor returns.
//
// The child is native/test/windows/test_helper_process.cpp, not the real
// syncd: see the comment at the top of that file for why.

#include "../test_harness.hpp"

#include "../../src/platform/windows/companion_process.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using noisefactor::sync::companion::CompanionProcess;
using noisefactor::sync::companion::CompanionProcessOptions;
using noisefactor::sync::companion::HealthSnapshot;

// Long enough that a loaded CI runner does not fail a test that would pass,
// short enough that a genuine hang is reported as a failure rather than
// sitting there until ctest's own timeout fires. That second half is a real
// constraint, not a preference: there are twenty wait_for sites in this file
// and they share one 300-second ctest deadline with every test in this
// binary, so a budget of fifteen seconds each would let a systemic failure
// consume the whole allowance and report one anonymous timeout instead of
// individually named failures. Every operation waited on here completes in
// milliseconds when it works at all.
constexpr int kGenerousWaitMs = 5'000;

// The directory this test executable lives in, which is also where the
// build puts sync_test_helper_process.exe.
std::wstring executable_directory() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD written = ::GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (written == 0) return {};
    if (written < path.size()) {
      path.resize(written);
      break;
    }
    path.resize(path.size() * 2);
  }
  const std::size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  return path.substr(0, slash + 1);
}

std::wstring fake_helper_path() {
  return executable_directory() + L"sync_test_helper_process.exe";
}

void set_helper_mode(const wchar_t* mode) {
  ::SetEnvironmentVariableW(L"SYNC_TEST_HELPER_MODE", mode);
}

// Polls rather than sleeping a fixed interval so a fast machine finishes
// fast and a slow one still gets its full budget. Returns false on timeout,
// which every caller turns into a named failure.
template <typename Predicate>
bool wait_for(Predicate predicate, int timeout_ms = kGenerousWaitMs) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

// Callbacks run on whichever background thread produced them (these tests
// configure no dispatch_to_owner, so Impl::dispatch runs them inline), which
// is why every observation the tests make goes through one of these.
struct Recorder {
  std::mutex mutex;
  std::atomic<int> exits{0};
  std::atomic<int> last_exit_status{-1};
  std::string stderr_text;  // guarded by mutex

  CompanionProcess::StderrCallback stderr_callback() {
    return [this](std::string_view bytes) {
      std::lock_guard lock(mutex);
      stderr_text.append(bytes);
    };
  }
  CompanionProcess::ExitCallback exit_callback() {
    return [this](int status) {
      last_exit_status.store(status);
      exits.fetch_add(1);
    };
  }
  std::string stderr_snapshot() {
    std::lock_guard lock(mutex);
    return stderr_text;
  }
};

// A listening socket on an ephemeral port, so no test has to guess a port
// number that might already be in use on the machine running it.
class LoopbackListener {
 public:
  LoopbackListener() {
    WSADATA data{};
    started_ = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    if (!started_) return;
    socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) return;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;  // let the kernel choose
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::bind(socket_, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0) {
      return;
    }
    if (::listen(socket_, SOMAXCONN) != 0) return;
    sockaddr_in bound{};
    int length = sizeof(bound);
    if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&bound), &length) !=
        0) {
      return;
    }
    port_ = ::ntohs(bound.sin_port);
  }
  ~LoopbackListener() {
    close();
    if (started_) ::WSACleanup();
  }
  LoopbackListener(const LoopbackListener&) = delete;
  LoopbackListener& operator=(const LoopbackListener&) = delete;

  // Frees the port while keeping the number, so a caller can ask for a port
  // that is known to have been free a moment ago and is now unoccupied.
  void close() {
    if (socket_ != INVALID_SOCKET) {
      ::closesocket(socket_);
      socket_ = INVALID_SOCKET;
    }
  }
  [[nodiscard]] std::uint16_t port() const { return port_; }

 private:
  bool started_ = false;
  SOCKET socket_ = INVALID_SOCKET;
  std::uint16_t port_ = 0;
};

// Closes on every path out of a test, including the throw a failed
// SYNC_REQUIRE performs -- the harness catches it and runs the next test in
// the same process, so a leaked handle would outlive the failure.
class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE value) noexcept : value_(value) {}
  ~ScopedHandle() {
    if (value_ != nullptr) ::CloseHandle(value_);
  }
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ScopedHandle(ScopedHandle&& other) noexcept : value_(other.value_) {
    other.value_ = nullptr;
  }
  ScopedHandle& operator=(ScopedHandle&& other) noexcept {
    if (this != &other) {
      if (value_ != nullptr) ::CloseHandle(value_);
      value_ = other.value_;
      other.value_ = nullptr;
    }
    return *this;
  }
  [[nodiscard]] HANDLE get() const noexcept { return value_; }

 private:
  HANDLE value_ = nullptr;
};

CompanionProcessOptions fake_helper_options() {
  CompanionProcessOptions options;
  options.helper_path = fake_helper_path();
  // Checked here rather than left to start() failing: a missing fixture
  // otherwise surfaces as seven unrelated tests failing on start(), with
  // nothing pointing at the build.
  SYNC_REQUIRE(::GetFileAttributesW(options.helper_path.c_str()) !=
               INVALID_FILE_ATTRIBUTES);
  // Short enough to keep the hard-kill test quick, long enough that a
  // helper which does honour the console event wins the race on a loaded
  // machine rather than being killed and reporting a false negative.
  options.termination_timeout_ms = 1'500;
  options.health_timeout_ms = 400;
  return options;
}

}  // namespace

// ---------------------------------------------------------------------
// Operations with no helper running
// ---------------------------------------------------------------------

SYNC_TEST(windows_terminate_without_a_helper_still_completes_exactly_once) {
  std::atomic<int> completions{0};
  {
    CompanionProcess companion(fake_helper_options());
    companion.terminate([&completions] { completions.fetch_add(1); });
    SYNC_REQUIRE(wait_for([&] { return completions.load() == 1; }));
  }
  // Nothing may fire a second time during destruction.
  SYNC_REQUIRE(completions.load() == 1);
}

SYNC_TEST(windows_probe_reports_unavailable_when_nothing_is_listening) {
  LoopbackListener listener;
  SYNC_REQUIRE(listener.port() != 0);
  listener.close();  // the port number is now known-free

  CompanionProcessOptions options = fake_helper_options();
  options.port = listener.port();

  std::atomic<bool> done{false};
  bool had_health = true;
  std::string message;
  {
    CompanionProcess companion(std::move(options));
    companion.probe(
        [&](std::optional<HealthSnapshot> health, std::string reason) {
          had_health = health.has_value();
          message = std::move(reason);
          done.store(true);
        });
    SYNC_REQUIRE(wait_for([&] { return done.load(); }));
  }
  SYNC_REQUIRE(!had_health);
  SYNC_REQUIRE(message == "Sync status was unavailable.");
}

SYNC_TEST(windows_probe_reports_an_occupied_port_as_incompatible) {
  // A socket that accepts connections but never speaks HTTP is exactly the
  // "something else already holds 53979" case the tray has to distinguish
  // from "syncd is not running", because the remedies differ.
  LoopbackListener listener;
  SYNC_REQUIRE(listener.port() != 0);

  CompanionProcessOptions options = fake_helper_options();
  options.port = listener.port();

  std::atomic<bool> done{false};
  std::optional<HealthSnapshot> observed;
  std::string message;
  {
    CompanionProcess companion(std::move(options));
    companion.probe(
        [&](std::optional<HealthSnapshot> health, std::string reason) {
          observed = std::move(health);
          message = std::move(reason);
          done.store(true);
        });
    SYNC_REQUIRE(wait_for([&] { return done.load(); }));
  }
  SYNC_REQUIRE(observed.has_value());
  SYNC_REQUIRE(observed->reachable);
  SYNC_REQUIRE(!observed->compatible);
  SYNC_REQUIRE(message.find("is occupied by an incompatible service") !=
               std::string::npos);
}

SYNC_TEST(windows_start_fails_without_launching_when_the_helper_is_missing) {
  CompanionProcessOptions options = fake_helper_options();
  options.helper_path = executable_directory() + L"no-such-helper-here.exe";

  Recorder recorder;
  CompanionProcess companion(std::move(options));
  std::string error;
  const bool started = companion.start(recorder.stderr_callback(),
                                       recorder.exit_callback(), error);
  SYNC_REQUIRE(!started);
  SYNC_REQUIRE(!error.empty());
  SYNC_REQUIRE(!companion.owned_pid().has_value());
  // A failed start must not report an exit for a process that never ran.
  SYNC_REQUIRE(recorder.exits.load() == 0);
}

// ---------------------------------------------------------------------
// Launching, supervising, and stopping a real child process
// ---------------------------------------------------------------------

SYNC_TEST(windows_start_launches_the_helper_and_pumps_its_stderr) {
  set_helper_mode(L"chatty");
  Recorder recorder;
  CompanionProcess companion(fake_helper_options());

  std::string error;
  SYNC_REQUIRE(companion.start(recorder.stderr_callback(),
                               recorder.exit_callback(), error));
  SYNC_REQUIRE(error.empty());
  SYNC_REQUIRE(companion.owned_pid().has_value());

  // Both writes must arrive, which is the part a single-ReadFile
  // implementation would get wrong.
  SYNC_REQUIRE(wait_for([&] {
    const std::string text = recorder.stderr_snapshot();
    return text.find("first line from the helper") != std::string::npos &&
           text.find("second line from the helper") != std::string::npos;
  }));

  std::atomic<bool> stopped{false};
  companion.terminate([&stopped] { stopped.store(true); });
  SYNC_REQUIRE(wait_for([&] { return stopped.load(); }));
}

SYNC_TEST(windows_starting_twice_is_refused_while_the_helper_runs) {
  set_helper_mode(L"graceful");
  Recorder recorder;
  CompanionProcess companion(fake_helper_options());

  std::string error;
  SYNC_REQUIRE(companion.start(recorder.stderr_callback(),
                               recorder.exit_callback(), error));
  const std::optional<int> first_pid = companion.owned_pid();
  SYNC_REQUIRE(first_pid.has_value());

  std::string second_error;
  SYNC_REQUIRE(!companion.start(recorder.stderr_callback(),
                                recorder.exit_callback(), second_error));
  SYNC_REQUIRE(!second_error.empty());
  // The refusal must leave the first helper untouched, not replace it.
  SYNC_REQUIRE(companion.owned_pid() == first_pid);

  std::atomic<bool> stopped{false};
  companion.terminate([&stopped] { stopped.store(true); });
  SYNC_REQUIRE(wait_for([&] { return stopped.load(); }));
}

SYNC_TEST(windows_terminate_stops_a_cooperative_helper_gracefully) {
  set_helper_mode(L"graceful");
  Recorder recorder;
  CompanionProcess companion(fake_helper_options());

  std::string error;
  SYNC_REQUIRE(companion.start(recorder.stderr_callback(),
                               recorder.exit_callback(), error));
  // Wait until the helper has installed its console control handler --
  // announced by the line it writes -- or the event could be delivered
  // before there is anything to receive it.
  SYNC_REQUIRE(wait_for([&] {
    return recorder.stderr_snapshot().find("helper ready") != std::string::npos;
  }));

  std::atomic<bool> stopped{false};
  companion.terminate([&stopped] { stopped.store(true); });
  SYNC_REQUIRE(wait_for([&] { return stopped.load(); }));
  SYNC_REQUIRE(wait_for([&] { return recorder.exits.load() == 1; }));
  // Exit status 0 is the whole point: it distinguishes a helper that was
  // asked to stop and did from one that had to be killed. This is the same
  // property scripts/smoke-windows-app.ps1 asserts end to end, proven here
  // against the code path rather than the packaged app.
  SYNC_REQUIRE(recorder.last_exit_status.load() == 0);
  SYNC_REQUIRE(!companion.owned_pid().has_value());
}

SYNC_TEST(windows_terminate_kills_a_helper_that_ignores_the_console_event) {
  set_helper_mode(L"stubborn");
  Recorder recorder;
  CompanionProcess companion(fake_helper_options());

  std::string error;
  SYNC_REQUIRE(companion.start(recorder.stderr_callback(),
                               recorder.exit_callback(), error));
  SYNC_REQUIRE(wait_for([&] {
    return recorder.stderr_snapshot().find("stubborn helper ready") !=
           std::string::npos;
  }));

  const auto requested = std::chrono::steady_clock::now();
  std::atomic<bool> stopped{false};
  companion.terminate([&stopped] { stopped.store(true); });
  SYNC_REQUIRE(wait_for([&] { return stopped.load(); }));
  const auto elapsed = std::chrono::steady_clock::now() - requested;

  SYNC_REQUIRE(wait_for([&] { return recorder.exits.load() == 1; }));
  // Killed, not stopped: TerminateProcess(process, 1) is what ran.
  SYNC_REQUIRE(recorder.last_exit_status.load() == 1);
  // Bracketed, not merely bounded. The lower bound is what proves the
  // watchdog is the thing that killed it: this helper claims the console
  // event and then ignores it, so nothing else can end it, and anything
  // faster than the configured 1500ms would mean it died for some other
  // reason and the test was passing by accident. The upper bound catches a
  // watchdog that was silently lengthened.
  SYNC_REQUIRE(elapsed >= std::chrono::milliseconds(1'000));
  SYNC_REQUIRE(elapsed < std::chrono::seconds(6));
  SYNC_REQUIRE(!companion.owned_pid().has_value());
}

SYNC_TEST(windows_terminate_is_idempotent_and_runs_every_completion) {
  set_helper_mode(L"graceful");
  Recorder recorder;
  CompanionProcess companion(fake_helper_options());

  std::string error;
  SYNC_REQUIRE(companion.start(recorder.stderr_callback(),
                               recorder.exit_callback(), error));
  SYNC_REQUIRE(wait_for([&] {
    return recorder.stderr_snapshot().find("helper ready") != std::string::npos;
  }));

  // Several terminate() calls racing from different threads is what a user
  // clicking Quit twice, plus the window closing, actually produces. Every
  // completion must run exactly once and only one shutdown may be started.
  constexpr int kRequests = 8;
  std::atomic<int> completions{0};
  std::vector<std::thread> threads;
  threads.reserve(kRequests);
  for (int i = 0; i < kRequests; ++i) {
    threads.emplace_back([&companion, &completions] {
      companion.terminate([&completions] { completions.fetch_add(1); });
    });
  }
  for (auto& thread : threads) thread.join();

  SYNC_REQUIRE(wait_for([&] { return completions.load() == kRequests; }));
  SYNC_REQUIRE(wait_for([&] { return recorder.exits.load() == 1; }));
  SYNC_REQUIRE(recorder.last_exit_status.load() == 0);

  // A terminate() after the helper is already gone still completes, and
  // still does not produce a second exit notification.
  std::atomic<bool> late{false};
  companion.terminate([&late] { late.store(true); });
  SYNC_REQUIRE(wait_for([&] { return late.load(); }));
  SYNC_REQUIRE(recorder.exits.load() == 1);
  SYNC_REQUIRE(completions.load() == kRequests);
}

SYNC_TEST(windows_a_helper_that_exits_on_its_own_is_reported_and_disowned) {
  set_helper_mode(L"instant:7");
  Recorder recorder;
  CompanionProcess companion(fake_helper_options());

  std::string error;
  SYNC_REQUIRE(companion.start(recorder.stderr_callback(),
                               recorder.exit_callback(), error));
  SYNC_REQUIRE(wait_for([&] { return recorder.exits.load() == 1; }));
  SYNC_REQUIRE(recorder.last_exit_status.load() == 7);
  // Ownership must be released, or a restart would be refused forever.
  SYNC_REQUIRE(wait_for([&] { return !companion.owned_pid().has_value(); }));

  // And a restart must therefore succeed.
  set_helper_mode(L"graceful");
  std::string restart_error;
  SYNC_REQUIRE(companion.start(recorder.stderr_callback(),
                               recorder.exit_callback(), restart_error));
  SYNC_REQUIRE(companion.owned_pid().has_value());
  std::atomic<bool> stopped{false};
  companion.terminate([&stopped] { stopped.store(true); });
  SYNC_REQUIRE(wait_for([&] { return stopped.load(); }));
}

// ---------------------------------------------------------------------
// The supervision and lifetime guarantees
// ---------------------------------------------------------------------

SYNC_TEST(windows_destroying_the_companion_kills_the_helper) {
  // The Windows analogue of the macOS supervision guarantee: a helper must
  // never outlive its supervisor, because it would keep holding TCP 53979
  // with nothing left to manage it. Here the kill comes from closing the
  // job object's last handle, so it holds even for a helper that ignores
  // console control events entirely.
  set_helper_mode(L"stubborn");
  Recorder recorder;

  ScopedHandle helper{nullptr};
  {
    CompanionProcess companion(fake_helper_options());
    std::string error;
    SYNC_REQUIRE(companion.start(recorder.stderr_callback(),
                                 recorder.exit_callback(), error));
    const std::optional<int> pid = companion.owned_pid();
    SYNC_REQUIRE(pid.has_value());
    // Opened while the helper is definitely alive: holding this handle also
    // keeps the pid from being recycled under us, so the wait below cannot
    // accidentally observe some unrelated process.
    helper = ScopedHandle(::OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
        static_cast<DWORD>(*pid)));
    SYNC_REQUIRE(helper.get() != nullptr);
    SYNC_REQUIRE(::WaitForSingleObject(helper.get(), 0) == WAIT_TIMEOUT);
  }  // ~CompanionProcess

  // Already dead by the time the destructor returned -- not merely dying.
  SYNC_REQUIRE(::WaitForSingleObject(helper.get(), 0) == WAIT_OBJECT_0);
}

SYNC_TEST(windows_the_destructor_waits_for_every_outstanding_probe) {
  // Every probe runs on its own detached thread and touches Impl when it
  // finishes. If the destructor returned before they did, each one would be
  // a use-after-free. Point them at a dead port so they are slow enough
  // (health_timeout_ms plus the reachability check) to still be in flight
  // when the destructor runs.
  LoopbackListener listener;
  SYNC_REQUIRE(listener.port() != 0);
  listener.close();

  CompanionProcessOptions options = fake_helper_options();
  options.port = listener.port();

  constexpr int kProbes = 16;
  std::atomic<int> completions{0};
  {
    CompanionProcess companion(std::move(options));
    for (int i = 0; i < kProbes; ++i) {
      companion.probe(
          [&completions](std::optional<HealthSnapshot>, std::string) {
            completions.fetch_add(1);
          });
    }
  }  // ~CompanionProcess must not return until all sixteen have finished
  SYNC_REQUIRE(completions.load() == kProbes);
}

SYNC_TEST(windows_the_destructor_drains_work_queued_from_many_threads) {
  // A helper is started first on purpose. Without one, terminate() takes
  // its no-helper path and completes synchronously, so counting those
  // completions would assert nothing about the drain -- they would all have
  // run before the threads were even joined.
  set_helper_mode(L"graceful");
  Recorder recorder;
  LoopbackListener listener;
  SYNC_REQUIRE(listener.port() != 0);
  listener.close();

  CompanionProcessOptions options = fake_helper_options();
  options.port = listener.port();

  std::atomic<int> completions{0};
  std::atomic<int> terminations{0};
  constexpr int kThreads = 8;
  {
    CompanionProcess companion(std::move(options));
    std::string error;
    SYNC_REQUIRE(companion.start(recorder.stderr_callback(),
                                 recorder.exit_callback(), error));
    SYNC_REQUIRE(wait_for([&] {
      return recorder.stderr_snapshot().find("helper ready") !=
             std::string::npos;
    }));
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
      threads.emplace_back([&] {
        companion.probe(
            [&completions](std::optional<HealthSnapshot>, std::string) {
              completions.fetch_add(1);
            });
        companion.terminate([&terminations] { terminations.fetch_add(1); });
      });
    }
    for (auto& thread : threads) thread.join();
  }
  SYNC_REQUIRE(completions.load() == kThreads);
  SYNC_REQUIRE(terminations.load() == kThreads);
  // Deliberately NOT asserting that the exit callback fired. The destructor
  // clears stderr_callback and exit_callback before closing the job, because
  // once it returns nothing may call back into whatever the owner captured.
  // The helper here is usually still stopping at that moment, so its exit is
  // reported to nobody -- by design. Termination completions are different:
  // somebody is waiting on each one, so they must all still run, which is
  // what the two assertions above check.
}

SYNC_TEST(windows_a_queueing_dispatcher_is_honoured_and_may_fall_back_inline) {
  // The documented contract for dispatch_to_owner: callbacks are marshalled
  // to the owner thread, and destruction is only safe if the dispatcher can
  // still run them -- app_main.cpp satisfies this by invoking them inline
  // once its message loop is gone. This test is that arrangement in
  // miniature, and it hangs if the inline fallback is ever removed.
  LoopbackListener listener;
  SYNC_REQUIRE(listener.port() != 0);
  listener.close();

  struct Dispatcher {
    std::mutex mutex;
    std::deque<std::function<void()>> queue;
    bool inline_mode = false;

    void post(std::function<void()> callback) {
      {
        std::lock_guard lock(mutex);
        if (!inline_mode) {
          queue.push_back(std::move(callback));
          return;
        }
      }
      callback();  // outside the lock, exactly as app_main's fallback does
    }
    int drain() {
      std::deque<std::function<void()>> pending;
      {
        std::lock_guard lock(mutex);
        pending.swap(queue);
      }
      for (auto& callback : pending) callback();
      return static_cast<int>(pending.size());
    }
    void go_inline() {
      std::lock_guard lock(mutex);
      inline_mode = true;
    }
  };

  Dispatcher dispatcher;
  CompanionProcessOptions options = fake_helper_options();
  options.port = listener.port();
  options.dispatch_to_owner = [&dispatcher](std::function<void()> callback) {
    dispatcher.post(std::move(callback));
  };

  std::atomic<int> completions{0};
  {
    CompanionProcess companion(std::move(options));
    // Declared after `companion`, so it is destroyed BEFORE it -- which is
    // the whole point. Every assertion below can throw, and if one does while
    // the dispatcher is still queueing, ~CompanionProcess would wait for a
    // completion that only a drain could deliver and nothing would ever drain
    // it again: a failed assertion would become a permanent hang, taking out
    // every other test in this binary instead of reporting one failure.
    struct FallBackInlineOnExit {
      Dispatcher& dispatcher;
      ~FallBackInlineOnExit() {
        dispatcher.go_inline();
        dispatcher.drain();
      }
    } fall_back_inline{dispatcher};

    companion.probe([&completions](std::optional<HealthSnapshot>, std::string) {
      completions.fetch_add(1);
    });
    // Nothing has run yet: the callback is sitting in the owner's queue.
    SYNC_REQUIRE(wait_for([&] {
      std::lock_guard lock(dispatcher.mutex);
      return !dispatcher.queue.empty();
    }));
    SYNC_REQUIRE(completions.load() == 0);
    SYNC_REQUIRE(dispatcher.drain() == 1);
    SYNC_REQUIRE(completions.load() == 1);

    // Queue one more and then tear down without draining it by hand, the
    // way a real shutdown does: the guard above switches the dispatcher to
    // inline just before ~CompanionProcess runs, exactly as app_main.cpp
    // does once its message loop has exited. If that fallback is removed
    // from either place, this blocks.
    companion.probe([&completions](std::optional<HealthSnapshot>, std::string) {
      completions.fetch_add(1);
    });
  }
  SYNC_REQUIRE(completions.load() == 2);
}

// ---------------------------------------------------------------------
// Management commands
//
// list_pairings() and revoke_pairing() run syncd a second time, briefly, to
// read or change the pairing store. Until now nothing exercised them, and
// they are the one path that launches a child the caller never gets a handle
// to -- so the fixture reports its own pid and arguments through files the
// test reads back.
// ---------------------------------------------------------------------

namespace {

// A scratch file the fixture writes and the test reads. Removed on the way in
// and out, so a stale file from an earlier test can never be mistaken for
// evidence that this one launched anything.
class ScratchFile {
 public:
  explicit ScratchFile(const wchar_t* variable) : variable_(variable) {
    std::wstring directory(MAX_PATH, L'\0');
    const DWORD written =
        ::GetTempPathW(static_cast<DWORD>(directory.size()), directory.data());
    directory.resize(written);
    path_ = directory + L"sync-fixture-" + variable_ + L".txt";
    ::DeleteFileW(path_.c_str());
    ::SetEnvironmentVariableW(variable_.c_str(), path_.c_str());
  }
  ~ScratchFile() {
    ::SetEnvironmentVariableW(variable_.c_str(), nullptr);
    ::DeleteFileW(path_.c_str());
  }
  ScratchFile(const ScratchFile&) = delete;
  ScratchFile& operator=(const ScratchFile&) = delete;

  [[nodiscard]] bool exists() const {
    return ::GetFileAttributesW(path_.c_str()) != INVALID_FILE_ATTRIBUTES;
  }
  [[nodiscard]] std::string read() const {
    const HANDLE file =
        ::CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string contents;
    std::array<char, 1024> buffer{};
    for (;;) {
      DWORD read = 0;
      if (::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                     &read, nullptr) == 0 ||
          read == 0) {
        break;
      }
      contents.append(buffer.data(), read);
    }
    ::CloseHandle(file);
    return contents;
  }

 private:
  std::wstring variable_;
  std::wstring path_;
};

constexpr std::string_view kTwoPairings =
    R"({"type":"pairings","origins":["https://one.example","http://localhost:8080"]})";

// Builds a "say:<exit>:<text>" mode for the fixture.
std::wstring say(int status, std::string_view text) {
  std::wstring mode = L"say:" + std::to_wstring(status) + L":";
  mode.append(text.begin(), text.end());  // the payloads here are all ASCII
  return mode;
}

}  // namespace

SYNC_TEST(windows_list_pairings_passes_its_argument_and_parses_the_origins) {
  ScratchFile arguments(L"SYNC_TEST_HELPER_ARGS_FILE");
  set_helper_mode(say(0, kTwoPairings).c_str());

  CompanionProcess companion(fake_helper_options());
  std::atomic<bool> done{false};
  std::vector<std::string> origins;
  std::string error;
  companion.list_pairings([&](std::vector<std::string> found,
                              std::string reason) {
    origins = std::move(found);
    error = std::move(reason);
    done.store(true);
  });
  SYNC_REQUIRE(wait_for([&] { return done.load(); }));

  SYNC_REQUIRE(error.empty());
  SYNC_REQUIRE(origins.size() == 2);
  SYNC_REQUIRE(origins[0] == "https://one.example");
  SYNC_REQUIRE(origins[1] == "http://localhost:8080");
  // The helper is asked for pairings and nothing else. Asserting the argument
  // is the only way to know the tray is not, say, silently starting a daemon.
  SYNC_REQUIRE(arguments.read() == "--list-pairings\n");
}

SYNC_TEST(windows_list_pairings_reports_a_failing_helper_without_origins) {
  set_helper_mode(say(1, kTwoPairings).c_str());

  CompanionProcess companion(fake_helper_options());
  std::atomic<bool> done{false};
  std::vector<std::string> origins;
  std::string error;
  companion.list_pairings([&](std::vector<std::string> found,
                              std::string reason) {
    origins = std::move(found);
    error = std::move(reason);
    done.store(true);
  });
  SYNC_REQUIRE(wait_for([&] { return done.load(); }));

  // Well-formed output must not be believed just because it parses: the exit
  // status is what says whether the helper actually read the store.
  SYNC_REQUIRE(!error.empty());
  SYNC_REQUIRE(origins.empty());
}

SYNC_TEST(windows_list_pairings_rejects_output_that_is_not_a_pairings_record) {
  set_helper_mode(say(0, "this is not json").c_str());

  CompanionProcess companion(fake_helper_options());
  std::atomic<bool> done{false};
  std::vector<std::string> origins;
  std::string error;
  companion.list_pairings([&](std::vector<std::string> found,
                              std::string reason) {
    origins = std::move(found);
    error = std::move(reason);
    done.store(true);
  });
  SYNC_REQUIRE(wait_for([&] { return done.load(); }));
  SYNC_REQUIRE(!error.empty());
  SYNC_REQUIRE(origins.empty());
}

SYNC_TEST(windows_management_output_is_bounded_and_never_reported_as_pairings) {
  // Twice the reader's ceiling. The reader truncates at exactly that ceiling,
  // so the "output too large" branch in list_pairings is in fact unreachable
  // -- the truncated bytes simply fail to parse. Either way the contract that
  // matters holds: unbounded output from the helper cannot exhaust memory
  // here, and cannot be reported as a list of pairings.
  set_helper_mode(L"flood:131072");

  CompanionProcess companion(fake_helper_options());
  std::atomic<bool> done{false};
  std::vector<std::string> origins;
  std::string error;
  companion.list_pairings([&](std::vector<std::string> found,
                              std::string reason) {
    origins = std::move(found);
    error = std::move(reason);
    done.store(true);
  });
  SYNC_REQUIRE(wait_for([&] { return done.load(); }));
  SYNC_REQUIRE(!error.empty());
  SYNC_REQUIRE(origins.empty());
}

SYNC_TEST(windows_a_wedged_management_helper_is_bounded_and_killed) {
  ScratchFile pid_file(L"SYNC_TEST_HELPER_PID_FILE");
  set_helper_mode(L"wedged");

  CompanionProcessOptions options = fake_helper_options();
  options.management_timeout_ms = 1'200;

  const auto requested = std::chrono::steady_clock::now();
  std::atomic<bool> done{false};
  std::string error;
  {
    CompanionProcess companion(std::move(options));
    companion.list_pairings([&](std::vector<std::string>, std::string reason) {
      error = std::move(reason);
      done.store(true);
    });
    SYNC_REQUIRE(wait_for([&] { return done.load(); }));
  }
  const auto elapsed = std::chrono::steady_clock::now() - requested;

  SYNC_REQUIRE(!error.empty());
  // Bracketed for the same reason as the helper hard-kill test: below the
  // configured timeout would mean something other than the watchdog ended it.
  SYNC_REQUIRE(elapsed >= std::chrono::milliseconds(800));
  SYNC_REQUIRE(elapsed < std::chrono::seconds(8));

  // And it is actually gone. A management child is launched without a handle
  // the caller keeps, so nothing else here would notice it surviving -- which
  // matters because a live one holds the pairing store's lock file and would
  // make the next syncd fail to open the store.
  SYNC_REQUIRE(pid_file.exists());
  const DWORD pid = static_cast<DWORD>(std::stoul(pid_file.read()));
  ScopedHandle child{::OpenProcess(SYNCHRONIZE, FALSE, pid)};
  if (child.get() != nullptr) {
    SYNC_REQUIRE(::WaitForSingleObject(child.get(), 5'000) == WAIT_OBJECT_0);
  }
}

SYNC_TEST(windows_revoke_pairing_refuses_a_non_canonical_origin_unlaunched) {
  ScratchFile arguments(L"SYNC_TEST_HELPER_ARGS_FILE");
  set_helper_mode(say(0, R"({"type":"revocation"})").c_str());

  CompanionProcess companion(fake_helper_options());
  std::atomic<bool> done{false};
  bool revoked = true;
  std::string error;
  companion.revoke_pairing("HTTPS://ONE.EXAMPLE:443",
                           [&](bool ok, std::string reason) {
                             revoked = ok;
                             error = std::move(reason);
                             done.store(true);
                           });
  SYNC_REQUIRE(wait_for([&] { return done.load(); }));

  SYNC_REQUIRE(!revoked);
  SYNC_REQUIRE(!error.empty());
  // Rejected here, not by the helper. An origin that survived normalisation
  // unchanged is the only thing that may reach the store, so a
  // non-canonical one must never become a command line at all.
  SYNC_REQUIRE(!arguments.exists());
}

SYNC_TEST(windows_revoke_pairing_passes_the_origin_and_accepts_a_durable_record) {
  ScratchFile arguments(L"SYNC_TEST_HELPER_ARGS_FILE");
  set_helper_mode(
      say(0,
          R"({"type":"revocation","origin":"https://one.example","status":"revoked"})")
          .c_str());

  CompanionProcess companion(fake_helper_options());
  std::atomic<bool> done{false};
  bool revoked = false;
  std::string error;
  companion.revoke_pairing("https://one.example",
                           [&](bool ok, std::string reason) {
                             revoked = ok;
                             error = std::move(reason);
                             done.store(true);
                           });
  SYNC_REQUIRE(wait_for([&] { return done.load(); }));

  SYNC_REQUIRE(revoked);
  SYNC_REQUIRE(error.empty());
  SYNC_REQUIRE(arguments.read() == "--revoke-origin\nhttps://one.example\n");
}

SYNC_TEST(windows_revoke_pairing_refuses_to_call_an_uncertain_record_revoked) {
  // syncd exits 3 when the revocation reached the store but could not be
  // proven durable. Reporting that as success would tell someone a browser
  // had been unpaired when a power cut could bring it back.
  set_helper_mode(
      say(3,
          R"({"type":"revocation","origin":"https://one.example","status":"revoked_durability_uncertain"})")
          .c_str());

  CompanionProcess companion(fake_helper_options());
  std::atomic<bool> done{false};
  bool revoked = true;
  std::string error;
  companion.revoke_pairing("https://one.example",
                           [&](bool ok, std::string reason) {
                             revoked = ok;
                             error = std::move(reason);
                             done.store(true);
                           });
  SYNC_REQUIRE(wait_for([&] { return done.load(); }));

  SYNC_REQUIRE(!revoked);
  SYNC_REQUIRE(!error.empty());
}

SYNC_TEST(windows_the_destructor_waits_for_a_management_command_in_flight) {
  // The management child is not the supervised helper and is not reached by
  // the job the destructor closes; only its own watchdog ends it. So the
  // destructor has to wait for that, and this is the test that says it does.
  ScratchFile pid_file(L"SYNC_TEST_HELPER_PID_FILE");
  set_helper_mode(L"wedged");

  CompanionProcessOptions options = fake_helper_options();
  options.management_timeout_ms = 1'000;

  std::atomic<int> completions{0};
  {
    CompanionProcess companion(std::move(options));
    companion.list_pairings(
        [&completions](std::vector<std::string>, std::string) {
          completions.fetch_add(1);
        });
    companion.revoke_pairing(
        "https://one.example",
        [&completions](bool, std::string) { completions.fetch_add(1); });
  }  // ~CompanionProcess

  // Both answered before the destructor returned, or they were running
  // against freed state.
  SYNC_REQUIRE(completions.load() == 2);
}
