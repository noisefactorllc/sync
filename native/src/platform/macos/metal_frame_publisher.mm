#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <sync/platform/metal_frame_consumer.hpp>
#include <sync/platform/metal_frame_publisher.hpp>

#include "metal_completion_tracker.hpp"
#include "metal_device_selection.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace noisefactor::sync {
namespace {

constexpr std::size_t kMaximumSenderIdBytes = 128;
constexpr std::size_t kMaximumSenderNameBytes = 64;
constexpr std::uint32_t kMaximumDimension = 4096;
constexpr std::uint32_t kMaximumPayloadBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMetalRowAlignment = 256;
constexpr std::uint64_t kMetalWatchdogTimeoutMs = 1'000;

auto select_metal_device() noexcept -> id<MTLDevice> {
  @try {
    // Metal's system-default selector is unsupported in command-line and daemon processes.
    // Copy stable device attributes and select independently of enumeration order.
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    constexpr std::size_t kMaximumEnumeratedDevices = 64;
    if (devices.count == 0 || devices.count > kMaximumEnumeratedDevices) {
      return nil;
    }
    std::array<detail::MetalDeviceAttributes, kMaximumEnumeratedDevices> attributes{};
    for (NSUInteger index = 0; index < devices.count; ++index) {
      id<MTLDevice> device = devices[index];
      attributes[index] = {
          .registry_id = device.registryID,
          .removable = device.removable,
          .headless = device.headless,
      };
    }
    const auto selected = detail::select_metal_device_index(
        std::span(attributes).first(static_cast<std::size_t>(devices.count)));
    return selected.has_value() ? devices[*selected] : nil;
  } @catch (NSException*) {
    return nil;
  }
}

auto checked_add(std::size_t left, std::size_t right, std::size_t& result) noexcept -> bool {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    return false;
  }
  result = left + right;
  return true;
}

auto checked_multiply(std::size_t left,
                      std::size_t right,
                      std::size_t& result) noexcept -> bool {
  if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
    return false;
  }
  result = left * right;
  return true;
}

auto aligned_staging_stride(std::uint32_t width, std::size_t& stride) noexcept -> bool {
  std::size_t pixel_bytes = 0;
  if (!checked_multiply(static_cast<std::size_t>(width), 4U, pixel_bytes)) {
    return false;
  }
  std::size_t with_padding = 0;
  if (!checked_add(pixel_bytes, kMetalRowAlignment - 1U, with_padding)) {
    return false;
  }
  stride = with_padding & ~(kMetalRowAlignment - 1U);
  return true;
}

auto view_is_valid(const protocol::FrameView& frame) noexcept -> bool {
  if (frame.version != 1 || frame.header_bytes != 64 || frame.flags != 1 || !frame.top_down ||
      frame.pixel_format != 1 || (frame.color_space != 1 && frame.color_space != 2) ||
      (frame.alpha_mode != 1 && frame.alpha_mode != 2 && frame.alpha_mode != 3) ||
      frame.width == 0 || frame.height == 0 || frame.width > kMaximumDimension ||
      frame.height > kMaximumDimension) {
    return false;
  }

  std::size_t packed_row_bytes = 0;
  if (!checked_multiply(static_cast<std::size_t>(frame.width), 4U, packed_row_bytes) ||
      static_cast<std::size_t>(frame.row_stride) < packed_row_bytes) {
    return false;
  }
  std::size_t expected_payload_bytes = 0;
  if (!checked_multiply(static_cast<std::size_t>(frame.row_stride),
                        static_cast<std::size_t>(frame.height), expected_payload_bytes) ||
      expected_payload_bytes > kMaximumPayloadBytes ||
      expected_payload_bytes != static_cast<std::size_t>(frame.payload_bytes) ||
      expected_payload_bytes != frame.payload.size()) {
    return false;
  }
  return true;
}

}  // namespace

struct MetalFramePublisher::Impl {
  struct Slot {
    id<MTLBuffer> __strong staging_buffer = nil;
    id<MTLTexture> __strong texture = nil;
    std::shared_ptr<detail::MetalCompletionTracker> completion;
  };

