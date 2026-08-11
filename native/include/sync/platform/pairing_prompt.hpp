#pragma once

#include <memory>

#include <sync/pairing.hpp>

namespace noisefactor::sync::platform {

namespace pairing_prompt_testing {
class Factory;
}

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

}  // namespace noisefactor::sync::platform
