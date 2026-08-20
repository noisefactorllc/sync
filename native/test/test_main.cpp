#include "test_harness.hpp"

#include <exception>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif

namespace {

// Windows will happily stop a failing test by putting a dialog on a desktop
// nobody is looking at. On a CI runner that is not a failure, it is a hang:
// the process sits at the message box until something kills it, and a killed
// job publishes no logs. That is exactly how a Debug run of these tests spent
// a full hour and reported nothing.
//
// Three separate mechanisms can raise one, so all three are turned off:
//   - the Debug CRT's assert/error reports, which default to a message box
//     (this is why only Debug hung -- Release compiles assert() out, so the
//     same code ran to completion there);
//   - abort()'s "this application has requested the Runtime to terminate"
//     dialog;
//   - the OS-level hard-error and crash dialogs, including the one the loader
//     raises when a DLL cannot be found.
//
// Redirected to stderr instead, each of these becomes an immediate, readable
// failure attributed to the test named by the last RUN line.
void report_failures_without_blocking() noexcept {
#if defined(_WIN32)
  ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
#endif
#if defined(_MSC_VER)
  for (const int report : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
    _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
  }
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
}

}  // namespace

int main() {
  report_failures_without_blocking();

  // An exception that escapes a noexcept path -- or any other route to
  // std::terminate -- otherwise kills this process with no output at all,
  // leaving a RUN line and silence. Naming the exception turns that into a
  // diagnosis. abort() follows, because terminate must not return.
  std::set_terminate([] {
    std::cerr << "TERMINATE in the test above";
    if (std::current_exception() != nullptr) {
      try {
        std::rethrow_exception(std::current_exception());
      } catch (const std::exception& error) {
        std::cerr << ": " << error.what();
      } catch (...) {
        std::cerr << ": a non-standard exception";
      }
    }
    std::cerr << std::endl;
    std::abort();
  });

  int failures = 0;
  for (const auto& test : noisefactor::sync::test::registry()) {
    // Announced before the test runs, and flushed. ctest captures a test's
    // stdout into a pipe -- which is fully buffered -- and kills the process
    // outright when it exceeds its timeout, discarding whatever is still in
    // that buffer. Reporting only after a test completes therefore says
    // nothing at all about the one that hung, which is precisely when the
    // information is needed: a Windows CI job once spent its entire hour
    // inside this binary and named no test. The last RUN line in the log is
    // the test that did not finish.
    std::cout << "RUN " << test.name << std::endl;
    try {
      test.function();
      std::cout << "PASS " << test.name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (noisefactor::sync::test::registry().size() - failures) << "/"
            << noisefactor::sync::test::registry().size() << " tests passed\n";
  return failures == 0 ? 0 : 1;
}
