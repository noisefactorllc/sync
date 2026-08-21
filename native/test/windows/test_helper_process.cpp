// A controllable stand-in for syncd.exe, used only by
// companion_process_lifecycle_test.cpp.
//
// Why not launch the real syncd? Because the behaviour that matters here is
// exactly the behaviour syncd cannot be made to exhibit on demand: refusing
// to honour a console control event so the hard-kill watchdog is forced to
// run, or exiting instantly with a chosen status. Using the real helper
// would also bind TCP 53979 and touch the real pairing store under
// %LOCALAPPDATA%, making the tests collide with a Sync the developer
// happens to be running.
//
// The mode is read from the SYNC_TEST_HELPER_MODE environment variable
// rather than from argv, because the arguments are the thing under test:
// CompanionProcess picks them itself (`--publisher spout --publisher ndi`
// when supervising, `--list-pairings` or `--revoke-origin <origin>` when
// managing), so the fixture must not need them to decide how to behave.
// CreateProcessW is called with a null environment block, so the child
// inherits whatever the test set immediately before the call.
//
// Argv is instead *reported*, to SYNC_TEST_HELPER_ARGS_FILE, so a test can
// assert what was actually passed. SYNC_TEST_HELPER_PID_FILE reports this
// process's id, which is the only way a test can check that a management
// child -- launched with no handle the caller keeps -- really was killed.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace {

// Signalled by the console control handler; the main thread waits on it.
HANDLE g_stop = nullptr;

BOOL WINAPI control_handler(DWORD type) {
  if (type != CTRL_BREAK_EVENT && type != CTRL_C_EVENT) return FALSE;
  if (g_stop != nullptr) ::SetEvent(g_stop);
  return TRUE;  // handled: the process is not killed and gets to exit itself
}

// Deliberately claims the event as handled and then does nothing with it,
// so the process keeps running and CompanionProcess::terminate()'s
// termination_timeout_ms watchdog is the only thing that can stop it.
BOOL WINAPI deaf_handler(DWORD type) {
  return type == CTRL_BREAK_EVENT || type == CTRL_C_EVENT;
}

std::string mode_from_environment() {
  // Sized for the whole mode, not just its name: the management modes carry a
  // JSON payload, and a buffer that is merely "big enough for a word" turns
  // an over-long mode into a silent fall back to "graceful" -- which is a
  // fixture that answers the wrong question rather than one that fails.
  std::string buffer(4096, '\0');
  const DWORD length = ::GetEnvironmentVariableA(
      "SYNC_TEST_HELPER_MODE", buffer.data(),
      static_cast<DWORD>(buffer.size()));
  if (length == 0) return "graceful";
  if (length >= buffer.size()) {
    // Truncation would run the wrong mode. Say so instead: exit 2 is not a
    // status any mode here produces on purpose.
    std::fputs("helper mode too long for this fixture\n", stderr);
    std::exit(2);
  }
  return std::string(buffer.data(), length);
}

void write_stderr(const char* text) {
  std::fwrite(text, 1, std::strlen(text), stderr);
  std::fflush(stderr);
}

// Written with the Win32 API rather than stdio: MSVC deprecates fopen under
// /W4, and this file is Windows-only anyway, so reaching for CreateFile is
// simpler than suppressing a warning that is telling the truth.
//
// Every failure here is silent on purpose. These files exist for the test to
// read; a fixture that refused to run because it could not write one would
// turn a diagnostic aid into a second thing that can fail.
void write_file(const char* variable, std::string_view contents) {
  char path[MAX_PATH] = {};
  const DWORD length =
      ::GetEnvironmentVariableA(variable, path, sizeof(path));
  if (length == 0 || length >= sizeof(path)) return;
  const HANDLE file = ::CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  ::WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()),
              &written, nullptr);
  ::CloseHandle(file);
}

// Management commands are launched with real arguments (--list-pairings,
// --revoke-origin <origin>), and a test that cannot see them cannot prove
// they were passed correctly. When SYNC_TEST_HELPER_ARGS_FILE is set, every
// argument is written there one per line for the test to read back.
void record_arguments(int argc, char** argv) {
  std::string joined;
  for (int index = 1; index < argc; ++index) {
    joined += argv[index];
    joined += '\n';
  }
  write_file("SYNC_TEST_HELPER_ARGS_FILE", joined);
}

// Management children are launched with no handle the caller can reach, so a
// test cannot otherwise tell "the watchdog killed it" from "it happened to
// stop". Writing the pid where the test can read it makes that observable.
void record_pid() {
  write_file("SYNC_TEST_HELPER_PID_FILE",
             std::to_string(::GetCurrentProcessId()));
}

}  // namespace

int main(int argc, char** argv) {
  record_arguments(argc, argv);
  record_pid();
  const std::string mode = mode_from_environment();

  // Management modes. These stand in for syncd answering --list-pairings and
  // --revoke-origin: they write to stdout and exit, where the publisher modes
  // below write to stderr and stay alive.
  if (mode.rfind("say:", 0) == 0) {
    // "say:<exit>:<text>" -- writes text to stdout and exits with <exit>.
    const std::string rest = mode.substr(4);
    const std::size_t colon = rest.find(':');
    if (colon == std::string::npos) return 2;
    const int status = std::atoi(rest.substr(0, colon).c_str());
    const std::string text = rest.substr(colon + 1);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    return status;
  }
  if (mode.rfind("flood:", 0) == 0) {
    // Emits more bytes than the caller is willing to read, to prove the
    // output bound is enforced rather than assumed.
    const std::size_t bytes = static_cast<std::size_t>(
        std::atol(mode.c_str() + 6));
    const std::string chunk(4096, 'x');
    for (std::size_t written = 0; written < bytes; written += chunk.size()) {
      std::fwrite(chunk.data(), 1, chunk.size(), stdout);
    }
    std::fflush(stdout);
    return 0;
  }
  if (mode == "wedged") {
    // Never writes, never exits: only the caller's watchdog can end this.
    ::Sleep(INFINITE);
    return 0;
  }

  // "instant:<n>" exits immediately with status n, without ever installing a
  // handler -- the shape of a helper that dies on its own.
  if (mode.rfind("instant:", 0) == 0) {
    return std::atoi(mode.c_str() + 8);
  }

  if (mode == "stubborn") {
    ::SetConsoleCtrlHandler(deaf_handler, TRUE);
    write_stderr("stubborn helper ready\n");
    ::Sleep(INFINITE);
    return 0;
  }

  g_stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_stop == nullptr) return 2;
  ::SetConsoleCtrlHandler(control_handler, TRUE);

  if (mode == "chatty") {
    // Two separate writes so the reader has to survive the pipe delivering
    // the output in more than one chunk.
    write_stderr("first line from the helper\n");
    ::Sleep(20);
    write_stderr("second line from the helper\n");
  } else {
    write_stderr("helper ready\n");
  }

  ::WaitForSingleObject(g_stop, INFINITE);
  return 0;
}
