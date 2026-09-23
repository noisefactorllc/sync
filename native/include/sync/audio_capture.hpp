#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace noisefactor::sync::audio {

inline constexpr unsigned kMaximumChannels = 32;
inline constexpr unsigned kMaximumPacketFrames = 480;

struct Packet {
  unsigned sample_rate = 0;
  unsigned channels = 0;
  std::uint64_t first_frame = 0;
  std::uint64_t dropped_frames = 0;
  std::vector<float> samples;
};

// The device callback retains a bounded backlog for temporary reader stalls.
class CaptureBuffer {
public:
  CaptureBuffer(unsigned sample_rate, unsigned channels,
                std::size_t capacity_frames = 65536);
  void push(std::span<const float> samples) noexcept;
  Packet read();
private:
  unsigned sample_rate_;
  unsigned channels_;
  std::size_t capacity_;
  std::vector<float> samples_;
  std::mutex mutex_;
  std::atomic<std::uint64_t> next_frame_{0};
  std::atomic<std::uint64_t> dropped_frames_{0};
  std::uint64_t first_frame_ = 0;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
};

std::vector<std::byte> encode_packet(const Packet &packet);

struct Source {
  std::string id;
  std::string name;
  unsigned channels = 0;
  unsigned sample_rate = 0;
};

class Capture {
public:
  virtual ~Capture() = default;
  virtual Packet read() = 0;
};

class InputBackend {
public:
  virtual ~InputBackend() = default;
  virtual std::vector<Source> sources() = 0;
  virtual std::unique_ptr<Capture> open(const std::string &source_id) = 0;
};

std::unique_ptr<InputBackend> make_native_input_backend();

} // namespace noisefactor::sync::audio
