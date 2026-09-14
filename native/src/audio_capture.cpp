#include <sync/audio_capture.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

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
  for (std::size_t frame = 0; frame < keep; ++frame) {
    const auto target = ((head_ + size_ + frame) % capacity_) * channels_;
    for (unsigned channel = 0; channel < channels_; ++channel) {
      const auto value = samples[(skip + frame) * channels_ + channel];
      samples_[target + channel] = std::isfinite(value) ? value : 0.0f;
    }
  }
  size_ += keep;
}

Packet CaptureBuffer::read() {
  std::lock_guard lock(mutex_);
  Packet packet{sample_rate_, channels_, first_frame_,
                dropped_frames_.load(std::memory_order_relaxed), {}};
  const auto frames = std::min<std::size_t>(size_, kMaximumPacketFrames);
  packet.samples.resize(frames * channels_);
  for (std::size_t frame = 0; frame < frames; ++frame)
    std::copy_n(samples_.data() + ((head_ + frame) % capacity_) * channels_,
                channels_, packet.samples.data() + frame * channels_);
  head_ = (head_ + frames) % capacity_;
  size_ -= frames;
  first_frame_ += frames;
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
  for (std::size_t i = 0; i < packet.samples.size(); ++i)
    put(bytes, 32 + i * 4, std::bit_cast<std::uint32_t>(packet.samples[i]), 4);
  return bytes;
}
} // namespace noisefactor::sync::audio
