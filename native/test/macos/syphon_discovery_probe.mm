#include <sync/platform/syphon_consumer.hpp>

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  const std::string_view explicit_path = argc == 2 ? std::string_view{argv[1]} : std::string_view{};
  noisefactor::sync::SyphonMetalConsumer consumer({.framework_path = explicit_path});
  std::cout << "{\"available\":" << (consumer.available() ? "true" : "false") << "}\n";
  return 0;
}