  struct Ring {
    bool allocated = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t staging_stride = 0;
    std::size_t resource_bytes = 0;
    std::array<Slot, MetalFramePublisher::kSlotsPerSender> slots{};

    [[nodiscard]] auto idle() const noexcept -> bool {
      for (const Slot& slot : slots) {
        if (slot.completion != nullptr && !slot.completion->available()) {
          return false;
        }
      }
      return true;
    }
  };

  struct SenderEntry {
    bool occupied = false;
    bool open = false;
    std::size_t sender_id_length = 0;
    std::array<char, kMaximumSenderIdBytes> sender_id{};
    std::size_t name_length = 0;
    std::array<char, kMaximumSenderNameBytes> name{};
    std::uint8_t opened_consumer_mask = 0;
    Ring ring{};

    [[nodiscard]] auto id_view() const noexcept -> std::string_view {
      return {sender_id.data(), sender_id_length};
    }
  };

  id<MTLDevice> __strong device = nil;
  id<MTLCommandQueue> __strong command_queue = nil;
  std::array<MetalFrameConsumer*, MetalFramePublisher::kMaximumConsumers> consumers{};
  std::size_t consumer_count = 0;
  std::size_t allocation_budget_bytes = 0;
  std::size_t allocated_bytes = 0;
  bool configuration_valid = true;
  std::shared_ptr<detail::MetalFailureLatch> failure_latch =
      std::make_shared<detail::MetalFailureLatch>();
  std::uint64_t observed_now_ms = 0;
  std::array<SenderEntry, MetalFramePublisher::kMaximumSenderEntries> senders{};

  void reap_drained() noexcept {
    for (SenderEntry& entry : senders) {
      if (entry.occupied && !entry.open && entry.ring.idle()) {
        if (entry.ring.allocated) {
          allocated_bytes -= entry.ring.resource_bytes;
        }
        entry = SenderEntry{};
      }
    }
  }

  [[nodiscard]] auto find_sender(std::string_view sender_id) noexcept -> SenderEntry* {
    for (SenderEntry& entry : senders) {
      if (entry.occupied && entry.sender_id_length == sender_id.size() &&
          entry.id_view() == sender_id) {
        return &entry;
      }
    }
    return nullptr;
  }

