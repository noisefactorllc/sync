#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include <sync/protocol.hpp>

namespace noisefactor::sync {

class H264Decoder {
 public:
  H264Decoder() noexcept;
  ~H264Decoder();
  H264Decoder(const H264Decoder&) = delete;
  auto operator=(const H264Decoder&) -> H264Decoder& = delete;

  // The output is RGBA8 and belongs to the caller. One Annex B access unit
  // must yield exactly one decoded image before this call returns.
  [[nodiscard]] auto decode(const protocol::FrameView& frame,
                            std::span<std::byte> output) noexcept -> bool;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace noisefactor::sync
