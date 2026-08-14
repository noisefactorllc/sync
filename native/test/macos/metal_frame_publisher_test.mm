#import <Metal/Metal.h>

#include "../test_harness.hpp"

#include <sync/platform/metal_frame_consumer.hpp>
#include <sync/platform/metal_frame_publisher.hpp>
#include <sync/protocol.hpp>

#include "../../src/platform/macos/metal_device_selection.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace noisefactor::sync {
namespace {

using namespace std::chrono_literals;

auto aligned_stride(std::uint32_t width) -> std::size_t {
  return (static_cast<std::size_t>(width) * 4U + 255U) & ~std::size_t{255U};
}

auto wait_until(const auto& predicate, std::chrono::milliseconds timeout = 3000ms) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

struct OwnedFrame {
  std::vector<std::byte> payload;
  protocol::FrameView view;
};

auto make_frame(std::uint32_t width,
                std::uint32_t height,
                std::uint32_t row_stride,
                std::uint64_t sequence = 1) -> OwnedFrame {
  OwnedFrame frame;
  frame.payload.resize(static_cast<std::size_t>(row_stride) * height, std::byte{0xee});
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::size_t offset = static_cast<std::size_t>(y) * row_stride + x * 4U;
      frame.payload[offset + 0] = std::byte{static_cast<unsigned char>(10U + y * 20U + x)};
      frame.payload[offset + 1] = std::byte{static_cast<unsigned char>(40U + y * 20U + x)};
      frame.payload[offset + 2] = std::byte{static_cast<unsigned char>(70U + y * 20U + x)};
      frame.payload[offset + 3] = std::byte{static_cast<unsigned char>(100U + y * 20U + x)};
    }
  }
  frame.view = {
      .version = 1,
      .header_bytes = 64,
      .flags = 1,
      .pixel_format = 1,
      .color_space = 2,
      .alpha_mode = 3,
      .width = width,
      .height = height,
      .row_stride = row_stride,
      .payload_bytes = static_cast<std::uint32_t>(frame.payload.size()),
      .sequence = sequence,
      .presentation_time_us = sequence * 1000U,
      .top_down = true,
      .payload = frame.payload,
  };
  return frame;
}

class RecordingConsumer final : public MetalFrameConsumer {
 public:
  explicit RecordingConsumer(bool accept_open = true) : accept_open_(accept_open) {}

  auto open_sender(std::string_view sender_id,
                   std::string_view name,
                   id<MTLDevice> device) noexcept -> bool override {
    open_ids.emplace_back(sender_id);
    open_names.emplace_back(name);
    devices.push_back(device);
    return accept_open_;
  }

  void close_sender(std::string_view sender_id) noexcept override {
    close_ids.emplace_back(sender_id);
  }

