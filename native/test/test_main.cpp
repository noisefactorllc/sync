#include "test_harness.hpp"

int main() {
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
