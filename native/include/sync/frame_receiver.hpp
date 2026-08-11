#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <sync/protocol.hpp>

namespace noisefactor::sync {

enum class PublishResult {
  Accepted,
  Backpressured,
  Failed,
};

class FramePublisher {
 public:
  virtual ~FramePublisher() = default;

  virtual auto open_sender(std::string_view sender_id,
                           std::string_view name) noexcept -> bool {
    (void)sender_id;
    (void)name;
    return true;
  }

  virtual void close_sender(std::string_view sender_id) noexcept {
    (void)sender_id;
  }

  // frame and frame.payload are borrowed views valid only for this call. A publisher that
  // retains frame data must synchronously copy it into owned, bounded storage before returning.
  virtual auto publish(std::string_view sender_id, const protocol::FrameView& frame) noexcept
      -> PublishResult = 0;

  virtual auto diagnostic_checksum(std::string_view sender_id) const noexcept
      -> std::uint64_t {
    (void)sender_id;
    return 0;
  }
};

enum class ReceiveStatus {
  Accepted,
  DroppedBackpressure,
  DroppedStale,
  RejectedMalformed,
  RejectedSender,
  PublishFailed,
};

struct ReceiveResult {
  ReceiveStatus status;
  protocol::DecodeError decode_error;
};

struct SenderStats {
  std::uint64_t accepted = 0;
  std::uint64_t dropped = 0;
  std::uint64_t rejected = 0;
  std::uint64_t failed = 0;
  bool has_last_sequence = false;
  std::uint64_t last_sequence = 0;
  std::uint64_t last_presentation_time_us = 0;
};

class FrameReceiver {
 public:
  explicit FrameReceiver(FramePublisher& publisher, protocol::Limits limits = {});

  [[nodiscard]] auto receive(std::string_view sender_id,
                             std::span<const std::byte> bytes) noexcept -> ReceiveResult;
  [[nodiscard]] auto stats(std::string_view sender_id) const noexcept -> const SenderStats*;
  [[nodiscard]] auto remove_sender(std::string_view sender_id) noexcept -> bool;

 private:
  static constexpr std::size_t kMaximumSenderEntries = 64;
  static constexpr std::size_t kMaximumSenderIdBytes = 128;

  struct SenderEntry {
    bool occupied = false;
    std::size_t sender_id_length = 0;
    std::array<char, kMaximumSenderIdBytes> sender_id{};
    SenderStats stats;
  };

  [[nodiscard]] auto find_sender(std::string_view sender_id) noexcept -> SenderEntry*;
  [[nodiscard]] auto find_sender(std::string_view sender_id) const noexcept
      -> const SenderEntry*;
  [[nodiscard]] auto find_or_create_sender(std::string_view sender_id) noexcept -> SenderEntry*;

  FramePublisher& publisher_;
  protocol::Limits limits_;
  std::array<SenderEntry, kMaximumSenderEntries> sender_entries_{};
};

}  // namespace noisefactor::sync
