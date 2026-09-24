#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

@protocol SyncSyphonServerDirectory <NSObject>
+ (id<SyncSyphonServerDirectory>)sharedDirectory;
- (NSArray<NSDictionary<NSString*, id>*>*)serversMatchingName:(NSString*)name
                                                      appName:(NSString*)appName;
@end

@protocol SyncSyphonMetalClient <NSObject>
- (id)initWithServerDescription:(NSDictionary<NSString*, id>*)description
                         device:(id<MTLDevice>)device
                        options:(NSDictionary<NSString*, id>*)options
                newFrameHandler:(void (^)(id<SyncSyphonMetalClient> client))handler;
- (id<MTLTexture>)newFrameImage;
- (void)stop;
- (BOOL)isValid;
@end

namespace {

constexpr std::size_t kMarkerBytes = 28;
constexpr std::size_t kReadbackBytesPerRow = 1024;
constexpr std::size_t kReadbackSlots = 4;
constexpr std::array<std::uint8_t, 4> kMarkerSignature{{'S', 'Y', 'N', 'C'}};

struct Options {
  std::string framework_path;
  std::string server_name;
  std::uint32_t duration_ms = 0;
  std::uint32_t discovery_timeout_ms = 0;
  std::uint32_t expected_width = 0;
  std::uint32_t expected_height = 0;
  bool nv12_marker = false;
  bool h264_marker = false;
  bool plain_video = false;
  bool plain_content = false;
};

struct Sample {
  std::uint64_t sequence = 0;
  std::uint64_t presentation_time_us = 0;
  std::uint64_t browser_send_time_us = 0;
  std::int64_t latency_us = 0;
  std::int64_t browser_age_us = 0;
  std::int64_t transport_latency_us = 0;
  // The browser stamps wall time as timeOrigin + performance.now(), fixed at
  // page load; this probe reads the system clock. When the system clock is
  // slewed during a long run the two drift apart and a fast frame can read as
  // a negative latency. Such a frame still arrived: count it, but keep it out
  // of the latency statistics.
  bool clock_skewed = false;
};

struct ReadbackSlot {
  id<MTLBuffer> __strong buffer = nil;
  std::atomic<bool> busy{false};
};

struct ProbeState {
  std::array<ReadbackSlot, kReadbackSlots> slots;
  std::atomic<std::uint64_t> frames_seen{0};
  std::atomic<std::uint64_t> probe_dropped{0};
  std::atomic<std::uint64_t> invalid_markers{0};
  std::atomic<std::uint64_t> dimension_mismatches{0};
  std::atomic<std::uint64_t> command_errors{0};
  std::atomic<std::uint64_t> in_flight{0};
  std::atomic<std::uint64_t> plain_frames{0};
  std::atomic<std::int64_t> first_plain_us{0};
  std::atomic<std::int64_t> last_plain_us{0};
  std::mutex content_mutex;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> content_hashes;
  std::mutex samples_mutex;
  std::vector<Sample> samples;
  std::size_t max_samples = 0;
  // Syphon's -stop holds the client's lock while it waits for the frame
  // queue, and -newFrameImage, called from a frame handler on that queue,
  // takes the same lock: a stop that meets an arriving frame deadlocks
  // (SyphonClientBase -stopBase, Syphon 71351d4 and upstream). This probe
  // hung in 2 of 150 stops against a 60 fps server. Handlers stand down
  // before every stop instead; see stop_client below.
  std::atomic<bool> standing_down{false};
  std::atomic<int> handlers_running{0};
};

// Counts a frame handler out however it returns.
struct HandlerScope {
  ProbeState* state;
  ~HandlerScope() { state->handlers_running.fetch_sub(1); }
};

auto parse_u32(std::string_view value, std::uint32_t& output) noexcept -> bool {
  if (value.empty()) return false;
  std::uint32_t parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() || parsed == 0) {
    return false;
  }
  output = parsed;
  return true;
}

