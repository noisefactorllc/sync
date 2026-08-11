#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include <sync/platform/pairing_prompt.hpp>

namespace noisefactor::sync::platform::pairing_prompt_testing {

inline constexpr std::size_t kMaximumPromptHeaderBytes = 64;
inline constexpr std::size_t kMaximumPromptMessageBytes = 768;
inline constexpr std::size_t kMaximumPromptButtonBytes = 16;

struct Presentation {
  [[nodiscard]] std::string_view header() const noexcept {
    return {header_bytes.data(), header_length};
  }
  [[nodiscard]] std::string_view message() const noexcept {
    return {message_bytes.data(), message_length};
  }
  [[nodiscard]] std::string_view default_button() const noexcept {
    return {default_button_bytes.data(), default_button_length};
  }
  [[nodiscard]] std::string_view alternate_button() const noexcept {
    return {alternate_button_bytes.data(), alternate_button_length};
  }

  std::array<char, kMaximumPromptHeaderBytes> header_bytes{};
  std::size_t header_length = 0;
  std::array<char, kMaximumPromptMessageBytes> message_bytes{};
  std::size_t message_length = 0;
  std::array<char, kMaximumPromptButtonBytes> default_button_bytes{};
  std::size_t default_button_length = 0;
  std::array<char, kMaximumPromptButtonBytes> alternate_button_bytes{};
  std::size_t alternate_button_length = 0;
  bool caution = true;
};

enum class AdapterResponse { Approved, Denied, SliceTimedOut, Failed };

class Adapter {
 public:
  virtual ~Adapter() = default;
  [[nodiscard]] virtual bool
  create(const Presentation& presentation,
         std::chrono::milliseconds ui_deadline) = 0;
  [[nodiscard]] virtual AdapterResponse
  receive(std::chrono::milliseconds slice) = 0;
  virtual void cancel() = 0;
  virtual void release() = 0;
};

[[nodiscard]] AdapterResponse decode_cf_response(
    std::int32_t status, std::uint64_t response_flags) noexcept;

class Factory {
 public:
  [[nodiscard]] static std::unique_ptr<MacPairingPrompt> create(
      std::unique_ptr<Adapter> adapter,
      std::chrono::milliseconds ui_deadline,
      std::chrono::milliseconds receive_slice);
};

}  // namespace noisefactor::sync::platform::pairing_prompt_testing
