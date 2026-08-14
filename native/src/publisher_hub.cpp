#include <sync/publisher_hub.hpp>

#include <algorithm>
#include <stdexcept>

namespace noisefactor::sync {

PublisherHub::PublisherHub(std::span<FramePublisher* const> providers) {
  for (FramePublisher* provider : providers) {
    if (provider == nullptr) continue;
    if (provider_count_ == providers_.size()) {
      throw std::invalid_argument("PublisherHub supports at most four providers");
    }
    providers_[provider_count_++] = provider;
  }
}

auto PublisherHub::find_sender(std::string_view sender_id) noexcept -> SenderEntry* {
  for (SenderEntry& sender : senders_) {
    if (sender.occupied && sender.sender_id_length == sender_id.size() &&
        std::string_view(sender.sender_id.data(), sender.sender_id_length) == sender_id) {
      return &sender;
    }
  }
  return nullptr;
}

auto PublisherHub::find_sender(std::string_view sender_id) const noexcept
    -> const SenderEntry* {
  for (const SenderEntry& sender : senders_) {
    if (sender.occupied && sender.sender_id_length == sender_id.size() &&
        std::string_view(sender.sender_id.data(), sender.sender_id_length) == sender_id) {
      return &sender;
    }
  }
  return nullptr;
}

auto PublisherHub::open_sender(std::string_view sender_id,
                               std::string_view name) noexcept -> bool {
  if (sender_id.empty() || sender_id.size() > kMaximumSenderIdBytes || name.empty() ||
      name.size() > kMaximumSenderNameBytes || find_sender(sender_id) != nullptr) {
    return false;
  }

  SenderEntry* target = nullptr;
  for (SenderEntry& sender : senders_) {
    if (!sender.occupied) {
      target = &sender;
      break;
    }
  }
  if (target == nullptr) return false;

  std::uint8_t opened_provider_mask = 0;
  for (std::size_t index = 0; index < provider_count_; ++index) {
    if (!providers_[index]->open_sender(sender_id, name)) {
      for (std::size_t rollback = index; rollback > 0; --rollback) {
        const std::size_t provider_index = rollback - 1;
        const auto provider_bit = static_cast<std::uint8_t>(1U << provider_index);
        if ((opened_provider_mask & provider_bit) != 0) {
          providers_[provider_index]->close_sender(sender_id);
        }
      }
      return false;
    }
    opened_provider_mask = static_cast<std::uint8_t>(opened_provider_mask | (1U << index));
  }

  std::copy(sender_id.begin(), sender_id.end(), target->sender_id.begin());
  std::copy(name.begin(), name.end(), target->name.begin());
  target->sender_id_length = sender_id.size();
  target->name_length = name.size();
  target->opened_provider_mask = opened_provider_mask;
  target->occupied = true;
  return true;
}

void PublisherHub::close_sender(std::string_view sender_id) noexcept {
  SenderEntry* sender = find_sender(sender_id);
  if (sender == nullptr) return;

  for (std::size_t reverse = provider_count_; reverse > 0; --reverse) {
    const std::size_t provider_index = reverse - 1;
    const auto provider_bit = static_cast<std::uint8_t>(1U << provider_index);
    if ((sender->opened_provider_mask & provider_bit) != 0) {
      providers_[provider_index]->close_sender(sender_id);
    }
  }
  *sender = SenderEntry{};
}

auto PublisherHub::publish(std::string_view sender_id,
                           const protocol::FrameView& frame) noexcept -> PublishResult {
  const SenderEntry* sender = find_sender(sender_id);
  if (sender == nullptr || sender->opened_provider_mask == 0) {
    return PublishResult::Failed;
  }

  bool accepted = false;
  bool backpressured = false;
  for (std::size_t index = 0; index < provider_count_; ++index) {
    const auto provider_bit = static_cast<std::uint8_t>(1U << index);
    if ((sender->opened_provider_mask & provider_bit) == 0) continue;
    switch (providers_[index]->publish(sender_id, frame)) {
      case PublishResult::Accepted:
        accepted = true;
        break;
      case PublishResult::Backpressured:
        backpressured = true;
        break;
      case PublishResult::Failed:
        break;
    }
  }
  if (accepted) return PublishResult::Accepted;
  if (backpressured) return PublishResult::Backpressured;
  return PublishResult::Failed;
}

auto PublisherHub::diagnostic_checksum(std::string_view sender_id) const noexcept
    -> std::uint64_t {
  const SenderEntry* sender = find_sender(sender_id);
  if (sender == nullptr) return 0;

  for (std::size_t index = 0; index < provider_count_; ++index) {
    const auto provider_bit = static_cast<std::uint8_t>(1U << index);
    if ((sender->opened_provider_mask & provider_bit) == 0) continue;
    const std::uint64_t checksum = providers_[index]->diagnostic_checksum(sender_id);
    if (checksum != 0) return checksum;
  }
  return 0;
}

auto PublisherHub::poll_failure(std::uint64_t now_ms) noexcept
    -> std::optional<ProviderFailure> {
  for (std::size_t index = 0; index < provider_count_; ++index) {
    if (auto failure = providers_[index]->poll_failure(now_ms);
        failure.has_value()) {
      return failure;
    }
  }
  return std::nullopt;
}

}  // namespace noisefactor::sync