auto parse_options(int argc, char** argv, Options& options) noexcept -> bool {
  if (argc != 7 && argc != 8) return false;
  if (argc == 8) {
    const std::string_view marker(argv[7]);
    if (marker != "nv12" && marker != "h264" && marker != "plain" &&
        marker != "plain-content") return false;
    options.nv12_marker = marker == "nv12";
    options.h264_marker = marker == "h264";
    options.plain_video = marker == "plain" || marker == "plain-content";
    options.plain_content = marker == "plain-content";
  }
  options.framework_path = argv[1];
  options.server_name = argv[2];
  const bool parsed = !options.framework_path.empty() && !options.server_name.empty() &&
         parse_u32(argv[3], options.duration_ms) &&
         parse_u32(argv[4], options.discovery_timeout_ms) &&
         parse_u32(argv[5], options.expected_width) &&
         parse_u32(argv[6], options.expected_height);
  return parsed && (!options.plain_content ||
                    (options.expected_width >= 16U && options.expected_height >= 16U));
}

auto exact_string(std::string_view bytes) noexcept -> NSString* {
  if (bytes.empty()) return nil;
  return [[NSString alloc] initWithBytes:bytes.data()
                                  length:bytes.size()
                                encoding:NSUTF8StringEncoding];
}

auto select_metal_device() noexcept -> id<MTLDevice> {
  @try {
    NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
    id<MTLDevice> selected = nil;
    for (id<MTLDevice> device in devices) {
      if (selected == nil ||
          (selected.removable && !device.removable) ||
          (selected.removable == device.removable && selected.headless && !device.headless) ||
          (selected.removable == device.removable && selected.headless == device.headless &&
           device.registryID < selected.registryID)) {
        selected = device;
      }
    }
    return selected;
  } @catch (NSException*) {
    return nil;
  }
}

auto now_us() noexcept -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

auto read_u64_le(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept
    -> std::uint64_t {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
  }
  return value;
}

