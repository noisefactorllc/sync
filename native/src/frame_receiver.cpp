#include <sync/frame_receiver.hpp>

namespace noisefactor::sync {
namespace {

ReceiveResult result(ReceiveStatus status,
                     protocol::DecodeError decode_error = protocol::DecodeError::None) noexcept {
  return {.status = status, .decode_error = decode_error};
}

}  // namespace

FrameReceiver::FrameReceiver(FramePublisher& publisher, protocol::Limits limits)
    : publisher_(publisher), limits_(limits) {}

auto FrameReceiver::find_sender(std::string_view sender_id) noexcept -> SenderEntry* {
  for (SenderEntry& entry : sender_entries_) {
    if (entry.occupied && entry.sender_id_length == sender_id.size() &&
        std::string_view(entry.sender_id.data(), entry.sender_id_length) == sender_id) {
      return &entry;
    }
  }
  return nullptr;
}

auto FrameReceiver::find_sender(std::string_view sender_id) const noexcept -> const SenderEntry* {
  for (const SenderEntry& entry : sender_entries_) {
    if (entry.occupied && entry.sender_id_length == sender_id.size() &&
        std::string_view(entry.sender_id.data(), entry.sender_id_length) == sender_id) {
      return &entry;
    }
  }
  return nullptr;
}

auto FrameReceiver::find_or_create_sender(std::string_view sender_id) noexcept -> SenderEntry* {
  if (SenderEntry* existing = find_sender(sender_id)) {
    return existing;
  }
  for (SenderEntry& entry : sender_entries_) {
    if (!entry.occupied) {
      for (std::size_t index = 0; index < sender_id.size(); ++index) {
        entry.sender_id[index] = sender_id[index];
      }
      entry.sender_id_length = sender_id.size();
      entry.occupied = true;
      return &entry;
    }
  }
  return nullptr;
}

auto FrameReceiver::receive(std::string_view sender_id,
                            std::span<const std::byte> bytes) noexcept -> ReceiveResult {
  if (sender_id.empty() || sender_id.size() > kMaximumSenderIdBytes) {
    return result(ReceiveStatus::RejectedSender);
  }

  SenderEntry* entry = find_or_create_sender(sender_id);
  if (entry == nullptr) {
    return result(ReceiveStatus::RejectedSender);
  }

  const auto decoded = protocol::decode_frame(bytes, limits_);
  SenderStats& stats = entry->stats;
  if (!decoded.ok()) {
    ++stats.rejected;
    return result(ReceiveStatus::RejectedMalformed, decoded.error);
  }

  const protocol::FrameView& frame = *decoded.frame;
  if (stats.has_last_sequence && frame.sequence <= stats.last_sequence) {
    ++stats.dropped;
    return result(ReceiveStatus::DroppedStale);
  }

  stats.has_last_sequence = true;
  stats.last_sequence = frame.sequence;
  stats.last_presentation_time_us = frame.presentation_time_us;

  switch (publisher_.publish(sender_id, frame)) {
    case PublishResult::Accepted:
      ++stats.accepted;
      return result(ReceiveStatus::Accepted);
    case PublishResult::Backpressured:
      ++stats.dropped;
      return result(ReceiveStatus::DroppedBackpressure);
    case PublishResult::Failed:
      ++stats.failed;
      return result(ReceiveStatus::PublishFailed);
  }

  ++stats.failed;
  return result(ReceiveStatus::PublishFailed);
}

auto FrameReceiver::stats(std::string_view sender_id) const noexcept -> const SenderStats* {
  const SenderEntry* entry = find_sender(sender_id);
  return entry == nullptr ? nullptr : &entry->stats;
}

auto FrameReceiver::remove_sender(std::string_view sender_id) noexcept -> bool {
  if (sender_id.empty() || sender_id.size() > kMaximumSenderIdBytes) {
    return false;
  }
  SenderEntry* entry = find_sender(sender_id);
  if (entry == nullptr) {
    return false;
  }
  *entry = SenderEntry{};
  return true;
}

}  // namespace noisefactor::sync