  [[nodiscard]] auto allocate_ring(std::uint32_t width,
                                   std::uint32_t height,
                                   Ring& result) noexcept -> bool {
    std::size_t stride = 0;
    std::size_t staging_bytes = 0;
    std::size_t texture_pixels = 0;
    std::size_t texture_bytes = 0;
    std::size_t per_slot_bytes = 0;
    std::size_t ring_bytes = 0;
    if (!aligned_staging_stride(width, stride) ||
        !checked_multiply(stride, static_cast<std::size_t>(height), staging_bytes) ||
        !checked_multiply(static_cast<std::size_t>(width), static_cast<std::size_t>(height),
                          texture_pixels) ||
        !checked_multiply(texture_pixels, 4U, texture_bytes) ||
        !checked_add(staging_bytes, texture_bytes, per_slot_bytes) ||
        !checked_multiply(per_slot_bytes, MetalFramePublisher::kSlotsPerSender, ring_bytes)) {
      return false;
    }

    result.width = width;
    result.height = height;
    result.staging_stride = stride;
    result.resource_bytes = ring_bytes;

    try {
      for (Slot& slot : result.slots) {
        slot.completion =
            std::make_shared<detail::MetalCompletionTracker>(failure_latch);
      }
    } catch (const std::exception&) {
      return false;
    }

    @try {
      MTLTextureDescriptor* descriptor =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                             width:width
                                                            height:height
                                                         mipmapped:NO];
      descriptor.storageMode = MTLStorageModePrivate;
      descriptor.usage = MTLTextureUsageShaderRead;

      for (Slot& slot : result.slots) {
        slot.staging_buffer =
            [device newBufferWithLength:staging_bytes options:MTLResourceStorageModeShared];
        slot.texture = [device newTextureWithDescriptor:descriptor];
        if (slot.staging_buffer == nil || slot.texture == nil) {
          return false;
        }
      }
    } @catch (NSException*) {
      return false;
    }
    result.allocated = true;
    return true;
  }

  [[nodiscard]] auto ensure_ring(SenderEntry& entry,
                                 std::uint32_t width,
                                 std::uint32_t height) noexcept -> PublishResult {
    if (entry.ring.allocated && entry.ring.width == width && entry.ring.height == height) {
      return PublishResult::Accepted;
    }
    if (entry.ring.allocated && !entry.ring.idle()) {
      return PublishResult::Backpressured;
    }

    std::size_t stride = 0;
    std::size_t staging_bytes = 0;
    std::size_t texture_pixels = 0;
    std::size_t texture_bytes = 0;
    std::size_t per_slot_bytes = 0;
    std::size_t requested_bytes = 0;
    if (!aligned_staging_stride(width, stride) ||
        !checked_multiply(stride, static_cast<std::size_t>(height), staging_bytes) ||
        !checked_multiply(static_cast<std::size_t>(width), static_cast<std::size_t>(height),
                          texture_pixels) ||
        !checked_multiply(texture_pixels, 4U, texture_bytes) ||
        !checked_add(staging_bytes, texture_bytes, per_slot_bytes) ||
        !checked_multiply(per_slot_bytes, MetalFramePublisher::kSlotsPerSender, requested_bytes)) {
      return PublishResult::Failed;
    }
    const std::size_t old_bytes = entry.ring.allocated ? entry.ring.resource_bytes : 0;
    if (old_bytes > allocated_bytes) {
      return PublishResult::Failed;
    }
    std::size_t peak_bytes = 0;
    if (!checked_add(allocated_bytes, requested_bytes, peak_bytes) ||
        peak_bytes > allocation_budget_bytes) {
      return PublishResult::Failed;
    }
    const std::size_t retained_bytes = allocated_bytes - old_bytes;
    std::size_t replacement_total = 0;
    if (!checked_add(retained_bytes, requested_bytes, replacement_total) ||
        replacement_total > allocation_budget_bytes) {
      return PublishResult::Failed;
    }

    Ring candidate;
    if (!allocate_ring(width, height, candidate)) {
      return PublishResult::Failed;
    }
    entry.ring = std::move(candidate);
    allocated_bytes = replacement_total;
    return PublishResult::Accepted;
  }
};

MetalFramePublisher::MetalFramePublisher(std::span<MetalFrameConsumer* const> consumers)
    : MetalFramePublisher(consumers, Options{.allocation_budget_bytes =
                                                 kProductAllocationBudgetBytes}) {}

MetalFramePublisher::MetalFramePublisher(std::span<MetalFrameConsumer* const> consumers,
                                         Options options)
    : impl_(std::make_unique<Impl>()) {
  impl_->allocation_budget_bytes = options.allocation_budget_bytes;
  if (options.allocation_budget_bytes > kProductAllocationBudgetBytes) {
    impl_->configuration_valid = false;
    return;
  }

  std::size_t non_null_count = 0;
  for (MetalFrameConsumer* consumer : consumers) {
    if (consumer != nullptr) {
      ++non_null_count;
      if (non_null_count <= kMaximumConsumers) {
        impl_->consumers[non_null_count - 1U] = consumer;
      }
    }
  }
  if (non_null_count > kMaximumConsumers) {
    impl_->configuration_valid = false;
    impl_->consumers = {};
    return;
  }
  impl_->consumer_count = non_null_count;

  @autoreleasepool {
    @try {
      impl_->device = select_metal_device();
      if (impl_->device != nil) {
        impl_->command_queue = [impl_->device newCommandQueue];
      }
    } @catch (NSException*) {
      impl_->device = nil;
      impl_->command_queue = nil;
    }
  }
}

MetalFramePublisher::~MetalFramePublisher() {
  if (impl_ == nullptr) {
    return;
  }
  @autoreleasepool {
    for (Impl::SenderEntry& entry : impl_->senders) {
      if (!entry.occupied || !entry.open) {
        continue;
      }
      entry.open = false;
      for (std::size_t index = impl_->consumer_count; index > 0; --index) {
        const std::size_t consumer_index = index - 1U;
        if ((entry.opened_consumer_mask & (1U << consumer_index)) == 0) {
          continue;
        }
        @try {
          impl_->consumers[consumer_index]->close_sender(entry.id_view());
        } @catch (NSException*) {
        }
      }
    }
    impl_->senders = {};
    impl_->allocated_bytes = 0;
    impl_.reset();
  }
}