  auto encode_frame(std::string_view sender_id,
                    id<MTLTexture> texture,
                    id<MTLCommandBuffer> command_buffer,
                    const MetalFrameMetadata& metadata) noexcept -> bool override {
    ++encode_calls;
    last_sender_id = sender_id;
    last_texture = texture;
    last_command_buffer = command_buffer;
    last_metadata = metadata;
    texture_was_private = texture.storageMode == MTLStorageModePrivate;
    texture_was_rgba8 = texture.pixelFormat == MTLPixelFormatRGBA8Unorm;
    command_buffer_was_uncommitted = command_buffer.status == MTLCommandBufferStatusNotEnqueued;
    if (!accept_encode) {
      return false;
    }

    const std::size_t stride = aligned_stride(metadata.width);
    id<MTLBuffer> output = [devices.front() newBufferWithLength:stride * metadata.height
                                                  options:MTLResourceStorageModeShared];
    if (output == nil) {
      return false;
    }
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    if (blit == nil) {
      return false;
    }
    [blit copyFromTexture:texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(metadata.width, metadata.height, 1)
                 toBuffer:output
        destinationOffset:0
   destinationBytesPerRow:stride
 destinationBytesPerImage:stride * metadata.height];
    [blit endEncoding];
    pending_outputs.push_back(output);
    const std::uint32_t width = metadata.width;
    const std::uint32_t height = metadata.height;
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
      std::vector<std::byte> packed(static_cast<std::size_t>(width) * height * 4U);
      const auto* source = static_cast<const std::byte*>(output.contents);
      for (std::uint32_t y = 0; y < height; ++y) {
        std::copy_n(source + static_cast<std::size_t>(y) * stride,
                    static_cast<std::size_t>(width) * 4U,
                    packed.data() + static_cast<std::size_t>(y) * width * 4U);
      }
      {
        std::lock_guard lock(outputs_mutex);
        completed_outputs.push_back(std::move(packed));
      }
      completion_count.fetch_add(1, std::memory_order_release);
    }];
    return true;
  }

  bool accept_encode = true;
  std::vector<std::string> open_ids;
  std::vector<std::string> open_names;
  std::vector<id<MTLDevice>> devices;
  std::vector<std::string> close_ids;
  std::size_t encode_calls = 0;
  std::string last_sender_id;
  id<MTLTexture> last_texture = nil;
  id<MTLCommandBuffer> last_command_buffer = nil;
  MetalFrameMetadata last_metadata{};
  bool texture_was_private = false;
  bool texture_was_rgba8 = false;
  bool command_buffer_was_uncommitted = false;
  std::vector<id<MTLBuffer>> pending_outputs;
  std::mutex outputs_mutex;
  std::vector<std::vector<std::byte>> completed_outputs;
  std::atomic<std::size_t> completion_count{0};

  auto output_copy(std::size_t index) -> std::vector<std::byte> {
    std::lock_guard lock(outputs_mutex);
    return completed_outputs.at(index);
  }

 private:
  bool accept_open_;
};

class OrderedConsumer final : public MetalFrameConsumer {
 public:
  OrderedConsumer(int number, std::vector<int>& events, bool accepts)
      : number_(number), events_(events), accepts_(accepts) {}
  auto open_sender(std::string_view, std::string_view, id<MTLDevice>) noexcept -> bool override {
    events_.push_back(number_);
    return accepts_;
  }
  void close_sender(std::string_view) noexcept override { events_.push_back(-number_); }
  auto encode_frame(std::string_view,
                    id<MTLTexture>,
                    id<MTLCommandBuffer>,
                    const MetalFrameMetadata&) noexcept -> bool override {
    return true;
  }

 private:
  int number_;
  std::vector<int>& events_;
  bool accepts_;
};

class GateConsumer final : public MetalFrameConsumer {
 public:
  ~GateConsumer() override { release(); }

  auto open_sender(std::string_view, std::string_view, id<MTLDevice> device) noexcept
      -> bool override {
    if (event == nil) {
      event = [device newSharedEvent];
    }
    return event != nil;
  }
  void close_sender(std::string_view) noexcept override { ++close_calls; }
  auto encode_frame(std::string_view,
                    id<MTLTexture>,
                    id<MTLCommandBuffer> command_buffer,
                    const MetalFrameMetadata&) noexcept -> bool override {
    [command_buffer encodeWaitForEvent:event value:gate_value];
    const auto proof = completion_proof;
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
      proof->fetch_add(1, std::memory_order_release);
    }];
    ++encode_calls;
    return true;
  }
  void release() { event.signaledValue = gate_value; }
  [[nodiscard]] auto completed() const noexcept -> std::size_t {
    return completion_proof->load(std::memory_order_acquire);
  }

  id<MTLSharedEvent> event = nil;
  std::uint64_t gate_value = 1;
  std::size_t encode_calls = 0;
  std::size_t close_calls = 0;
  std::shared_ptr<std::atomic<std::size_t>> completion_proof =
      std::make_shared<std::atomic<std::size_t>>(0);
};

