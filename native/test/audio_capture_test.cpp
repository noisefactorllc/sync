#include "test_harness.hpp"
#include <sync/audio_capture.hpp>
#include <array>
#include <limits>

namespace audio = noisefactor::sync::audio;

SYNC_TEST(audio_capture_keeps_all_32_channels_and_frame_order) {
  audio::CaptureBuffer input(48000, 32, 1024);
  std::vector<float> samples(640 * 32);
  for (std::size_t frame = 0; frame < 640; ++frame)
    for (std::size_t channel = 0; channel < 32; ++channel)
      samples[frame * 32 + channel] = static_cast<float>(frame * 32 + channel);
  input.push(samples);
  auto first = input.read();
  SYNC_REQUIRE(first.channels == 32);
  SYNC_REQUIRE(first.sample_rate == 48000);
  SYNC_REQUIRE(first.first_frame == 0);
  SYNC_REQUIRE(first.samples.size() == 480 * 32);
  SYNC_REQUIRE(first.samples[31] == 31);
  SYNC_REQUIRE(first.samples.back() == 15359);
  auto second = input.read();
  SYNC_REQUIRE(second.first_frame == 480);
  SYNC_REQUIRE(second.samples.size() == 160 * 32);
  SYNC_REQUIRE(second.samples.front() == 15360);
  SYNC_REQUIRE(second.samples.back() == 20479);
  SYNC_REQUIRE(input.read().samples.empty());
}

SYNC_TEST(audio_capture_discards_oldest_frames_at_its_fixed_capacity) {
  audio::CaptureBuffer input(44100, 2, 3);
  input.push(std::array<float, 4>{1, 11, 2, 12});
  input.push(std::array<float, 4>{3, 13, 4, 14});
  const auto packet = input.read();
  SYNC_REQUIRE(packet.first_frame == 1);
  SYNC_REQUIRE(packet.dropped_frames == 1);
  SYNC_REQUIRE((packet.samples == std::vector<float>{2, 12, 3, 13, 4, 14}));
}

SYNC_TEST(audio_capture_bounds_oversized_callbacks_and_sanitizes_nonfinite_samples) {
  audio::CaptureBuffer input(48000, 1, 2);
  input.push(std::array<float, 4>{1, 2, -0.5f, std::numeric_limits<float>::quiet_NaN()});
  const auto packet = input.read();
  SYNC_REQUIRE(packet.first_frame == 2);
  SYNC_REQUIRE(packet.dropped_frames == 2);
  SYNC_REQUIRE((packet.samples == std::vector<float>{-0.5f, 0}));
}

SYNC_TEST(audio_wire_packet_has_little_endian_header_and_signed_pcm) {
  audio::CaptureBuffer input(48000, 2);
  input.push(std::array<float, 4>{0.5f, -0.5f, 1.0f, -1.0f});
  const auto bytes = audio::encode_packet(input.read());
  SYNC_REQUIRE(bytes.size() == 48);
  const std::array<unsigned char, 16> header{'N', 'A', 'U', 'D', 1, 0, 2, 0,
      0x80, 0xbb, 0, 0, 2, 0, 0, 0};
  for (std::size_t i = 0; i < header.size(); ++i)
    SYNC_REQUIRE(std::to_integer<unsigned char>(bytes[i]) == header[i]);
  SYNC_REQUIRE(std::to_integer<unsigned char>(bytes[35]) == 0x3f);
  SYNC_REQUIRE(std::to_integer<unsigned char>(bytes[39]) == 0xbf);
}

SYNC_TEST(audio_capture_rejects_invalid_channel_layouts_before_allocating) {
  for (const auto channels : {0u, 33u}) {
    bool rejected = false;
    try { audio::CaptureBuffer input(48000, channels); }
    catch (const std::invalid_argument&) { rejected = true; }
    SYNC_REQUIRE(rejected);
  }
}
