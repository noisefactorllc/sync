#pragma once

#if !defined(__APPLE__)
#error "syphon_consumer.hpp is available only on Apple platforms"
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include <sync/platform/metal_frame_consumer.hpp>

namespace noisefactor::sync {

// Why discovery ended without a usable SyphonMetalServer. `available()` alone
// answered "no" and nothing else, which is a dead end for whoever has to fix
// it: a framework that is absent, one that will not load, and one that loads
// but carries none of Syphon's classes all look identical from outside.
enum class SyphonUnavailableReason : std::uint8_t {
  None = 0,
  FrameworkNotFound,
  FrameworkLoadFailed,
  ServerClassMissing,
  ServerClassIncompatible,
  DiscoveryFailed,
};

// A short phrase per reason, safe to print and to paste into a bug report: it
// names the class of problem and never a path, a token, or a hash.
[[nodiscard]] auto describe(SyphonUnavailableReason reason) noexcept -> const char*;

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

  // SyphonUnavailableReason::None exactly when available() is true.
  [[nodiscard]] auto unavailable_reason() const noexcept -> SyphonUnavailableReason;
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
