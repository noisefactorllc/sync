#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <sync/frame_receiver.hpp>

namespace noisefactor::sync {

// Direct CPU-frame send provider for NDI. Available on every desktop
// platform Sync targets; unlike MetalFramePublisher this class carries no
// platform #error guard because the NDI runtime is discovered dynamically
// (see ndi_publisher.cpp) rather than linked, so the same translation unit
// builds on Windows, macOS, and Linux.
class NdiFramePublisher final : public FramePublisher {
 public:
  static constexpr std::size_t kMaximumSenderEntries = 8;
  static constexpr std::size_t kProductAllocationBudgetBytes = 512ULL * 1024ULL * 1024ULL;

  struct Options {
    // Directory to search first, ahead of the vendor's documented
    // NDI_RUNTIME_DIR_V6 / NDI_RUNTIME_DIR_V5 environment variables. Wired
    // from the --ndi-runtime CLI flag by the caller; empty means "rely on
    // the documented discovery variables and the OS loader's default
    // search only".
    std::string_view runtime_path{};
    std::size_t allocation_budget_bytes = kProductAllocationBudgetBytes;
  };

  NdiFramePublisher();
  explicit NdiFramePublisher(Options options);
  ~NdiFramePublisher() override;

  NdiFramePublisher(const NdiFramePublisher&) = delete;
  auto operator=(const NdiFramePublisher&) -> NdiFramePublisher& = delete;
  NdiFramePublisher(NdiFramePublisher&&) = delete;
  auto operator=(NdiFramePublisher&&) -> NdiFramePublisher& = delete;

  // False whenever the NDI runtime could not be found, failed its ABI
  // probe, or reported an unsupported CPU. Absence is never an error: a
  // caller simply does not offer this provider when available() is false.
  [[nodiscard]] auto available() const noexcept -> bool;

  auto open_sender(std::string_view sender_id, std::string_view name) noexcept -> bool override;
  void close_sender(std::string_view sender_id) noexcept override;
  auto publish(std::string_view sender_id, const protocol::FrameView& frame) noexcept
      -> PublishResult override;
  auto poll_failure(std::uint64_t now_ms) noexcept
      -> std::optional<ProviderFailure> override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace noisefactor::sync
