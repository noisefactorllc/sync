#pragma once

#if !defined(__APPLE__)
#error "syphon_consumer.hpp is available only on Apple platforms"
#endif

#include <cstddef>
#include <memory>
#include <string_view>

#include <sync/platform/metal_frame_consumer.hpp>

namespace noisefactor::sync {

class SyphonMetalConsumer final : public MetalFrameConsumer {
 public:
  static constexpr std::size_t kMaximumSenderEntries = 8;

  struct Options {
    std::string_view framework_path{};
  };

  SyphonMetalConsumer();
  explicit SyphonMetalConsumer(Options options);
  ~SyphonMetalConsumer() override;

  SyphonMetalConsumer(const SyphonMetalConsumer&) = delete;
  auto operator=(const SyphonMetalConsumer&) -> SyphonMetalConsumer& = delete;
  SyphonMetalConsumer(SyphonMetalConsumer&&) = delete;
  auto operator=(SyphonMetalConsumer&&) -> SyphonMetalConsumer& = delete;

  [[nodiscard]] auto available() const noexcept -> bool;
  auto open_sender(std::string_view sender_id,
                   std::string_view name,
                   id<MTLDevice> device) noexcept -> bool override;
  void close_sender(std::string_view sender_id) noexcept override;
  auto encode_frame(std::string_view sender_id,
                    id<MTLTexture> texture,
                    id<MTLCommandBuffer> command_buffer,
                    const MetalFrameMetadata& metadata) noexcept -> bool override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace noisefactor::sync