auto MetalFramePublisher::available() const noexcept -> bool {
  return impl_ != nullptr && impl_->configuration_valid && impl_->device != nil &&
         impl_->command_queue != nil && impl_->consumer_count != 0;
}

auto MetalFramePublisher::open_sender(std::string_view sender_id,
                                      std::string_view name) noexcept -> bool {
  @autoreleasepool {
    if (!available() || sender_id.empty() || sender_id.size() > kMaximumSenderIdBytes ||
        name.empty() || name.size() > kMaximumSenderNameBytes) {
      return false;
    }
    impl_->reap_drained();
    if (impl_->find_sender(sender_id) != nullptr) {
      return false;
    }

    Impl::SenderEntry* entry = nullptr;
    for (Impl::SenderEntry& candidate : impl_->senders) {
      if (!candidate.occupied) {
        entry = &candidate;
        break;
      }
    }
    if (entry == nullptr) {
      return false;
    }

    std::uint8_t opened_mask = 0;
    bool accepted = true;
    for (std::size_t index = 0; index < impl_->consumer_count; ++index) {
      bool consumer_accepted = false;
      @try {
        consumer_accepted = impl_->consumers[index]->open_sender(sender_id, name, impl_->device);
      } @catch (NSException*) {
        consumer_accepted = false;
      }
      if (!consumer_accepted) {
        accepted = false;
        break;
      }
      opened_mask |= static_cast<std::uint8_t>(1U << index);
    }

    if (!accepted) {
      for (std::size_t index = impl_->consumer_count; index > 0; --index) {
        const std::size_t consumer_index = index - 1U;
        if ((opened_mask & (1U << consumer_index)) == 0) {
          continue;
        }
        @try {
          impl_->consumers[consumer_index]->close_sender(sender_id);
        } @catch (NSException*) {
        }
      }
      return false;
    }

    entry->occupied = true;
    entry->open = true;
    entry->sender_id_length = sender_id.size();
    std::memcpy(entry->sender_id.data(), sender_id.data(), sender_id.size());
    entry->name_length = name.size();
    std::memcpy(entry->name.data(), name.data(), name.size());
    entry->opened_consumer_mask = opened_mask;
    return true;
  }
}

void MetalFramePublisher::close_sender(std::string_view sender_id) noexcept {
  @autoreleasepool {
    if (impl_ == nullptr) {
      return;
    }
    impl_->reap_drained();
    Impl::SenderEntry* entry = impl_->find_sender(sender_id);
    if (entry == nullptr || !entry->open) {
      return;
    }

    entry->open = false;
    for (std::size_t index = impl_->consumer_count; index > 0; --index) {
      const std::size_t consumer_index = index - 1U;
      if ((entry->opened_consumer_mask & (1U << consumer_index)) == 0) {
        continue;
      }
      @try {
        impl_->consumers[consumer_index]->close_sender(entry->id_view());
      } @catch (NSException*) {
      }
    }
    entry->opened_consumer_mask = 0;
    impl_->reap_drained();
  }
}

