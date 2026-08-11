#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "../test_harness.hpp"

#include <sync/platform/metal_frame_publisher.hpp>
#include <sync/platform/syphon_consumer.hpp>
#include <sync/protocol.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

@interface SyncSyphonFakeObservation : NSObject
@property(nonatomic) NSUInteger instanceNumber;
@property(nonatomic, copy) NSString* name;
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) NSDictionary<NSString*, id>* options;
@property(nonatomic) NSUInteger stopCount;
@property(nonatomic) NSUInteger publishCount;
@property(nonatomic, strong) id<MTLTexture> texture;
@property(nonatomic, strong) id<MTLCommandBuffer> commandBuffer;
@property(nonatomic) NSRect imageRegion;
@property(nonatomic) BOOL flipped;
@property(nonatomic) MTLCommandBufferStatus statusAtPublish;
@end

@implementation SyncSyphonFakeObservation
@end

namespace {

NSMutableArray<SyncSyphonFakeObservation*>* fake_observations() {
  static NSMutableArray<SyncSyphonFakeObservation*>* observations = nil;
  if (observations == nil) {
    observations = [[NSMutableArray alloc] init];
  }
  return observations;
}

NSMutableArray<NSNumber*>* fake_stop_order() {
  static NSMutableArray<NSNumber*>* order = nil;
  if (order == nil) {
    order = [[NSMutableArray alloc] init];
  }
  return order;
}

bool fake_reject_next_init = false;
bool fake_throw_next_init = false;
bool fake_throw_next_publish = false;
bool fake_throw_next_stop = false;
id<MTLSharedEvent> fake_signal_event = nil;
std::uint64_t fake_signal_value = 0;
NSUInteger fake_next_instance_number = 1;

void reset_fake() {
  [fake_observations() removeAllObjects];
  [fake_stop_order() removeAllObjects];
  fake_reject_next_init = false;
  fake_throw_next_init = false;
  fake_throw_next_publish = false;
  fake_throw_next_stop = false;
  fake_signal_event = nil;
  fake_signal_value = 0;
  fake_next_instance_number = 1;
}

SyncSyphonFakeObservation* observation_named(NSString* name) {
  for (SyncSyphonFakeObservation* observation in fake_observations()) {
    if ([observation.name isEqualToString:name]) {
      return observation;
    }
  }
  return nil;
}

}  // namespace

@interface SyphonMetalServer : NSObject {
 @private
  SyncSyphonFakeObservation* _observation;
}
- (id)initWithName:(NSString*)name
            device:(id<MTLDevice>)device
           options:(NSDictionary<NSString*, id>*)options;
- (void)publishFrameTexture:(id<MTLTexture>)texture
            onCommandBuffer:(id<MTLCommandBuffer>)commandBuffer
                imageRegion:(NSRect)region
                    flipped:(BOOL)flipped;
- (void)stop;
@end

@implementation SyphonMetalServer

- (id)initWithName:(NSString*)name
            device:(id<MTLDevice>)device
           options:(NSDictionary<NSString*, id>*)options {
  (void)options;
  self = [super init];
  if (self == nil) {
    return nil;
  }
  if (fake_reject_next_init) {
    fake_reject_next_init = false;
    return nil;
  }
  if (fake_throw_next_init) {
    fake_throw_next_init = false;
    @throw [NSException exceptionWithName:@"SyncSyphonFakeInitException"
                                   reason:@"deterministic test exception"
                                 userInfo:nil];
  }
  _observation = [[SyncSyphonFakeObservation alloc] init];
  _observation.instanceNumber = fake_next_instance_number++;
  _observation.name = name;
  _observation.device = device;
  _observation.options = options;
  [fake_observations() addObject:_observation];
  return self;
}

