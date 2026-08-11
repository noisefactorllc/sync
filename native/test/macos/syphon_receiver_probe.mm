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
@end

namespace {

constexpr std::size_t kMarkerBytes = 28;
constexpr std::size_t kReadbackBytesPerRow = 256;
constexpr std::size_t kMaximumSamples = 60U * 60U * 10U;
constexpr std::size_t kReadbackSlots = 4;
constexpr std::array<std::uint8_t, 4> kMarkerSignature{{'S', 'Y', 'N', 'C'}};

struct Options {
  std::string framework_path;
  std::string server_name;
  std::uint32_t duration_ms = 0;
  std::uint32_t discovery_timeout_ms = 0;
  std::uint32_t expected_width = 0;
  std::uint32_t expected_height = 0;
};

struct Sample {
  std::uint64_t sequence = 0;
  std::uint64_t presentation_time_us = 0;
  std::uint64_t browser_send_time_us = 0;
  std::int64_t latency_us = 0;
  std::int64_t browser_age_us = 0;
  std::int64_t transport_latency_us = 0;
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
  std::mutex samples_mutex;
  std::vector<Sample> samples;
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
  if (argc != 7) return false;
  options.framework_path = argv[1];
  options.server_name = argv[2];
  return !options.framework_path.empty() && !options.server_name.empty() &&
         parse_u32(argv[3], options.duration_ms) &&
         parse_u32(argv[4], options.discovery_timeout_ms) &&
         parse_u32(argv[5], options.expected_width) &&
         parse_u32(argv[6], options.expected_height);
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

auto decode_marker(const void* raw_bytes, Sample& sample) noexcept -> bool {
  if (raw_bytes == nullptr) return false;
  const auto* bgra = static_cast<const std::uint8_t*>(raw_bytes);
  std::array<std::uint8_t, kMarkerBytes> rgba{};
  for (std::size_t pixel = 0; pixel < kMarkerBytes / 4U; ++pixel) {
    rgba[pixel * 4U] = bgra[pixel * 4U + 2U];
    rgba[pixel * 4U + 1U] = bgra[pixel * 4U + 1U];
    rgba[pixel * 4U + 2U] = bgra[pixel * 4U];
    rgba[pixel * 4U + 3U] = bgra[pixel * 4U + 3U];
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
  return sample.latency_us >= 0 && sample.browser_age_us >= 0 &&
         sample.transport_latency_us >= 0;
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
                   "<duration-ms> <discovery-timeout-ms> <width> <height>\n";
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
      slot.buffer = [device newBufferWithLength:kReadbackBytesPerRow
                                        options:MTLResourceStorageModeShared];
      if (slot.buffer == nil) {
        std::cerr << "sync_syphon_receiver_probe: failed to allocate readback slots\n";
        return 1;
      }
    }

    state->samples.reserve(std::min<std::size_t>(
        kMaximumSamples, static_cast<std::size_t>(options.duration_ms) / 8U + 32U));

    __block id<SyncSyphonMetalClient> client = nil;
    id allocated_client = [client_class alloc];
    client = [(id<SyncSyphonMetalClient>)allocated_client
        initWithServerDescription:description
                           device:device
                          options:nil
                  newFrameHandler:^(id<SyncSyphonMetalClient> callback_client) {
                    state->frames_seen.fetch_add(1, std::memory_order_relaxed);
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
                               sourceOrigin:MTLOriginMake(0, 0, 0)
                                 sourceSize:MTLSizeMake(kMarkerBytes / 4U, 1, 1)
                                   toBuffer:claimed->buffer
                          destinationOffset:0
                     destinationBytesPerRow:kReadbackBytesPerRow
                   destinationBytesPerImage:kReadbackBytesPerRow];
                      [blit endEncoding];
                      state->in_flight.fetch_add(1, std::memory_order_relaxed);
                      [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
                        if (completed.status != MTLCommandBufferStatusCompleted) {
                          state->command_errors.fetch_add(1, std::memory_order_relaxed);
                        } else {
                          Sample sample;
                          if (decode_marker(claimed->buffer.contents, sample)) {
                            std::lock_guard lock(state->samples_mutex);
                            if (state->samples.size() < kMaximumSamples) {
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
                    }
                  }];
    if (client == nil) {
      std::cerr << "sync_syphon_receiver_probe: failed to create Syphon client\n";
      return 1;
    }

    run_loop_for(std::chrono::milliseconds(options.duration_ms));
    [client stop];
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
    const auto unique_end = std::unique(
        captured.begin(), captured.end(),
        [](const Sample& left, const Sample& right) {
          return left.sequence == right.sequence;
        });
    const std::size_t duplicate_markers =
        static_cast<std::size_t>(std::distance(unique_end, captured.end()));
    captured.erase(unique_end, captured.end());
    std::uint64_t missed_sequences = 0;
    for (std::size_t index = 1; index < captured.size(); ++index) {
      if (captured[index].sequence > captured[index - 1U].sequence + 1U) {
        missed_sequences += captured[index].sequence - captured[index - 1U].sequence - 1U;
      }
    }
    std::vector<std::int64_t> latencies;
    std::vector<std::int64_t> browser_ages;
    std::vector<std::int64_t> transport_latencies;
    latencies.reserve(captured.size());
    browser_ages.reserve(captured.size());
    transport_latencies.reserve(captured.size());
    for (const Sample& sample : captured) {
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

    std::cout << "{\"available\":true,\"serverFound\":true,\"framesSeen\":"
              << state->frames_seen.load() << ",\"markers\":" << captured.size()
              << ",\"duplicateMarkers\":" << duplicate_markers
              << ",\"probeDropped\":" << state->probe_dropped.load()
              << ",\"invalidMarkers\":" << state->invalid_markers.load()
              << ",\"dimensionMismatches\":"
              << state->dimension_mismatches.load()
              << ",\"commandErrors\":" << state->command_errors.load()
              << ",\"missedSequences\":" << missed_sequences
              << ",\"firstSequence\":" << (captured.empty() ? 0 : captured.front().sequence)
              << ",\"lastSequence\":" << (captured.empty() ? 0 : captured.back().sequence)
              << ",\"latencyUs\":{\"min\":" << min_latency
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
    return captured.empty() || state->dimension_mismatches.load() != 0 ||
                   state->command_errors.load() != 0
               ? 1
               : 0;
  }
}