auto decode_marker(const void* raw_bytes, Sample& sample,
                   bool nv12_marker, bool h264_marker) noexcept -> bool {
  if (raw_bytes == nullptr) return false;
  const auto* bgra = static_cast<const std::uint8_t*>(raw_bytes);
  std::array<std::uint8_t, kMarkerBytes> rgba{};
  if (h264_marker) {
    for (std::size_t bit = 0; bit < kMarkerBytes * 8U; ++bit) {
      const std::size_t x = (bit % 16U) * 8U + 4U;
      const std::size_t y = (bit / 16U) * 8U + 4U;
      if (bgra[y * kReadbackBytesPerRow + x * 4U] > 127) {
        rgba[bit / 8U] |= static_cast<std::uint8_t>(1U << (bit % 8U));
      }
    }
  } else if (nv12_marker) {
    for (std::size_t byte = 0; byte < rgba.size(); ++byte) {
      for (std::size_t bit = 0; bit < 8; ++bit) {
        if (bgra[(byte * 8U + bit) * 4U] > 127) {
          rgba[byte] |= static_cast<std::uint8_t>(1U << bit);
        }
      }
    }
  } else {
    for (std::size_t pixel = 0; pixel < kMarkerBytes / 4U; ++pixel) {
      rgba[pixel * 4U] = bgra[pixel * 4U + 2U];
      rgba[pixel * 4U + 1U] = bgra[pixel * 4U + 1U];
      rgba[pixel * 4U + 2U] = bgra[pixel * 4U];
      rgba[pixel * 4U + 3U] = bgra[pixel * 4U + 3U];
    }
  }
  if (!std::equal(kMarkerSignature.begin(), kMarkerSignature.end(), rgba.begin())) {
    return false;
  }
  sample.sequence = read_u64_le(rgba, 4);
  sample.presentation_time_us = read_u64_le(rgba, 12);
  sample.browser_send_time_us = read_u64_le(rgba, 20);
  if (sample.sequence == 0 || sample.presentation_time_us == 0 ||
      sample.browser_send_time_us < sample.presentation_time_us ||
      sample.presentation_time_us >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      sample.browser_send_time_us >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  const std::int64_t received_time_us = now_us();
  sample.latency_us =
      received_time_us - static_cast<std::int64_t>(sample.presentation_time_us);
  sample.browser_age_us = static_cast<std::int64_t>(sample.browser_send_time_us) -
                          static_cast<std::int64_t>(sample.presentation_time_us);
  sample.transport_latency_us =
      received_time_us - static_cast<std::int64_t>(sample.browser_send_time_us);
  if (sample.browser_age_us < 0) return false;
  sample.clock_skewed = sample.latency_us < 0 || sample.transport_latency_us < 0;
  return true;
}

auto percentile(std::vector<std::int64_t> sorted, double fraction) -> std::int64_t {
  if (sorted.empty()) return 0;
  std::sort(sorted.begin(), sorted.end());
  const double rank = fraction * static_cast<double>(sorted.size() - 1U);
  const auto index = static_cast<std::size_t>(rank + 0.5);
  return sorted[std::min(index, sorted.size() - 1U)];
}

void run_loop_for(std::chrono::milliseconds duration) {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const auto slice = std::min(remaining, std::chrono::milliseconds(10));
    [[NSRunLoop currentRunLoop]
        runUntilDate:[NSDate dateWithTimeIntervalSinceNow:
                                 std::chrono::duration<double>(slice).count()]];
  }
}

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    Options options;
    if (!parse_options(argc, argv, options)) {
      std::cerr << "usage: sync_syphon_receiver_probe <Syphon.framework> <server-name> "
                   "<duration-ms> <discovery-timeout-ms> <width> <height> [nv12|h264|plain|plain-content]\n";
      return 2;
    }

    NSString* framework_path = exact_string(options.framework_path);
    NSString* server_name = exact_string(options.server_name);
    if (framework_path == nil || server_name == nil) {
      std::cerr << "sync_syphon_receiver_probe: paths and names must be valid UTF-8\n";
      return 2;
    }

    NSBundle* bundle = [NSBundle bundleWithPath:framework_path];
    NSError* load_error = nil;
    if (bundle == nil || ![bundle loadAndReturnError:&load_error]) {
      std::cerr << "sync_syphon_receiver_probe: failed to load Syphon.framework\n";
      return 1;
    }

    Class directory_class = NSClassFromString(@"SyphonServerDirectory");
    Class client_class = NSClassFromString(@"SyphonMetalClient");
    if (directory_class == Nil || client_class == Nil ||
        ![directory_class respondsToSelector:@selector(sharedDirectory)] ||
        ![client_class instancesRespondToSelector:
                           @selector(initWithServerDescription:device:options:newFrameHandler:)] ||
        ![client_class instancesRespondToSelector:@selector(newFrameImage)] ||
        ![client_class instancesRespondToSelector:@selector(stop)]) {
      std::cerr << "sync_syphon_receiver_probe: incompatible Syphon.framework\n";
      return 1;
    }

    id<MTLDevice> device = select_metal_device();
    id<MTLCommandQueue> command_queue = [device newCommandQueue];
    if (device == nil || command_queue == nil) {
      std::cerr << "sync_syphon_receiver_probe: Metal is unavailable\n";
      return 1;
    }

    id<SyncSyphonServerDirectory> directory =
        [(Class<SyncSyphonServerDirectory>)directory_class sharedDirectory];
    NSDictionary<NSString*, id>* description = nil;
    const auto discovery_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(options.discovery_timeout_ms);
    while (description == nil && std::chrono::steady_clock::now() < discovery_deadline) {
      NSArray<NSDictionary<NSString*, id>*>* matches =
          [directory serversMatchingName:server_name appName:nil];
      if (matches.count > 0) description = matches.firstObject;
      if (description == nil) run_loop_for(std::chrono::milliseconds(10));
    }
    if (description == nil) {
      NSArray<NSDictionary<NSString*, id>*>* discovered =
          [directory serversMatchingName:nil appName:nil];
      const char* discovered_description = discovered.description.UTF8String;
      std::cerr << "sync_syphon_receiver_probe: discovered servers: "
                << (discovered_description == nullptr
                        ? "(unavailable)"
                        : discovered_description)
                << '\n';
      std::cout << "{\"available\":true,\"serverFound\":false,\"framesSeen\":0,"
                   "\"markers\":0,\"dimensionMismatches\":0,\"commandErrors\":0}\n";
      return 1;
    }

    auto state = std::make_shared<ProbeState>();
    for (ReadbackSlot& slot : state->slots) {
      slot.buffer = [device newBufferWithLength:kReadbackBytesPerRow *
                                               (options.h264_marker ? 112U :
                                                options.plain_content ? 16U : 1U)
                                        options:MTLResourceStorageModeShared];
      if (slot.buffer == nil) {
        std::cerr << "sync_syphon_receiver_probe: failed to allocate readback slots\n";
        return 1;
      }
    }

    // Keep a sample for every marked frame of the whole run, at up to 125 fps.
    // A fixed ten-minute cap made longer runs report at most 36,000 markers.
    state->max_samples = static_cast<std::size_t>(options.duration_ms) / 8U + 1024U;
    state->samples.reserve(state->max_samples);

    __block id<SyncSyphonMetalClient> client = nil;
    void (^frame_handler)(id<SyncSyphonMetalClient>) =
        ^(id<SyncSyphonMetalClient> callback_client) {
                    // Counted in before the flag is read, and stop_client sets
                    // the flag before it reads the count: a handler either
                    // finishes before the stop begins or never touches the
                    // client's lock.
                    state->handlers_running.fetch_add(1);
                    const HandlerScope scope{state.get()};
                    if (state->standing_down.load()) return;
                    const auto frame_index =
                        state->frames_seen.fetch_add(1, std::memory_order_relaxed) + 1U;
                    ReadbackSlot* claimed = nullptr;
                    for (ReadbackSlot& slot : state->slots) {
                      bool expected = false;
                      if (slot.busy.compare_exchange_strong(expected, true,
                                                            std::memory_order_acq_rel)) {
                        claimed = &slot;
                        break;
                      }
                    }
                    if (claimed == nullptr) {
                      state->probe_dropped.fetch_add(1, std::memory_order_relaxed);
                      return;
                    }

                    @autoreleasepool {
                      id<MTLTexture> texture = [callback_client newFrameImage];
                      if (texture == nil) {
                        claimed->busy.store(false, std::memory_order_release);
                        state->invalid_markers.fetch_add(1, std::memory_order_relaxed);
                        return;
                      }
                      if (texture.width != options.expected_width ||
                          texture.height != options.expected_height) {
                        claimed->busy.store(false, std::memory_order_release);
                        state->dimension_mismatches.fetch_add(
                            1, std::memory_order_relaxed);
                        return;
                      }
                      if (options.plain_video) {
                        const auto observed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        std::int64_t first = 0;
                        state->first_plain_us.compare_exchange_strong(first, observed_us);
                        state->last_plain_us.store(observed_us, std::memory_order_relaxed);
                        state->plain_frames.fetch_add(1, std::memory_order_relaxed);
                        if (!options.plain_content) {
                          claimed->busy.store(false, std::memory_order_release);
                          return;
                        }
                      }
                      id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
                      id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
                      if (command_buffer == nil || blit == nil) {
                        claimed->busy.store(false, std::memory_order_release);
                        state->probe_dropped.fetch_add(1, std::memory_order_relaxed);
                        return;
                      }
                      [blit copyFromTexture:texture
                                sourceSlice:0
                                sourceLevel:0
                               sourceOrigin:MTLOriginMake(options.plain_content ?
                                                            (options.expected_width - 16U) / 2U : 0U,
                                                          options.plain_content ?
                                                            (options.expected_height - 16U) / 2U : 0U,
                                                          0)
                                 sourceSize:MTLSizeMake(options.h264_marker ? 128U :
                                                            options.nv12_marker ? kMarkerBytes * 8U
                                                            : options.plain_content ? 16U
                                                                                : kMarkerBytes / 4U,
                                                         options.h264_marker ? 112U :
                                                         options.plain_content ? 16U : 1U, 1)
                                   toBuffer:claimed->buffer
                          destinationOffset:0
                     destinationBytesPerRow:kReadbackBytesPerRow
                   destinationBytesPerImage:kReadbackBytesPerRow];
                      [blit endEncoding];
                      state->in_flight.fetch_add(1, std::memory_order_relaxed);
                      [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                        if (completed.status != MTLCommandBufferStatusCompleted) {
                          state->command_errors.fetch_add(1, std::memory_order_relaxed);
                        } else if (options.plain_content) {
                          const auto* bytes = static_cast<const std::uint8_t*>(claimed->buffer.contents);
                          std::uint64_t hash = 14695981039346656037ULL;
                          for (std::size_t row = 0; row < 16U; ++row) {
                            for (std::size_t column = 0; column < 16U * 4U; ++column) {
                              hash ^= bytes[row * kReadbackBytesPerRow + column];
                              hash *= 1099511628211ULL;
                            }
                          }
                          std::lock_guard lock(state->content_mutex);
                          state->content_hashes.emplace_back(frame_index, hash);
                        } else {
                          Sample sample;
                          if (decode_marker(claimed->buffer.contents, sample,
                                            options.nv12_marker,
                                            options.h264_marker)) {
                            std::lock_guard lock(state->samples_mutex);
                            if (state->samples.size() < state->max_samples) {
                              state->samples.push_back(sample);
                            }
                          } else {
                            state->invalid_markers.fetch_add(1, std::memory_order_relaxed);
                          }
                        }
                        claimed->busy.store(false, std::memory_order_release);
                        state->in_flight.fetch_sub(1, std::memory_order_relaxed);
                      }];
                      [command_buffer commit];
                      // A Syphon texture may be reused before an asynchronous
                      // readback executes. Finish H.264 marker and content
                      // copies while this callback still owns its frame image.
                      if (options.h264_marker || options.plain_content)
                        [command_buffer waitUntilCompleted];
                    }
                  };
    const auto stop_client = [&state](id<SyncSyphonMetalClient> stopping) {
      state->standing_down.store(true);
      while (state->handlers_running.load() != 0) run_loop_for(std::chrono::milliseconds(1));
      @try {
        [stopping stop];
      } @catch (NSException*) {
      }
    };
    id allocated_client = [client_class alloc];
    client = [(id<SyncSyphonMetalClient>)allocated_client
        initWithServerDescription:description
                           device:device
                          options:nil
                  newFrameHandler:frame_handler];
    if (client == nil) {
      std::cerr << "sync_syphon_receiver_probe: failed to create Syphon client\n";
      return 1;
    }

    const auto probe_deadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(options.duration_ms);
    while (std::chrono::steady_clock::now() < probe_deadline) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          probe_deadline - std::chrono::steady_clock::now());
      run_loop_for(std::min(remaining, std::chrono::milliseconds(50)));
      if (client == nil || ([client respondsToSelector:@selector(isValid)] && ![client isValid])) {
        NSArray<NSDictionary<NSString*, id>*>* matches =
            [directory serversMatchingName:server_name appName:nil];
        if (matches.count > 0) {
          stop_client(client);
          client = nil;
          @try {
            client = [(id<SyncSyphonMetalClient>)[client_class alloc]
                initWithServerDescription:matches.firstObject
                                   device:device
                                  options:nil
                          newFrameHandler:frame_handler];
          } @catch (NSException*) {
            client = nil;
          }
          state->standing_down.store(false);
        }
      }
    }
    stop_client(client);
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (state->in_flight.load(std::memory_order_acquire) != 0 &&
           std::chrono::steady_clock::now() < drain_deadline) {
      run_loop_for(std::chrono::milliseconds(1));
    }
    client = nil;

    std::vector<Sample> captured;
    {
      std::lock_guard lock(state->samples_mutex);
      captured = state->samples;
    }
    std::sort(captured.begin(), captured.end(),
              [](const Sample& left, const Sample& right) {
                return left.sequence < right.sequence;
              });
    std::vector<std::uint64_t> duplicate_sequence_ids;
    for (std::size_t index = 1; index < captured.size(); ++index) {
      if (captured[index].sequence == captured[index - 1U].sequence &&
          duplicate_sequence_ids.size() < 64) {
        duplicate_sequence_ids.push_back(captured[index].sequence);
      }
    }
    const auto unique_end = std::unique(
        captured.begin(), captured.end(),
        [](const Sample& left, const Sample& right) {
          return left.sequence == right.sequence;
        });
    const std::size_t duplicate_markers =
        static_cast<std::size_t>(std::distance(unique_end, captured.end()));
    captured.erase(unique_end, captured.end());
    std::uint64_t missed_sequences = 0;
    std::vector<std::uint64_t> gap_after;
    for (std::size_t index = 1; index < captured.size(); ++index) {
      if (captured[index].sequence > captured[index - 1U].sequence + 1U) {
        missed_sequences += captured[index].sequence - captured[index - 1U].sequence - 1U;
        if (gap_after.size() < 64) gap_after.push_back(captured[index - 1U].sequence);
      }
    }
    std::vector<std::int64_t> latencies;
    std::vector<std::int64_t> browser_ages;
    std::vector<std::int64_t> transport_latencies;
    latencies.reserve(captured.size());
    browser_ages.reserve(captured.size());
    transport_latencies.reserve(captured.size());
    std::size_t clock_skewed_markers = 0;
    for (const Sample& sample : captured) {
      if (sample.clock_skewed) {
        ++clock_skewed_markers;
        continue;
      }
      latencies.push_back(sample.latency_us);
      browser_ages.push_back(sample.browser_age_us);
      transport_latencies.push_back(sample.transport_latency_us);
    }
    std::int64_t min_latency = 0;
    std::int64_t max_latency = 0;
    if (!latencies.empty()) {
      const auto bounds = std::minmax_element(latencies.begin(), latencies.end());
      min_latency = *bounds.first;
      max_latency = *bounds.second;
    }
    const auto plain_frames = state->plain_frames.load(std::memory_order_relaxed);
    const auto plain_span_us = state->last_plain_us.load(std::memory_order_relaxed) -
                               state->first_plain_us.load(std::memory_order_relaxed);
    const double plain_fps = plain_frames > 1 && plain_span_us > 0
        ? static_cast<double>(plain_frames - 1) * 1000000.0 /
              static_cast<double>(plain_span_us)
        : 0.0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> content_hashes;
    {
      std::lock_guard lock(state->content_mutex);
      content_hashes = state->content_hashes;
    }
    std::sort(content_hashes.begin(), content_hashes.end());
    std::size_t duplicate_content_frames = 0;
    std::unordered_set<std::uint64_t> distinct_content;
    for (std::size_t index = 0; index < content_hashes.size(); ++index) {
      distinct_content.insert(content_hashes[index].second);
      if (index > 0 && content_hashes[index].second == content_hashes[index - 1U].second)
        ++duplicate_content_frames;
    }

    std::cout << "{\"available\":true,\"serverFound\":true,\"framesSeen\":"
              << state->frames_seen.load() << ",\"markers\":" << captured.size()
              << ",\"plainFrames\":" << plain_frames
              << ",\"plainFps\":" << plain_fps
              << ",\"plainContentFrames\":" << content_hashes.size()
              << ",\"duplicateContentFrames\":" << duplicate_content_frames
              << ",\"contentDistinct\":" << distinct_content.size()
              << ",\"duplicateMarkers\":" << duplicate_markers
              << ",\"probeDropped\":" << state->probe_dropped.load()
              << ",\"invalidMarkers\":" << state->invalid_markers.load()
              << ",\"dimensionMismatches\":"
              << state->dimension_mismatches.load()
              << ",\"commandErrors\":" << state->command_errors.load()
              << ",\"missedSequences\":" << missed_sequences
              << ",\"clockSkewedMarkers\":" << clock_skewed_markers
              << ",\"firstSequence\":" << (captured.empty() ? 0 : captured.front().sequence)
              << ",\"lastSequence\":" << (captured.empty() ? 0 : captured.back().sequence)
              << ",\"gapAfter\":[";
    for (std::size_t index = 0; index < gap_after.size(); ++index) {
      if (index) std::cout << ',';
      std::cout << gap_after[index];
    }
    std::cout << "],\"duplicateSequenceIds\":[";
    for (std::size_t index = 0; index < duplicate_sequence_ids.size(); ++index) {
      if (index) std::cout << ',';
      std::cout << duplicate_sequence_ids[index];
    }
    std::cout << "],\"latencyUs\":{\"min\":" << min_latency
              << ",\"p50\":" << percentile(latencies, 0.50)
              << ",\"p95\":" << percentile(latencies, 0.95)
              << ",\"p99\":" << percentile(latencies, 0.99)
              << ",\"max\":" << max_latency
              << "},\"browserAgeUs\":{\"p50\":"
              << percentile(browser_ages, 0.50)
              << ",\"p95\":" << percentile(browser_ages, 0.95)
              << ",\"p99\":" << percentile(browser_ages, 0.99)
              << "},\"transportLatencyUs\":{\"p50\":"
              << percentile(transport_latencies, 0.50)
              << ",\"p95\":" << percentile(transport_latencies, 0.95)
              << ",\"p99\":" << percentile(transport_latencies, 0.99)
              << "}}\n";
    return ((options.plain_video ? plain_frames == 0 : captured.empty()) ||
                   state->dimension_mismatches.load() != 0 ||
                   state->command_errors.load() != 0
               ) ? 1
               : 0;
  }
}
