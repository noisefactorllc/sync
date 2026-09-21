#include <sync/audio_capture.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace noisefactor::sync::audio {
namespace {
std::size_t checked_size(unsigned rate, unsigned channels, std::size_t frames) {
  if (rate < 8000 || rate > 384000 || channels == 0 ||
      channels > kMaximumChannels || frames == 0 || frames > 65536)
    throw std::invalid_argument("Invalid audio capture format");
  return frames * channels;
}
void put(std::vector<std::byte> &bytes, std::size_t offset,
         std::uint64_t value, unsigned count) {
  for (unsigned i = 0; i < count; ++i)
    bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 255);
}
} // namespace

CaptureBuffer::CaptureBuffer(unsigned rate, unsigned channels, std::size_t capacity)
    : sample_rate_(rate), channels_(channels), capacity_(capacity),
      samples_(checked_size(rate, channels, capacity)) {}

void CaptureBuffer::push(std::span<const float> samples) noexcept {
  if (samples.size() % channels_ != 0 || samples.empty()) return;
  const auto frames = samples.size() / channels_;
  const auto start = next_frame_.fetch_add(frames, std::memory_order_relaxed);
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    for (int retry = 0; retry < 100; ++retry) {
#if defined(__aarch64__) || defined(__arm64__)
      asm volatile("isb" ::: "memory");
#elif defined(__x86_64__) || defined(_M_X64)
      _mm_pause();
#endif
      if (lock.try_lock()) break;
    }
  }
  if (!lock.owns_lock()) {
    dropped_frames_.fetch_add(frames, std::memory_order_relaxed);
    return;
  }
  if (first_frame_ + size_ != start) {
    dropped_frames_.fetch_add(size_, std::memory_order_relaxed);
    size_ = 0;
    first_frame_ = start;
  }
  const auto skip = frames > capacity_ ? frames - capacity_ : 0;
  const auto keep = frames - skip;
  const auto discard = size_ + keep > capacity_ ? size_ + keep - capacity_ : 0;
  dropped_frames_.fetch_add(discard + skip, std::memory_order_relaxed);
  head_ = (head_ + discard) % capacity_;
  first_frame_ += discard + skip;
  size_ -= discard;

  const auto write_pos = (head_ + size_) % capacity_;
  const auto first_chunk = std::min(keep, capacity_ - write_pos);
  const auto *src = samples.data() + skip * channels_;
  auto *dst = samples_.data() + write_pos * channels_;
  for (std::size_t i = 0; i < first_chunk * channels_; ++i) {
    dst[i] = std::isfinite(src[i]) ? src[i] : 0.0f;
  }
  if (keep > first_chunk) {
    const auto second_chunk = keep - first_chunk;
    const auto *src2 = src + first_chunk * channels_;
    auto *dst2 = samples_.data();
    for (std::size_t i = 0; i < second_chunk * channels_; ++i) {
      dst2[i] = std::isfinite(src2[i]) ? src2[i] : 0.0f;
    }
  }
  size_ += keep;
}

Packet CaptureBuffer::read() {
  Packet packet{sample_rate_, channels_, 0, 0, {}};
  packet.samples.resize(kMaximumPacketFrames * channels_);
  std::size_t frames = 0;
  {
    std::lock_guard lock(mutex_);
    frames = std::min<std::size_t>(size_, kMaximumPacketFrames);
    packet.first_frame = first_frame_;
    packet.dropped_frames = dropped_frames_.load(std::memory_order_relaxed);
    if (frames > 0) {
      // Keep ownership until the copy completes: push() can reuse consumed
      // slots immediately, or discard unread slots when the ring is full.
      const auto first_chunk = std::min(frames, capacity_ - head_);
      std::memcpy(packet.samples.data(),
                  samples_.data() + head_ * channels_,
                  first_chunk * channels_ * sizeof(float));
      if (frames > first_chunk) {
        std::memcpy(packet.samples.data() + first_chunk * channels_,
                    samples_.data(),
                    (frames - first_chunk) * channels_ * sizeof(float));
      }
      head_ = (head_ + frames) % capacity_;
      size_ -= frames;
      first_frame_ += frames;
    }
  }
  packet.samples.resize(frames * channels_);
  return packet;
}

std::vector<std::byte> encode_packet(const Packet &packet) {
  checked_size(packet.sample_rate, packet.channels, 1);
  if (packet.samples.size() % packet.channels != 0 ||
      packet.samples.size() / packet.channels > kMaximumPacketFrames)
    throw std::invalid_argument("Invalid audio packet length");
  std::vector<std::byte> bytes(32 + packet.samples.size() * 4);
  bytes[0] = std::byte{'N'}; bytes[1] = std::byte{'A'};
  bytes[2] = std::byte{'U'}; bytes[3] = std::byte{'D'};
  put(bytes, 4, 1, 2);
  put(bytes, 6, packet.channels, 2);
  put(bytes, 8, packet.sample_rate, 4);
  put(bytes, 12, packet.samples.size() / packet.channels, 4);
  put(bytes, 16, packet.first_frame, 8);
  put(bytes, 24, packet.dropped_frames, 8);
  if constexpr (std::endian::native == std::endian::little) {
    std::memcpy(bytes.data() + 32, packet.samples.data(), packet.samples.size() * sizeof(float));
  } else {
    for (std::size_t i = 0; i < packet.samples.size(); ++i)
      put(bytes, 32 + i * 4, std::bit_cast<std::uint32_t>(packet.samples[i]), 4);
  }
  return bytes;
}
} // namespace noisefactor::sync::audio
