#pragma once

#if !defined(__APPLE__)
#error "metal_frame_publisher.hpp is available only on Apple platforms"
#endif

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

#include <sync/frame_receiver.hpp>

namespace noisefactor::sync {

class MetalFrameConsumer;

class MetalFramePublisher final : public FramePublisher {
 public:
  static constexpr std::size_t kSlotsPerSender = 3;
  static constexpr std::size_t kMaximumSenderEntries = 8;
  static constexpr std::size_t kMaximumConsumers = 4;
  static constexpr std::size_t kProductAllocationBudgetBytes = 512ULL * 1024ULL * 1024ULL;

  struct Options {
    std::size_t allocation_budget_bytes = kProductAllocationBudgetBytes;
  };

  explicit MetalFramePublisher(std::span<MetalFrameConsumer* const> consumers);
  MetalFramePublisher(std::span<MetalFrameConsumer* const> consumers, Options options);
  ~MetalFramePublisher() override;

  MetalFramePublisher(const MetalFramePublisher&) = delete;
  auto operator=(const MetalFramePublisher&) -> MetalFramePublisher& = delete;
  MetalFramePublisher(MetalFramePublisher&&) = delete;
  auto operator=(MetalFramePublisher&&) -> MetalFramePublisher& = delete;

  [[nodiscard]] auto available() const noexcept -> bool;
  auto open_sender(std::string_view sender_id, std::string_view name) noexcept -> bool override;
  void close_sender(std::string_view sender_id) noexcept override;
  auto publish(std::string_view sender_id, const protocol::FrameView& frame) noexcept
      -> PublishResult override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace noisefactor::sync