auto MetalFramePublisher::publish(std::string_view sender_id,
                                  const protocol::FrameView& frame) noexcept -> PublishResult {
  @autoreleasepool {
    if (!available() || impl_->failure_latch->failed() ||
        !view_is_valid(frame)) {
      return PublishResult::Failed;
    }
    impl_->reap_drained();
    Impl::SenderEntry* entry = impl_->find_sender(sender_id);
    if (entry == nullptr || !entry->open) {
      return PublishResult::Failed;
    }

    const PublishResult ring_result = impl_->ensure_ring(*entry, frame.width, frame.height);
    if (ring_result != PublishResult::Accepted) {
      return ring_result;
    }

    Impl::Slot* claimed_slot = nullptr;
    for (Impl::Slot& slot : entry->ring.slots) {
      if (slot.completion->try_begin(impl_->observed_now_ms)) {
        claimed_slot = &slot;
        break;
      }
    }
    if (claimed_slot == nullptr) {
      return PublishResult::Backpressured;
    }

    const std::size_t packed_row_bytes = static_cast<std::size_t>(frame.width) * 4U;
    auto* destination = static_cast<std::byte*>(claimed_slot->staging_buffer.contents);
    if (destination == nullptr) {
      claimed_slot->completion->cancel_before_commit();
      return PublishResult::Failed;
    }
    if (static_cast<std::size_t>(frame.row_stride) == entry->ring.staging_stride) {
      std::memcpy(destination, frame.payload.data(), frame.payload.size());
    } else {
      for (std::uint32_t row = 0; row < frame.height; ++row) {
        std::memcpy(destination + static_cast<std::size_t>(row) * entry->ring.staging_stride,
                    frame.payload.data() + static_cast<std::size_t>(row) * frame.row_stride,
                    packed_row_bytes);
      }
    }

    id<MTLCommandBuffer> command_buffer = nil;
    bool encoded = false;
    bool completion_installed = false;
    @try {
      command_buffer = [impl_->command_queue commandBuffer];
      if (command_buffer == nil) {
        claimed_slot->completion->cancel_before_commit();
        return PublishResult::Failed;
      }
      id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
      if (blit == nil) {
        claimed_slot->completion->cancel_before_commit();
        return PublishResult::Failed;
      }
      [blit copyFromBuffer:claimed_slot->staging_buffer
              sourceOffset:0
         sourceBytesPerRow:entry->ring.staging_stride
       sourceBytesPerImage:entry->ring.staging_stride * frame.height
                sourceSize:MTLSizeMake(frame.width, frame.height, 1)
                 toTexture:claimed_slot->texture
          destinationSlice:0
          destinationLevel:0
         destinationOrigin:MTLOriginMake(0, 0, 0)];
      [blit endEncoding];

      const MetalFrameMetadata metadata{
          .width = frame.width,
          .height = frame.height,
          .color_space = static_cast<MetalColorSpace>(frame.color_space),
          .alpha_mode = static_cast<MetalAlphaMode>(frame.alpha_mode),
          .sequence = frame.sequence,
          .presentation_time_us = frame.presentation_time_us,
          .top_down = true,
      };
      encoded = true;
      for (std::size_t index = 0; index < impl_->consumer_count; ++index) {
        if ((entry->opened_consumer_mask & (1U << index)) != 0 &&
            !impl_->consumers[index]->encode_frame(entry->id_view(), claimed_slot->texture,
                                                   command_buffer, metadata)) {
          encoded = false;
          break;
        }
      }
      if (!encoded) {
        claimed_slot->completion->cancel_before_commit();
        return PublishResult::Failed;
      }

      auto completion = claimed_slot->completion;
      [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        @autoreleasepool {
          if (completed.status == MTLCommandBufferStatusCompleted) {
            completion->complete_success();
          } else {
            const std::int64_t error_code =
                completed.error == nil
                    ? 0
                    : static_cast<std::int64_t>(completed.error.code);
            completion->complete_failure(
                static_cast<std::uint32_t>(completed.status), error_code);
          }
        }
      }];
      completion_installed = true;
      [command_buffer commit];
    } @catch (NSException*) {
      if (completion_installed) {
        claimed_slot->completion->complete_failure(0, 0);
      } else {
        claimed_slot->completion->cancel_before_commit();
      }
      return PublishResult::Failed;
    }
    return PublishResult::Accepted;
  }
}

auto MetalFramePublisher::poll_failure(std::uint64_t now_ms) noexcept
    -> std::optional<ProviderFailure> {
  if (impl_ == nullptr) return std::nullopt;
  impl_->observed_now_ms = now_ms;
  for (Impl::SenderEntry& entry : impl_->senders) {
    if (!entry.ring.allocated) continue;
    for (Impl::Slot& slot : entry.ring.slots) {
      if (slot.completion != nullptr) {
        (void)slot.completion->poll_watchdog(now_ms,
                                             kMetalWatchdogTimeoutMs);
      }
    }
  }
  return impl_->failure_latch->failure();
}

}  // namespace noisefactor::sync
