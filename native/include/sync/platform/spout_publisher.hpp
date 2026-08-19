#pragma once

#if !defined(_WIN32)
#error "spout_publisher.hpp is available only on Windows"
#endif

#include <cstddef>
#include <memory>
#include <string_view>

#include <sync/frame_receiver.hpp>

namespace noisefactor::sync {

// Direct CPU-RGBA send provider for Spout (Windows). Spout's public ABI is a
// runtime-loaded C export (SpoutLibrary.dll's GetSpout()) that hands back a
// stable vtable, so -- exactly like SyphonMetalConsumer on macOS -- this type
// never links Spout at build time and never vendors its headers. See
// docs/dependencies/spout.md for the discovery and ABI contract, and
// spout_publisher.cpp for the reconstructed vtable and its VERIFY notes.
class SpoutFramePublisher final : public FramePublisher {
 public:
  static constexpr std::size_t kMaximumSenderEntries = 8;

  // 512 MiB, mirroring MetalFramePublisher::kProductAllocationBudgetBytes: the
  // two CPU providers (Spout, NDI) and the GPU provider are independent
  // budgets, not a shared one, so matching the existing product number here
  // is a deliberate, not incidental, choice.
  static constexpr std::size_t kProductAllocationBudgetBytes = 512ULL * 1024ULL * 1024ULL;

  struct Options {
    // Explicit path from the --spout-library CLI flag. Empty means "search
    // the bounded default locations described in spout_publisher.cpp".
    std::string_view library_path{};
    std::size_t allocation_budget_bytes = kProductAllocationBudgetBytes;
  };

  SpoutFramePublisher();
  explicit SpoutFramePublisher(Options options);
  ~SpoutFramePublisher() override;

  SpoutFramePublisher(const SpoutFramePublisher&) = delete;
  auto operator=(const SpoutFramePublisher&) -> SpoutFramePublisher& = delete;
  SpoutFramePublisher(SpoutFramePublisher&&) = delete;
  auto operator=(SpoutFramePublisher&&) -> SpoutFramePublisher& = delete;

  // False whenever SpoutLibrary.dll is absent, its ABI probe fails, or the
  // constructing thread's OpenGL context (CreateOpenGL()) could not be
  // created. Absence is never an error -- callers simply do not offer this
  // provider.
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