- (void)publishFrameTexture:(id<MTLTexture>)texture
            onCommandBuffer:(id<MTLCommandBuffer>)commandBuffer
                imageRegion:(NSRect)region
                    flipped:(BOOL)flipped {
  _observation.publishCount += 1;
  _observation.texture = texture;
  _observation.commandBuffer = commandBuffer;
  _observation.imageRegion = region;
  _observation.flipped = flipped;
  _observation.statusAtPublish = commandBuffer.status;
  if (fake_throw_next_publish) {
    fake_throw_next_publish = false;
    @throw [NSException exceptionWithName:@"SyncSyphonFakePublishException"
                                   reason:@"deterministic test exception"
                                 userInfo:nil];
  }
  if (fake_signal_event != nil) {
    [commandBuffer encodeSignalEvent:fake_signal_event value:fake_signal_value];
  }
}

- (void)stop {
  _observation.stopCount += 1;
  [fake_stop_order() addObject:@(_observation.instanceNumber)];
  if (fake_throw_next_stop) {
    fake_throw_next_stop = false;
    @throw [NSException exceptionWithName:@"SyncSyphonFakeStopException"
                                   reason:@"deterministic test exception"
                                 userInfo:nil];
  }
}

@end

@interface SyncSyphonTextureProxy : NSObject
@property(nonatomic, strong) id<MTLDevice> exposedDevice;
@property(nonatomic) NSUInteger exposedWidth;
@property(nonatomic) NSUInteger exposedHeight;
@end

@implementation SyncSyphonTextureProxy
- (id<MTLDevice>)device {
  return self.exposedDevice;
}
- (NSUInteger)width {
  return self.exposedWidth;
}
- (NSUInteger)height {
  return self.exposedHeight;
}
@end

@interface SyncSyphonCommandBufferProxy : NSObject
@property(nonatomic, strong) id<MTLDevice> exposedDevice;
@end

@implementation SyncSyphonCommandBufferProxy
- (id<MTLDevice>)device {
  return self.exposedDevice;
}
@end

