#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <sync/daemon_metrics.hpp>
#include <sync/pairing.hpp>

namespace noisefactor::sync::linux_control {

struct LinuxProviderStatus {
  std::array<char, 16> id{};
  bool selected = false;
  bool available = false;
  bool healthy = false;
  std::array<char, 160> reason{};
};

class LinuxRuntimeStatusSource {
 public:
  virtual ~LinuxRuntimeStatusSource() = default;
  [[nodiscard]] virtual auto providers(
      std::span<LinuxProviderStatus> output) const noexcept -> std::size_t = 0;
  [[nodiscard]] virtual auto metrics() const noexcept
      -> DaemonMetricsSnapshot = 0;
};

class LinuxRuntimeStatus final : public LinuxRuntimeStatusSource {
 public:
  explicit LinuxRuntimeStatus(const DaemonMetrics& metrics) noexcept;
  ~LinuxRuntimeStatus() noexcept override;
  void set_provider(std::string_view id, bool selected, bool available,
                    bool healthy, std::string_view reason) noexcept;
  [[nodiscard]] auto providers(
      std::span<LinuxProviderStatus> output) const noexcept
      -> std::size_t override;
  [[nodiscard]] auto metrics() const noexcept
      -> DaemonMetricsSnapshot override;

 private:
  struct State;
  std::unique_ptr<State> state_;
  const DaemonMetrics& metrics_;
};

class LinuxControlService final : public pairing::PairingPrompt {
 public:
  struct Options {
    std::string_view runtime_directory;
    std::uint32_t expected_uid = 0;
    pairing::PairingManagement* management = nullptr;
    LinuxRuntimeStatusSource* status = nullptr;
    // Test seam. Production obtains SO_PEERCRED directly when this is null.
    std::uint32_t (*peer_uid)(int descriptor) noexcept = nullptr;
  };

  explicit LinuxControlService(Options options);
  ~LinuxControlService() noexcept override;
  LinuxControlService(const LinuxControlService&) = delete;
  LinuxControlService& operator=(const LinuxControlService&) = delete;

  [[nodiscard]] auto start() noexcept -> bool;
  [[nodiscard]] auto socket_path() const noexcept -> std::string_view;
  [[nodiscard]] auto begin(const pairing::PromptRequest& request) noexcept
      -> bool override;
  [[nodiscard]] auto poll() noexcept -> pairing::PromptResult override;
  void cancel(std::uint64_t generation) noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace noisefactor::sync::linux_control