class RejectingSignalConsumer final : public MetalFrameConsumer {
 public:
  auto open_sender(std::string_view, std::string_view, id<MTLDevice> device) noexcept
      -> bool override {
    event = [device newSharedEvent];
    return event != nil;
  }
  void close_sender(std::string_view) noexcept override {}
  auto encode_frame(std::string_view,
                    id<MTLTexture>,
                    id<MTLCommandBuffer> command_buffer,
                    const MetalFrameMetadata&) noexcept -> bool override {
    ++encode_calls;
    [command_buffer encodeSignalEvent:event value:encode_calls];
    if (reject_next) {
      rejected_command_buffer = command_buffer;
      reject_next = false;
      return false;
    }
    accepted_command_buffer = command_buffer;
    return true;
  }

  id<MTLSharedEvent> event = nil;
  id<MTLCommandBuffer> rejected_command_buffer = nil;
  id<MTLCommandBuffer> accepted_command_buffer = nil;
  std::uint64_t encode_calls = 0;
  bool reject_next = true;
};

SYNC_TEST(metal_publisher_availability_lifecycle_and_fixed_bounds) {
  MetalFramePublisher none({});
  SYNC_REQUIRE(!none.available());

  RecordingConsumer first;
  RecordingConsumer second;
  std::array<MetalFrameConsumer*, 2> consumers{&first, &second};
  MetalFramePublisher publisher(consumers);
  SYNC_REQUIRE(publisher.available());
  SYNC_REQUIRE(publisher.open_sender("sender", "Human Name"));
  SYNC_REQUIRE(first.devices.size() == 1);
  SYNC_REQUIRE(first.devices[0] == second.devices[0]);
  NSArray<id<MTLDevice>>* enumerated_devices = MTLCopyAllDevices();
  SYNC_REQUIRE(enumerated_devices.count > 0);
  std::array<detail::MetalDeviceAttributes, 64> real_attributes{};
  SYNC_REQUIRE(enumerated_devices.count <= real_attributes.size());
  for (NSUInteger index = 0; index < enumerated_devices.count; ++index) {
    id<MTLDevice> device = enumerated_devices[index];
    real_attributes[index] = {
        .registry_id = device.registryID,
        .removable = device.removable,
        .headless = device.headless,
    };
  }
  const auto expected_device_index = detail::select_metal_device_index(
      std::span(real_attributes).first(enumerated_devices.count));
  SYNC_REQUIRE(expected_device_index.has_value());
  SYNC_REQUIRE(first.devices[0] == enumerated_devices[*expected_device_index]);
  SYNC_REQUIRE(first.open_ids[0] == "sender");
  SYNC_REQUIRE(first.open_names[0] == "Human Name");
  SYNC_REQUIRE(!publisher.open_sender("sender", "Duplicate"));
  SYNC_REQUIRE(!publisher.open_sender("", "Name"));
  SYNC_REQUIRE(!publisher.open_sender(std::string(129, 'i'), "Name"));
  SYNC_REQUIRE(!publisher.open_sender("id", ""));
  SYNC_REQUIRE(!publisher.open_sender("id", std::string(65, 'n')));
  publisher.close_sender("sender");
  publisher.close_sender("sender");
  SYNC_REQUIRE(first.close_ids.size() == 1);
  SYNC_REQUIRE(second.close_ids.size() == 1);

  std::array<MetalFrameConsumer*, 5> too_many{&first, &second, &first, &second, &first};
  MetalFramePublisher rejected(too_many);
  SYNC_REQUIRE(!rejected.available());

  RecordingConsumer sole;
  std::array<MetalFrameConsumer*, 1> one{&sole};
  MetalFramePublisher capped(one);
  for (int index = 0; index < 8; ++index) {
    SYNC_REQUIRE(capped.open_sender("s" + std::to_string(index), "name"));
  }
  SYNC_REQUIRE(!capped.open_sender("overflow", "name"));
  capped.close_sender("s3");
  SYNC_REQUIRE(capped.open_sender("replacement", "name"));
}

