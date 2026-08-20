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
// rather than from argv, because CompanionProcess::launch_arguments() is
// fixed (`--publisher spout --publisher ndi`) and the point of these tests
// is to exercise the real launch path, argument list included, rather than
// a special one. CreateProcessW is called with a null environment block, so
// the child inherits whatever the test set immediately before start().

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
  char buffer[64] = {};
  const DWORD length = ::GetEnvironmentVariableA("SYNC_TEST_HELPER_MODE",
                                                 buffer, sizeof(buffer));
  if (length == 0 || length >= sizeof(buffer)) return "graceful";
  return std::string(buffer, length);
}

void write_stderr(const char* text) {
  std::fwrite(text, 1, std::strlen(text), stderr);
  std::fflush(stderr);
}

}  // namespace

int main() {
  const std::string mode = mode_from_environment();

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
