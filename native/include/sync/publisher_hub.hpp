#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <sync/frame_receiver.hpp>

namespace noisefactor::sync {

class PublisherHub final : public FramePublisher {
 public:
  static constexpr std::size_t kMaximumProviders = 4;
  static constexpr std::size_t kMaximumSenders = 64;
  static constexpr std::size_t kMaximumSenderIdBytes = 128;
  static constexpr std::size_t kMaximumSenderNameBytes = 64;

  explicit PublisherHub(std::span<FramePublisher* const> providers);

  auto open_sender(std::string_view sender_id,
                   std::string_view name) noexcept -> bool override;
  void close_sender(std::string_view sender_id) noexcept override;
  auto publish(std::string_view sender_id,
               const protocol::FrameView& frame) noexcept -> PublishResult override;
  auto diagnostic_checksum(std::string_view sender_id) const noexcept
      -> std::uint64_t override;
  auto poll_failure(std::uint64_t now_ms) noexcept
      -> std::optional<ProviderFailure> override;

 private:
  struct SenderEntry {
    bool occupied = false;
    std::uint8_t opened_provider_mask = 0;
    std::size_t sender_id_length = 0;
    std::size_t name_length = 0;
    std::array<char, kMaximumSenderIdBytes> sender_id{};
    std::array<char, kMaximumSenderNameBytes> name{};
  };

  auto find_sender(std::string_view sender_id) noexcept -> SenderEntry*;
  auto find_sender(std::string_view sender_id) const noexcept -> const SenderEntry*;

  std::array<FramePublisher*, kMaximumProviders> providers_{};
  std::size_t provider_count_ = 0;
  std::array<SenderEntry, kMaximumSenders> senders_{};
};

}  // namespace noisefactor::sync
