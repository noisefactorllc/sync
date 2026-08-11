#include <sync/platform/metal_frame_consumer.hpp>
#include <sync/platform/metal_frame_publisher.hpp>
#include <sync/protocol.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>

namespace {

class ProbeConsumer final : public noisefactor::sync::MetalFrameConsumer {
 public:
  explicit ProbeConsumer(std::shared_ptr<std::atomic<bool>> completion)
      : completion_(std::move(completion)) {}

  auto open_sender(std::string_view, std::string_view, id<MTLDevice>) noexcept -> bool override {
    return true;
  }

  void close_sender(std::string_view) noexcept override {}

  auto encode_frame(std::string_view,
                    id<MTLTexture>,
                    id<MTLCommandBuffer> command_buffer,
                    const noisefactor::sync::MetalFrameMetadata&) noexcept -> bool override {
    const auto completion = completion_;
    [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
      @autoreleasepool {
        completion->store(true, std::memory_order_release);
      }
    }];
    return true;
  }

 private:
  std::shared_ptr<std::atomic<bool>> completion_;
};

}  // namespace

int main() {
  using noisefactor::sync::MetalFrameConsumer;
  using noisefactor::sync::MetalFramePublisher;
  using noisefactor::sync::PublishResult;

  const auto completion = std::make_shared<std::atomic<bool>>(false);
  ProbeConsumer consumer(completion);
  std::array<MetalFrameConsumer*, 1> consumers{&consumer};
  std::array<std::byte, 16> payload{
      std::byte{0xff}, std::byte{0x00}, std::byte{0x00}, std::byte{0xff},
      std::byte{0x00}, std::byte{0xff}, std::byte{0x00}, std::byte{0xff},
      std::byte{0x00}, std::byte{0x00}, std::byte{0xff}, std::byte{0xff},
      std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
  };
  const noisefactor::sync::protocol::FrameView frame{
      .version = 1,
      .header_bytes = 64,
      .flags = 1,
      .pixel_format = 1,
      .color_space = 1,
      .alpha_mode = 1,
      .width = 2,
      .height = 2,
      .row_stride = 8,
      .payload_bytes = static_cast<std::uint32_t>(payload.size()),
      .sequence = 1,
      .presentation_time_us = 1000,
      .top_down = true,
      .payload = payload,
  };

  {
    MetalFramePublisher publisher(consumers);
    if (!publisher.available()) {
      return 2;
    }
    if (!publisher.open_sender("probe", "Probe")) {
      return 3;
    }
    if (publisher.publish("probe", frame) != PublishResult::Accepted) {
      return 4;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!completion->load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!completion->load(std::memory_order_acquire)) {
      return 5;
    }
    publisher.close_sender("probe");
  }

  std::cout << "{\"published\":true}\n";
  return 0;
}