SYNC_TEST(metal_device_selector_is_stable_under_reordered_input) {
  using detail::MetalDeviceAttributes;
  const std::array first_order{
      MetalDeviceAttributes{.registry_id = 50},
      MetalDeviceAttributes{.registry_id = 1, .removable = true},
      MetalDeviceAttributes{.registry_id = 20},
      MetalDeviceAttributes{.registry_id = 2, .headless = true},
  };
  const std::array second_order{first_order[3], first_order[2], first_order[1], first_order[0]};
  const auto first_index = detail::select_metal_device_index(first_order);
  const auto second_index = detail::select_metal_device_index(second_order);
  SYNC_REQUIRE(first_index.has_value());
  SYNC_REQUIRE(second_index.has_value());
  SYNC_REQUIRE(first_order[*first_index].registry_id == 20);
  SYNC_REQUIRE(second_order[*second_index].registry_id == 20);
}

SYNC_TEST(metal_device_selector_prefers_display_attached_and_falls_back_deterministically) {
  using detail::MetalDeviceAttributes;
  const std::array with_preferred{
      MetalDeviceAttributes{.registry_id = 1, .removable = true},
      MetalDeviceAttributes{.registry_id = 2, .headless = true},
      MetalDeviceAttributes{.registry_id = 99},
  };
  const auto preferred_index = detail::select_metal_device_index(with_preferred);
  SYNC_REQUIRE(preferred_index.has_value());
  SYNC_REQUIRE(with_preferred[*preferred_index].registry_id == 99);

  const std::array fallback_only{
      MetalDeviceAttributes{.registry_id = 9, .removable = true},
      MetalDeviceAttributes{.registry_id = 4, .headless = true},
  };
  const auto fallback_index = detail::select_metal_device_index(fallback_only);
  SYNC_REQUIRE(fallback_index.has_value());
  SYNC_REQUIRE(fallback_only[*fallback_index].registry_id == 4);
}

SYNC_TEST(metal_device_selector_handles_empty_input_and_stable_ties) {
  using detail::MetalDeviceAttributes;
  SYNC_REQUIRE(!detail::select_metal_device_index(
                    std::span<const MetalDeviceAttributes>{})
                    .has_value());

  const std::array tied{
      MetalDeviceAttributes{.registry_id = 7},
      MetalDeviceAttributes{.registry_id = 7},
      MetalDeviceAttributes{.registry_id = 7},
  };
  const std::array reordered{tied[2], tied[0], tied[1]};
  const auto tied_index = detail::select_metal_device_index(tied);
  const auto reordered_index = detail::select_metal_device_index(reordered);
  SYNC_REQUIRE(tied_index.has_value());
  SYNC_REQUIRE(reordered_index.has_value());
  SYNC_REQUIRE(tied[*tied_index].registry_id == 7);
  SYNC_REQUIRE(reordered[*reordered_index].registry_id == 7);
}

SYNC_TEST(metal_publisher_rolls_back_open_and_closes_in_reverse) {
  std::vector<int> events;
  OrderedConsumer first(1, events, true);
  OrderedConsumer second(2, events, true);
  OrderedConsumer third(3, events, false);
  std::array<MetalFrameConsumer*, 3> consumers{&first, &second, &third};
  MetalFramePublisher publisher(consumers);
  SYNC_REQUIRE(!publisher.open_sender("id", "name"));
  SYNC_REQUIRE((events == std::vector<int>{1, 2, 3, -2, -1}));

  events.clear();
  std::array<MetalFrameConsumer*, 2> accepted{&first, &second};
  MetalFramePublisher accepted_publisher(accepted);
  SYNC_REQUIRE(accepted_publisher.open_sender("id", "name"));
  accepted_publisher.close_sender("id");
  SYNC_REQUIRE((events == std::vector<int>{1, 2, -2, -1}));
}

