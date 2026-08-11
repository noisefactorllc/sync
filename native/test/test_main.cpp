#include "test_harness.hpp"

int main() {
  int failures = 0;
  for (const auto& test : noisefactor::sync::test::registry()) {
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
