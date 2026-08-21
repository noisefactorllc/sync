#pragma once

#include <memory>

#include <sync/pairing.hpp>

namespace noisefactor::sync::platform {

namespace pairing_prompt_testing {
class Factory;
}

#if defined(__APPLE__)

class MacPairingPrompt final : public pairing::PairingPrompt {
 public:
  MacPairingPrompt();
  ~MacPairingPrompt() noexcept override;
  MacPairingPrompt(const MacPairingPrompt&) = delete;
  MacPairingPrompt& operator=(const MacPairingPrompt&) = delete;

  [[nodiscard]] bool begin(
      const pairing::PromptRequest& request) noexcept override;
  [[nodiscard]] pairing::PromptResult poll() noexcept override;
  void cancel(std::uint64_t generation) noexcept override;

 private:
  struct Impl;
  explicit MacPairingPrompt(std::unique_ptr<Impl> impl) noexcept;
  friend class pairing_prompt_testing::Factory;
  std::unique_ptr<Impl> impl_;
};

#endif  // defined(__APPLE__)

#if defined(_WIN32)

// Windows analogue of MacPairingPrompt: same non-blocking begin/poll/cancel
// contract, backed by a cancellable TaskDialog shown on a dedicated worker
// thread instead of CFUserNotification. See native/src/platform/windows/
// pairing_prompt.cpp for the implementation and pairing_prompt_internal.hpp
// for the adapter test seam.
class WindowsPairingPrompt final : public pairing::PairingPrompt {
 public:
  WindowsPairingPrompt();
  ~WindowsPairingPrompt() noexcept override;
  WindowsPairingPrompt(const WindowsPairingPrompt&) = delete;
  WindowsPairingPrompt& operator=(const WindowsPairingPrompt&) = delete;

  [[nodiscard]] bool begin(
      const pairing::PromptRequest& request) noexcept override;
  [[nodiscard]] pairing::PromptResult poll() noexcept override;
  void cancel(std::uint64_t generation) noexcept override;

 private:
  struct Impl;
  explicit WindowsPairingPrompt(std::unique_ptr<Impl> impl) noexcept;
  friend class pairing_prompt_testing::Factory;
  std::unique_ptr<Impl> impl_;
};

#endif  // defined(_WIN32)

}  // namespace noisefactor::sync::platform