SYNC_TEST(metal_publisher_uploads_golden_frame_top_down_on_real_gpu) {
  std::ifstream file(std::string(SYNC_SOURCE_DIR) + "/test/fixtures/frame-v1.bin",
                     std::ios::binary);
  SYNC_REQUIRE(file.good());
  std::vector<char> chars((std::istreambuf_iterator<char>(file)), {});
  std::vector<std::byte> bytes(chars.size());
  std::transform(chars.begin(), chars.end(), bytes.begin(), [](char value) {
    return std::byte{static_cast<unsigned char>(value)};
  });
  const auto decoded = protocol::decode_frame(bytes);
  SYNC_REQUIRE(decoded.ok());

  RecordingConsumer consumer;
  std::array<MetalFrameConsumer*, 1> consumers{&consumer};
  MetalFramePublisher publisher(consumers);
  SYNC_REQUIRE(publisher.open_sender("golden", "Golden"));
  SYNC_REQUIRE(publisher.publish("golden", *decoded.frame) == PublishResult::Accepted);
  SYNC_REQUIRE(wait_until([&] { return consumer.completion_count.load() == 1; }));
  SYNC_REQUIRE(consumer.texture_was_private);
  SYNC_REQUIRE(consumer.texture_was_rgba8);
  SYNC_REQUIRE(consumer.command_buffer_was_uncommitted);
  SYNC_REQUIRE(consumer.output_copy(0) ==
               std::vector<std::byte>(decoded.frame->payload.begin(), decoded.frame->payload.end()));
}

SYNC_TEST(metal_publisher_repacks_padded_rows_and_preserves_metadata) {
  RecordingConsumer consumer;
  std::array<MetalFrameConsumer*, 1> consumers{&consumer};
  MetalFramePublisher publisher(consumers);
  SYNC_REQUIRE(publisher.open_sender("rows", "Rows"));

  auto padded = make_frame(2, 2, 12, 42);
  SYNC_REQUIRE(publisher.publish("rows", padded.view) == PublishResult::Accepted);
  SYNC_REQUIRE(wait_until([&] { return consumer.completion_count.load() == 1; }));
  std::vector<std::byte> expected;
  expected.insert(expected.end(), padded.payload.begin(), padded.payload.begin() + 8);
  expected.insert(expected.end(), padded.payload.begin() + 12, padded.payload.begin() + 20);
  SYNC_REQUIRE(consumer.output_copy(0) == expected);
  SYNC_REQUIRE(consumer.last_metadata.width == 2);
  SYNC_REQUIRE(consumer.last_metadata.height == 2);
  SYNC_REQUIRE(consumer.last_metadata.color_space == MetalColorSpace::DisplayP3);
  SYNC_REQUIRE(consumer.last_metadata.alpha_mode == MetalAlphaMode::Premultiplied);
  SYNC_REQUIRE(consumer.last_metadata.sequence == 42);
  SYNC_REQUIRE(consumer.last_metadata.presentation_time_us == 42000);
  SYNC_REQUIRE(consumer.last_metadata.top_down);

  auto aligned = make_frame(64, 1, 256, 43);
  SYNC_REQUIRE(publisher.publish("rows", aligned.view) == PublishResult::Accepted);
  SYNC_REQUIRE(wait_until([&] { return consumer.completion_count.load() == 2; }));
  SYNC_REQUIRE(consumer.output_copy(1) == aligned.payload);
}

SYNC_TEST(metal_publisher_saturates_three_slots_and_reuses_after_gpu_completion) {
  GateConsumer gate;
  std::array<MetalFrameConsumer*, 1> consumers{&gate};
  MetalFramePublisher publisher(consumers);
  SYNC_REQUIRE(publisher.open_sender("gate", "Gate"));
  auto frame = make_frame(2, 2, 8);
  SYNC_REQUIRE(publisher.publish("gate", frame.view) == PublishResult::Accepted);
  frame.view.sequence++;
  SYNC_REQUIRE(publisher.publish("gate", frame.view) == PublishResult::Accepted);
  frame.view.sequence++;
  SYNC_REQUIRE(publisher.publish("gate", frame.view) == PublishResult::Accepted);
  frame.view.sequence++;
  SYNC_REQUIRE(publisher.publish("gate", frame.view) == PublishResult::Backpressured);
  gate.release();
  SYNC_REQUIRE(wait_until([&] {
    return publisher.publish("gate", frame.view) == PublishResult::Accepted;
  }));
}

