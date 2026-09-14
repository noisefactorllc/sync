#include <sync/audio_capture.hpp>
#include <sync/control.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
namespace audio = noisefactor::sync::audio;
using namespace std::chrono_literals;

constexpr auto kCaptureDuration = 2s;
constexpr auto kPollInterval = 5ms;
constexpr unsigned kJackChannels = 32;
constexpr unsigned kJackSampleRate = 48'000;

struct Options {
  bool list = false;
  bool expect_jack_pattern = false;
  std::string source_id;
};

[[noreturn]] void usage_error(std::string_view message) {
  throw std::invalid_argument(std::string(message) +
      "; usage: sync_audio_native_probe [--list] | "
      "--source-id <id> [--expect-jack-pattern]");
}

Options parse_options(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--list") {
      if (options.list) usage_error("--list may be specified only once");
      options.list = true;
    } else if (argument == "--source-id") {
      if (!options.source_id.empty())
        usage_error("--source-id may be specified only once");
      if (++index == argc || std::string_view(argv[index]).empty())
        usage_error("--source-id requires a nonempty value");
      options.source_id = argv[index];
    } else if (argument == "--expect-jack-pattern") {
      if (options.expect_jack_pattern)
        usage_error("--expect-jack-pattern may be specified only once");
      options.expect_jack_pattern = true;
    } else {
      usage_error("unknown argument: " + std::string(argument));
    }
  }
  if (options.list && (!options.source_id.empty() || options.expect_jack_pattern))
    usage_error("--list cannot be combined with capture options");
  if (options.expect_jack_pattern && options.source_id.empty())
    usage_error("--expect-jack-pattern requires --source-id");
  return options;
}

void append_json_string(std::string &output, std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  output.push_back('"');
  for (const unsigned char byte : value) {
    switch (byte) {
    case '"': output.append("\\\""); break;
    case '\\': output.append("\\\\"); break;
    case '\b': output.append("\\b"); break;
    case '\f': output.append("\\f"); break;
    case '\n': output.append("\\n"); break;
    case '\r': output.append("\\r"); break;
    case '\t': output.append("\\t"); break;
    default:
      if (byte < 0x20) {
        output.append("\\u00");
        output.push_back(hex[byte >> 4U]);
        output.push_back(hex[byte & 0x0fU]);
      } else {
        output.push_back(static_cast<char>(byte));
      }
    }
  }
  output.push_back('"');
}

float expected_jack_sample(unsigned channel) {
  const auto magnitude = static_cast<float>(channel + 1) / 64.0f;
  return channel % 2 == 0 ? magnitude : -magnitude;
}

struct Statistics {
  unsigned sample_rate = 0;
  unsigned channels = 0;
  std::uint64_t read_calls = 0;
  std::uint64_t packets = 0;
  std::uint64_t received_frames = 0;
  std::uint64_t finite_samples = 0;
  std::uint64_t nonfinite_samples = 0;
  std::uint64_t first_frame = 0;
  std::uint64_t last_frame_exclusive = 0;
  std::uint64_t cursor_discontinuities = 0;
  std::uint64_t dropped_frames = 0;
  std::uint64_t startup_frames_ignored = 0;
  std::uint64_t pattern_frames = 0;
  std::uint64_t pattern_mismatches = 0;
  bool have_format = false;
  bool have_frames = false;
  bool pattern_started = false;
};

void inspect_packet(const audio::Packet &packet, bool expect_jack_pattern,
                    Statistics &statistics) {
  ++statistics.read_calls;
  if (packet.sample_rate < 8'000 || packet.sample_rate > 384'000 ||
      packet.channels == 0 || packet.channels > audio::kMaximumChannels)
    throw std::runtime_error("Native audio returned an invalid format");
  if (!statistics.have_format) {
    statistics.sample_rate = packet.sample_rate;
    statistics.channels = packet.channels;
    statistics.have_format = true;
  } else if (packet.sample_rate != statistics.sample_rate ||
             packet.channels != statistics.channels) {
    throw std::runtime_error("Native audio format changed during capture");
  }
  if (packet.samples.size() % packet.channels != 0)
    throw std::runtime_error("Native audio returned a partial frame");
  const auto frame_count = packet.samples.size() / packet.channels;
  if (frame_count > audio::kMaximumPacketFrames)
    throw std::runtime_error("Native audio returned an oversized packet");
  statistics.dropped_frames =
      std::max(statistics.dropped_frames, packet.dropped_frames);
  if (frame_count == 0) return;

  ++statistics.packets;
  if (!statistics.have_frames) {
    statistics.first_frame = packet.first_frame;
    statistics.have_frames = true;
  } else if (packet.first_frame != statistics.last_frame_exclusive) {
    ++statistics.cursor_discontinuities;
  }
  statistics.last_frame_exclusive = packet.first_frame + frame_count;
  statistics.received_frames += frame_count;

  for (std::size_t frame = 0; frame < frame_count; ++frame) {
    bool frame_matches_pattern = expect_jack_pattern;
    bool frame_is_silent = true;
    std::uint64_t frame_pattern_mismatches = 0;
    for (unsigned channel = 0; channel < packet.channels; ++channel) {
      const auto sample = packet.samples[frame * packet.channels + channel];
      if (std::isfinite(sample)) {
        ++statistics.finite_samples;
      } else {
        ++statistics.nonfinite_samples;
      }
      if (sample != 0.0f) frame_is_silent = false;
      if (expect_jack_pattern && sample != expected_jack_sample(channel)) {
        frame_matches_pattern = false;
        ++frame_pattern_mismatches;
      }
    }
    if (!expect_jack_pattern) continue;
    if (!statistics.pattern_started) {
      if (!frame_matches_pattern) {
        if (frame_is_silent) {
          ++statistics.startup_frames_ignored;
        } else {
          statistics.pattern_mismatches += frame_pattern_mismatches;
        }
        continue;
      }
      statistics.pattern_started = true;
    }
    ++statistics.pattern_frames;
    statistics.pattern_mismatches += frame_pattern_mismatches;
  }
}