namespace noisefactor::sync {
namespace {

using namespace std::chrono_literals;

auto wait_until(const auto& predicate, std::chrono::milliseconds timeout = 3000ms) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

auto test_device() -> id<MTLDevice> {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (device != nil) {
    return device;
  }
  return MTLCopyAllDevices().firstObject;
}

auto make_texture(id<MTLDevice> device, NSUInteger width, NSUInteger height) -> id<MTLTexture> {
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.usage = MTLTextureUsageShaderRead;
  return [device newTextureWithDescriptor:descriptor];
}

auto metadata(std::uint32_t width = 2, std::uint32_t height = 2) -> MetalFrameMetadata {
  return {
      .width = width,
      .height = height,
      .color_space = MetalColorSpace::DisplayP3,
      .alpha_mode = MetalAlphaMode::Premultiplied,
      .sequence = 19,
      .presentation_time_us = 19000,
      .top_down = true,
  };
}

struct OwnedFrame {
  std::vector<std::byte> payload;
  protocol::FrameView view;
};

auto make_frame() -> OwnedFrame {
  OwnedFrame frame;
  frame.payload = {
      std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
      std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8},
      std::byte{9}, std::byte{10}, std::byte{11}, std::byte{12},
      std::byte{13}, std::byte{14}, std::byte{15}, std::byte{16},
  };
  frame.view = {
      .version = 1,
      .header_bytes = 64,
      .flags = 1,
      .pixel_format = 1,
      .color_space = 2,
      .alpha_mode = 3,
      .width = 2,
      .height = 2,
      .row_stride = 8,
      .payload_bytes = 16,
      .sequence = 7,
      .presentation_time_us = 7000,
      .top_down = true,
      .payload = frame.payload,
  };
  return frame;
}

SYNC_TEST(syphon_discovers_an_already_loaded_class_with_the_exact_documented_selectors) {
  reset_fake();
  Class server_class = NSClassFromString(@"SyphonMetalServer");
  SYNC_REQUIRE(server_class != Nil);
  SYNC_REQUIRE([server_class instancesRespondToSelector:@selector(initWithName:device:options:)]);
  SYNC_REQUIRE([server_class
      instancesRespondToSelector:
          @selector(publishFrameTexture:onCommandBuffer:imageRegion:flipped:)]);
  SYNC_REQUIRE([server_class instancesRespondToSelector:@selector(stop)]);

  SyphonMetalConsumer consumer;
  SYNC_REQUIRE(consumer.available());
}

SYNC_TEST(syphon_open_preserves_unicode_and_enforces_transactional_fixed_bounds) {
  reset_fake();
  id<MTLDevice> device = test_device();
  SYNC_REQUIRE(device != nil);
  SyphonMetalConsumer consumer;

  const std::string unicode_name = "同步 🌈";
  SYNC_REQUIRE(consumer.open_sender("unicode", unicode_name, device));
  SYNC_REQUIRE(fake_observations().count == 1);
  SyncSyphonFakeObservation* unicode = fake_observations().firstObject;
  SYNC_REQUIRE([unicode.name isEqualToString:@"同步 🌈"]);
  SYNC_REQUIRE(unicode.device == device);
  SYNC_REQUIRE(unicode.options == nil);
  SYNC_REQUIRE(!consumer.open_sender("unicode", "Duplicate", device));
  consumer.close_sender("unicode");

  fake_reject_next_init = true;
  const NSUInteger observation_count_before_rejection = fake_observations().count;
  SYNC_REQUIRE(!consumer.open_sender("nil-init", "Rejected", device));
  SYNC_REQUIRE(fake_observations().count == observation_count_before_rejection);
  SYNC_REQUIRE(consumer.open_sender("nil-init", "Accepted", device));
  fake_throw_next_init = true;
  SYNC_REQUIRE(!consumer.open_sender("throw-init", "Rejected", device));
  SYNC_REQUIRE(consumer.open_sender("throw-init", "Accepted", device));

  SYNC_REQUIRE(!consumer.open_sender("", "Name", device));
  SYNC_REQUIRE(!consumer.open_sender(std::string(129, 'i'), "Name", device));
  SYNC_REQUIRE(!consumer.open_sender("empty-name", "", device));
  SYNC_REQUIRE(!consumer.open_sender("long-name", std::string(65, 'n'), device));
  const std::string invalid_utf8{"\xC3\x28", 2};
  SYNC_REQUIRE(!consumer.open_sender("bad-utf8", invalid_utf8, device));
  SYNC_REQUIRE(!consumer.open_sender("nil-device", "Name", nil));

  consumer.close_sender("nil-init");
  consumer.close_sender("throw-init");
  for (int index = 0; index < 8; ++index) {
    SYNC_REQUIRE(consumer.open_sender("sender-" + std::to_string(index), "Name", device));
  }
  SYNC_REQUIRE(!consumer.open_sender("overflow", "Name", device));
  consumer.close_sender("sender-3");
  SYNC_REQUIRE(consumer.open_sender("replacement", "Name", device));
}

SYNC_TEST(syphon_senders_have_distinct_servers_and_stop_exactly_once) {
  reset_fake();
  id<MTLDevice> device = test_device();
  SYNC_REQUIRE(device != nil);
  {
    SyphonMetalConsumer consumer;
    SYNC_REQUIRE(consumer.open_sender("first", "First", device));
    SYNC_REQUIRE(consumer.open_sender("second", "Second", device));
    SyncSyphonFakeObservation* first = observation_named(@"First");
    SyncSyphonFakeObservation* second = observation_named(@"Second");
    SYNC_REQUIRE(first != nil);
    SYNC_REQUIRE(second != nil);
    SYNC_REQUIRE(first.instanceNumber != second.instanceNumber);
    consumer.close_sender("first");
    consumer.close_sender("first");
    SYNC_REQUIRE(first.stopCount == 1);
    SYNC_REQUIRE(second.stopCount == 0);
  }
  SYNC_REQUIRE(observation_named(@"First").stopCount == 1);
  SYNC_REQUIRE(observation_named(@"Second").stopCount == 1);

  reset_fake();
  {
    SyphonMetalConsumer consumer;
    SYNC_REQUIRE(consumer.open_sender("first", "First", device));
    SYNC_REQUIRE(consumer.open_sender("second", "Second", device));
  }
  SYNC_REQUIRE(fake_stop_order().count == 2);
  SYNC_REQUIRE(fake_stop_order()[0].unsignedIntegerValue == 2);
  SYNC_REQUIRE(fake_stop_order()[1].unsignedIntegerValue == 1);

  reset_fake();
  {
    SyphonMetalConsumer consumer;
    SYNC_REQUIRE(consumer.open_sender("throw-stop", "Throw Stop", device));
    SyncSyphonFakeObservation* observation = observation_named(@"Throw Stop");
    fake_throw_next_stop = true;
    consumer.close_sender("throw-stop");
    consumer.close_sender("throw-stop");
    SYNC_REQUIRE(observation.stopCount == 1);
  }
  SYNC_REQUIRE(observation_named(@"Throw Stop").stopCount == 1);
}

SYNC_TEST(syphon_encode_forwards_exact_objects_full_region_and_no_flip_without_committing) {
  reset_fake();
  id<MTLDevice> device = test_device();
  SYNC_REQUIRE(device != nil);
  id<MTLTexture> texture = make_texture(device, 2, 2);
  id<MTLCommandBuffer> command_buffer = [[device newCommandQueue] commandBuffer];
  SYNC_REQUIRE(texture != nil);
  SYNC_REQUIRE(command_buffer != nil);

  SyphonMetalConsumer consumer;
  SYNC_REQUIRE(consumer.open_sender("sender", "Exact", device));
  SYNC_REQUIRE(consumer.encode_frame("sender", texture, command_buffer, metadata()));
  SyncSyphonFakeObservation* observation = observation_named(@"Exact");
  SYNC_REQUIRE(observation.publishCount == 1);
  SYNC_REQUIRE(observation.texture == texture);
  SYNC_REQUIRE(observation.commandBuffer == command_buffer);
  SYNC_REQUIRE(NSEqualRects(observation.imageRegion, NSMakeRect(0, 0, 2, 2)));
  SYNC_REQUIRE(observation.flipped == NO);
  SYNC_REQUIRE(observation.statusAtPublish == MTLCommandBufferStatusNotEnqueued);
  SYNC_REQUIRE(command_buffer.status == MTLCommandBufferStatusNotEnqueued);
}

SYNC_TEST(syphon_encodes_on_the_callers_buffer_and_never_commits_it) {
  reset_fake();
  id<MTLDevice> device = test_device();
  SYNC_REQUIRE(device != nil);
  id<MTLTexture> texture = make_texture(device, 2, 2);
  id<MTLCommandBuffer> command_buffer = [[device newCommandQueue] commandBuffer];
  fake_signal_event = [device newSharedEvent];
  fake_signal_value = 17;
  SYNC_REQUIRE(texture != nil);
  SYNC_REQUIRE(command_buffer != nil);
  SYNC_REQUIRE(fake_signal_event != nil);

  SyphonMetalConsumer consumer;
  SYNC_REQUIRE(consumer.open_sender("event", "Event", device));
  SYNC_REQUIRE(consumer.encode_frame("event", texture, command_buffer, metadata()));
  SYNC_REQUIRE(fake_signal_event.signaledValue == 0);
  SYNC_REQUIRE(command_buffer.status == MTLCommandBufferStatusNotEnqueued);
  [command_buffer commit];
  SYNC_REQUIRE(wait_until([&] { return fake_signal_event.signaledValue == 17; }));
  SYNC_REQUIRE(wait_until(
      [&] { return command_buffer.status == MTLCommandBufferStatusCompleted; }));
}

SYNC_TEST(syphon_rejects_invalid_frames_and_contains_publish_exceptions) {
  reset_fake();
  id<MTLDevice> device = test_device();
  SYNC_REQUIRE(device != nil);
  id<MTLTexture> texture = make_texture(device, 2, 2);
  id<MTLCommandQueue> queue = [device newCommandQueue];
  id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
  SYNC_REQUIRE(texture != nil);
  SYNC_REQUIRE(command_buffer != nil);

  SyphonMetalConsumer consumer;
  SYNC_REQUIRE(consumer.open_sender("sender", "Validation", device));
  SYNC_REQUIRE(!consumer.encode_frame("missing", texture, command_buffer, metadata()));
  SYNC_REQUIRE(!consumer.encode_frame("sender", nil, command_buffer, metadata()));
  SYNC_REQUIRE(!consumer.encode_frame("sender", texture, nil, metadata()));

  auto invalid = metadata();
  invalid.top_down = false;
  SYNC_REQUIRE(!consumer.encode_frame("sender", texture, command_buffer, invalid));
  invalid = metadata(0, 2);
  SYNC_REQUIRE(!consumer.encode_frame("sender", texture, command_buffer, invalid));
  invalid = metadata(3, 2);
  SYNC_REQUIRE(!consumer.encode_frame("sender", texture, command_buffer, invalid));

  id<MTLCommandBuffer> committed_buffer = [queue commandBuffer];
  [committed_buffer commit];
  SYNC_REQUIRE(!consumer.encode_frame("sender", texture, committed_buffer, metadata()));

  id<MTLDevice> wrong_device = (id<MTLDevice>)[[NSObject alloc] init];
  SyncSyphonTextureProxy* wrong_texture = [[SyncSyphonTextureProxy alloc] init];
  wrong_texture.exposedDevice = wrong_device;
  wrong_texture.exposedWidth = 2;
  wrong_texture.exposedHeight = 2;
  SYNC_REQUIRE(!consumer.encode_frame("sender", (id<MTLTexture>)wrong_texture,
                                      command_buffer, metadata()));
  SyncSyphonCommandBufferProxy* wrong_command = [[SyncSyphonCommandBufferProxy alloc] init];
  wrong_command.exposedDevice = wrong_device;
  SYNC_REQUIRE(!consumer.encode_frame("sender", texture,
                                      (id<MTLCommandBuffer>)wrong_command, metadata()));

  fake_throw_next_publish = true;
  id<MTLCommandBuffer> throwing_buffer = [queue commandBuffer];
  SYNC_REQUIRE(!consumer.encode_frame("sender", texture, throwing_buffer, metadata()));
  SYNC_REQUIRE(throwing_buffer.status == MTLCommandBufferStatusNotEnqueued);
  id<MTLCommandBuffer> recovered_buffer = [queue commandBuffer];
  SYNC_REQUIRE(consumer.encode_frame("sender", texture, recovered_buffer, metadata()));
  SYNC_REQUIRE(recovered_buffer.status == MTLCommandBufferStatusNotEnqueued);
  SYNC_REQUIRE(observation_named(@"Validation").publishCount == 2);
}

SYNC_TEST(syphon_consumer_integrates_with_the_real_metal_publisher_commit_boundary) {
  reset_fake();
  SyphonMetalConsumer syphon;
  std::array<MetalFrameConsumer*, 1> consumers{&syphon};
  MetalFramePublisher publisher(consumers);
  SYNC_REQUIRE(publisher.available());
  SYNC_REQUIRE(publisher.open_sender("integrated", "Integrated"));
  auto frame = make_frame();
  SYNC_REQUIRE(publisher.publish("integrated", frame.view) == PublishResult::Accepted);

  SyncSyphonFakeObservation* observation = observation_named(@"Integrated");
  SYNC_REQUIRE(observation != nil);
  SYNC_REQUIRE(observation.publishCount == 1);
  SYNC_REQUIRE(observation.texture.device == observation.device);
  SYNC_REQUIRE(observation.commandBuffer.device == observation.device);
  SYNC_REQUIRE(observation.statusAtPublish == MTLCommandBufferStatusNotEnqueued);
  SYNC_REQUIRE(observation.flipped == NO);
  SYNC_REQUIRE(wait_until(
      [&] { return observation.commandBuffer.status == MTLCommandBufferStatusCompleted; }));
}

}  // namespace
}  // namespace noisefactor::sync