SYNC_TEST(metal_publisher_reports_a_gated_command_at_the_watchdog_boundary) {
  GateConsumer gate;
  std::array<MetalFrameConsumer*, 1> consumers{&gate};
  MetalFramePublisher publisher(consumers);
  SYNC_REQUIRE(publisher.open_sender("watchdog", "Watchdog"));
  auto frame = make_frame(2, 2, 8);

  SYNC_REQUIRE(!publisher.poll_failure(10'000).has_value());
  SYNC_REQUIRE(publisher.publish("watchdog", frame.view) ==
               PublishResult::Accepted);
  SYNC_REQUIRE(!publisher.poll_failure(10'999).has_value());

  const auto failure = publisher.poll_failure(11'000);
  SYNC_REQUIRE(failure.has_value());
  SYNC_REQUIRE(failure->kind ==
               ProviderFailureKind::MetalWatchdogTimeout);
  SYNC_REQUIRE(publisher.publish("watchdog", frame.view) ==
               PublishResult::Failed);

  gate.release();
  SYNC_REQUIRE(wait_until([&] { return gate.completed() == 1; }));
  const auto after_completion = publisher.poll_failure(12'000);
  SYNC_REQUIRE(after_completion.has_value());
  SYNC_REQUIRE(after_completion->kind ==
               ProviderFailureKind::MetalWatchdogTimeout);
}

SYNC_TEST(metal_publisher_counts_a_closed_inflight_sender_until_it_drains) {
  GateConsumer gate;
  std::array<MetalFrameConsumer*, 1> consumers{&gate};
  MetalFramePublisher publisher(consumers);
  for (int index = 0; index < 8; ++index) {
    SYNC_REQUIRE(publisher.open_sender("drain" + std::to_string(index), "name"));
  }
  auto frame = make_frame(2, 2, 8);
  SYNC_REQUIRE(publisher.publish("drain3", frame.view) == PublishResult::Accepted);
  publisher.close_sender("drain3");
  SYNC_REQUIRE(!publisher.open_sender("ninth", "name"));
  gate.release();
  SYNC_REQUIRE(wait_until([&] { return gate.completed() == 1; }));
  SYNC_REQUIRE(wait_until([&] { return publisher.open_sender("drain3", "reused"); }));
}

SYNC_TEST(metal_publisher_close_and_destroy_are_safe_with_gated_frames) {
  GateConsumer gate;
  std::array<MetalFrameConsumer*, 1> consumers{&gate};
  auto frame = make_frame(2, 2, 8);
  {
    MetalFramePublisher publisher(consumers);
    SYNC_REQUIRE(publisher.open_sender("reuse", "First"));
    for (int index = 0; index < 3; ++index) {
      frame.view.sequence++;
      SYNC_REQUIRE(publisher.publish("reuse", frame.view) == PublishResult::Accepted);
    }
    publisher.close_sender("reuse");
    SYNC_REQUIRE(publisher.publish("reuse", frame.view) == PublishResult::Failed);
    SYNC_REQUIRE(!publisher.open_sender("reuse", "Too Soon"));
    gate.release();
    SYNC_REQUIRE(wait_until([&] { return publisher.open_sender("reuse", "Again"); }));
    publisher.close_sender("reuse");
  }

  GateConsumer destruction_gate;
  std::array<MetalFrameConsumer*, 1> destruction_consumers{&destruction_gate};
  const auto started = std::chrono::steady_clock::now();
  {
    MetalFramePublisher publisher(destruction_consumers);
    SYNC_REQUIRE(publisher.open_sender("destroy", "Destroy"));
    SYNC_REQUIRE(publisher.publish("destroy", frame.view) == PublishResult::Accepted);
  }
  SYNC_REQUIRE(std::chrono::steady_clock::now() - started < 1000ms);
  destruction_gate.release();
  SYNC_REQUIRE(wait_until([&] { return destruction_gate.completed() == 1; }));
}

SYNC_TEST(metal_publisher_reconfiguration_and_budget_are_transactional) {
  GateConsumer gate;
  std::array<MetalFrameConsumer*, 1> consumers{&gate};
  MetalFramePublisher publisher(consumers);
  SYNC_REQUIRE(publisher.open_sender("resize", "Resize"));
  auto small = make_frame(2, 2, 8);
  auto large = make_frame(3, 2, 12);
  SYNC_REQUIRE(publisher.publish("resize", small.view) == PublishResult::Accepted);
  SYNC_REQUIRE(publisher.publish("resize", large.view) == PublishResult::Backpressured);
  gate.release();
  SYNC_REQUIRE(wait_until([&] {
    return publisher.publish("resize", large.view) == PublishResult::Accepted;
  }));

  RecordingConsumer budget_recorder;
  std::array<MetalFrameConsumer*, 1> budget_consumers{&budget_recorder};
  const std::size_t exact_two_by_two = (256U * 2U + 2U * 2U * 4U) * 3U;
  MetalFramePublisher too_small(budget_consumers,
                                {.allocation_budget_bytes = exact_two_by_two - 1U});
  SYNC_REQUIRE(too_small.open_sender("budget", "Budget"));
  SYNC_REQUIRE(too_small.publish("budget", small.view) == PublishResult::Failed);
  SYNC_REQUIRE(budget_recorder.encode_calls == 0);

  RecordingConsumer transactional_recorder;
  std::array<MetalFrameConsumer*, 1> transactional_consumers{&transactional_recorder};
  MetalFramePublisher transactional(transactional_consumers,
                                     {.allocation_budget_bytes = exact_two_by_two});
  SYNC_REQUIRE(transactional.open_sender("budget", "Budget"));
  SYNC_REQUIRE(transactional.publish("budget", small.view) == PublishResult::Accepted);
  SYNC_REQUIRE(wait_until([&] { return transactional_recorder.completion_count.load() == 1; }));
  SYNC_REQUIRE(transactional.publish("budget", large.view) == PublishResult::Failed);
  small.view.sequence++;
  SYNC_REQUIRE(transactional.publish("budget", small.view) == PublishResult::Accepted);
  SYNC_REQUIRE(wait_until([&] { return transactional_recorder.completion_count.load() == 2; }));
}

SYNC_TEST(metal_publisher_resize_requires_peak_coexistence_budget) {
  RecordingConsumer consumer;
  std::array<MetalFrameConsumer*, 1> consumers{&consumer};
  auto old_frame = make_frame(2, 2, 8, 1);
  auto replacement_frame = make_frame(3, 2, 12, 2);
  const std::size_t old_ring_bytes = (256U * 2U + 2U * 2U * 4U) * 3U;
  const std::size_t replacement_ring_bytes = (256U * 2U + 3U * 2U * 4U) * 3U;
  SYNC_REQUIRE(old_ring_bytes < replacement_ring_bytes);
  SYNC_REQUIRE(replacement_ring_bytes < old_ring_bytes + replacement_ring_bytes);

  MetalFramePublisher publisher(consumers,
                                {.allocation_budget_bytes = replacement_ring_bytes});
  SYNC_REQUIRE(publisher.open_sender("peak", "Peak"));
  SYNC_REQUIRE(publisher.publish("peak", old_frame.view) == PublishResult::Accepted);
  SYNC_REQUIRE(wait_until([&] { return consumer.completion_count.load() == 1; }));
  SYNC_REQUIRE(consumer.output_copy(0) == old_frame.payload);

  const PublishResult resize_result = publisher.publish("peak", replacement_frame.view);
  if (resize_result == PublishResult::Accepted) {
    SYNC_REQUIRE(wait_until([&] { return consumer.completion_count.load() == 2; }));
  }
  SYNC_REQUIRE(resize_result == PublishResult::Failed);
  SYNC_REQUIRE(consumer.encode_calls == 1);

  old_frame.view.sequence = 3;
  SYNC_REQUIRE(publisher.publish("peak", old_frame.view) == PublishResult::Accepted);
  SYNC_REQUIRE(wait_until([&] { return consumer.completion_count.load() == 2; }));
  SYNC_REQUIRE(consumer.output_copy(1) == old_frame.payload);
}

SYNC_TEST(metal_publisher_rejects_invalid_views_and_consumer_failure_releases_slot) {
  RecordingConsumer consumer;
  std::array<MetalFrameConsumer*, 1> consumers{&consumer};
  MetalFramePublisher publisher(consumers);
  SYNC_REQUIRE(publisher.open_sender("invalid", "Invalid"));
  auto frame = make_frame(2, 2, 8);

  const auto expect_invalid = [&](const protocol::FrameView& candidate) {
    const auto before = consumer.encode_calls;
    SYNC_REQUIRE(publisher.publish("invalid", candidate) == PublishResult::Failed);
    SYNC_REQUIRE(consumer.encode_calls == before);
  };
  auto invalid = frame.view;
  invalid.top_down = false;
  expect_invalid(invalid);
  invalid = frame.view;
  invalid.pixel_format = 2;
  expect_invalid(invalid);
  invalid = frame.view;
  invalid.color_space = 7;
  expect_invalid(invalid);
  invalid = frame.view;
  invalid.alpha_mode = 7;
  expect_invalid(invalid);
  invalid = frame.view;
  invalid.row_stride = 7;
  expect_invalid(invalid);
  invalid = frame.view;
  invalid.payload_bytes--;
  expect_invalid(invalid);
  invalid = frame.view;
  invalid.payload = invalid.payload.first(invalid.payload.size() - 1);
  expect_invalid(invalid);
  invalid = frame.view;
  invalid.width = 0;
  expect_invalid(invalid);
  invalid = frame.view;
  invalid.width = 4097;
  expect_invalid(invalid);
  invalid = frame.view;
  invalid.height = 4097;
  expect_invalid(invalid);

  consumer.accept_encode = false;
  SYNC_REQUIRE(publisher.publish("invalid", frame.view) == PublishResult::Failed);
  consumer.accept_encode = true;
  SYNC_REQUIRE(publisher.publish("invalid", frame.view) == PublishResult::Accepted);
  SYNC_REQUIRE(wait_until([&] { return consumer.completion_count.load() == 1; }));
}

SYNC_TEST(metal_publisher_never_commits_a_consumer_rejected_command_buffer) {
  RejectingSignalConsumer consumer;
  std::array<MetalFrameConsumer*, 1> consumers{&consumer};
  MetalFramePublisher publisher(consumers);
  SYNC_REQUIRE(publisher.open_sender("reject", "Reject"));
  auto frame = make_frame(2, 2, 8);

  SYNC_REQUIRE(publisher.publish("reject", frame.view) == PublishResult::Failed);
  SYNC_REQUIRE(consumer.rejected_command_buffer != nil);
  SYNC_REQUIRE(consumer.rejected_command_buffer.status == MTLCommandBufferStatusNotEnqueued);
  SYNC_REQUIRE(!wait_until([&] { return consumer.event.signaledValue != 0; }, 50ms));
  SYNC_REQUIRE(consumer.event.signaledValue == 0);

  frame.view.sequence++;
  SYNC_REQUIRE(publisher.publish("reject", frame.view) == PublishResult::Accepted);
  SYNC_REQUIRE(wait_until([&] { return consumer.event.signaledValue == 2; }));
  SYNC_REQUIRE(consumer.accepted_command_buffer.status == MTLCommandBufferStatusCompleted);
}

}  // namespace
}  // namespace noisefactor::sync
