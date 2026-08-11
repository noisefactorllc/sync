#pragma once

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace noisefactor::sync::test {

using TestFunction = void (*)();

struct TestCase {
  std::string name;
  TestFunction function;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

class Registration {
 public:
  Registration(std::string name, TestFunction function) {
    registry().push_back({std::move(name), function});
  }
};

inline void require(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": " + expression);
  }
}

}  // namespace noisefactor::sync::test

#define SYNC_TEST(name) \
  static void name(); \
  static ::noisefactor::sync::test::Registration name##_registration(#name, &name); \
  static void name()

#define SYNC_REQUIRE(expression) \
  ::noisefactor::sync::test::require((expression), #expression, __FILE__, __LINE__)