std::string capture_report(std::string_view source_id,
                           const Statistics &statistics,
                           bool expect_jack_pattern, bool qualified) {
  std::string output = "{\"type\":\"audioNativeProbe\",\"sourceId\":";
  append_json_string(output, source_id);
  output.append(",\"captureMilliseconds\":2000,\"sampleRate\":");
  output.append(std::to_string(statistics.sample_rate));
  output.append(",\"channelCount\":");
  output.append(std::to_string(statistics.channels));
  output.append(",\"readCalls\":");
  output.append(std::to_string(statistics.read_calls));
  output.append(",\"packets\":");
  output.append(std::to_string(statistics.packets));
  output.append(",\"receivedFrames\":");
  output.append(std::to_string(statistics.received_frames));
  output.append(",\"finiteSamples\":");
  output.append(std::to_string(statistics.finite_samples));
  output.append(",\"nonfiniteSamples\":");
  output.append(std::to_string(statistics.nonfinite_samples));
  output.append(",\"allSamplesFinite\":");
  output.append(statistics.nonfinite_samples == 0 ? "true" : "false");
  output.append(",\"firstFrame\":");
  output.append(std::to_string(statistics.first_frame));
  output.append(",\"lastFrameExclusive\":");
  output.append(std::to_string(statistics.last_frame_exclusive));
  output.append(",\"cursorDiscontinuities\":");
  output.append(std::to_string(statistics.cursor_discontinuities));
  output.append(",\"droppedFrames\":");
  output.append(std::to_string(statistics.dropped_frames));
  if (expect_jack_pattern) {
    output.append(",\"jackPattern\":{\"startupFramesIgnored\":");
    output.append(std::to_string(statistics.startup_frames_ignored));
    output.append(",\"validatedFrames\":");
    output.append(std::to_string(statistics.pattern_frames));
    output.append(",\"mismatches\":");
    output.append(std::to_string(statistics.pattern_mismatches));
    output.push_back('}');
  }
  output.append(",\"qualified\":");
  output.append(qualified ? "true}" : "false}");
  return output;
}

int capture_source(audio::InputBackend &backend, const Options &options) {
  const auto sources = backend.sources();
  if (std::ranges::none_of(sources, [&](const audio::Source &source) {
        return source.id == options.source_id;
      }))
    throw std::runtime_error("Native audio source id was not found");

  auto capture = backend.open(options.source_id);
  Statistics statistics;
  const auto deadline = std::chrono::steady_clock::now() + kCaptureDuration;
  while (std::chrono::steady_clock::now() < deadline) {
    inspect_packet(capture->read(), options.expect_jack_pattern, statistics);
    std::this_thread::sleep_for(kPollInterval);
  }

  bool qualified = statistics.have_format && statistics.received_frames != 0 &&
                   statistics.nonfinite_samples == 0 &&
                   statistics.cursor_discontinuities == 0 &&
                   statistics.dropped_frames == 0;
  std::string failure;
  if (!statistics.have_format || statistics.received_frames == 0)
    failure = "Native audio produced no frames";
  else if (statistics.nonfinite_samples != 0)
    failure = "Native audio produced nonfinite samples";
  else if (statistics.cursor_discontinuities != 0 || statistics.dropped_frames != 0)
    failure = "Native audio capture was discontinuous";

  if (options.expect_jack_pattern) {
    const bool format_matches = statistics.channels == kJackChannels &&
                                statistics.sample_rate == kJackSampleRate;
    const bool pattern_matches = statistics.pattern_started &&
        statistics.pattern_frames >= kJackSampleRate &&
        statistics.pattern_mismatches == 0;
    qualified = qualified && format_matches && pattern_matches;
    if (failure.empty() && !format_matches)
      failure = "JACK source did not open as 32-channel 48 kHz audio";
    else if (failure.empty() && !statistics.pattern_started)
      failure = "JACK pattern did not begin after startup silence";
    else if (failure.empty() && statistics.pattern_frames < kJackSampleRate)
      failure = "JACK pattern provided fewer than 48000 validated frames";
    else if (failure.empty() && statistics.pattern_mismatches != 0)
      failure = "JACK pattern contained sample mismatches";
  }

  std::cout << capture_report(options.source_id, statistics,
                              options.expect_jack_pattern, qualified) << '\n';
  if (!qualified) {
    std::cerr << "sync_audio_native_probe: " << failure << '\n';
    return 1;
  }
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  Options options;
  try {
    options = parse_options(argc, argv);
  } catch (const std::invalid_argument &error) {
    std::cerr << "sync_audio_native_probe: " << error.what() << '\n';
    return 2;
  }
  try {
    auto backend = audio::make_native_input_backend();
    if (options.source_id.empty()) {
      std::cout << noisefactor::sync::control::encode_audio_sources(
                       backend->sources())
                << '\n';
      return 0;
    }
    return capture_source(*backend, options);
  } catch (const std::exception &error) {
    std::cerr << "sync_audio_native_probe: " << error.what() << '\n';
    return 1;
  }
}
