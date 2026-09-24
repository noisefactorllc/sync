// The render helper's tests need a QGuiApplication (sockets, timers and the
// offscreen GL surface all hang off it), which the shared native test main
// cannot provide because it takes no arguments. Same registry, same RUN /
// PASS / FAIL lines, one application object for the whole run.

#include "test_harness.hpp"

#include <QGuiApplication>

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
#if defined(Q_OS_MACOS)
  qputenv("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM", "1");
#endif
  QGuiApplication app(argc, argv);
  int failures = 0;
  for (const auto& test : noisefactor::sync::test::registry()) {
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
