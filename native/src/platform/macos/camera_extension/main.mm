// The Sync Camera extension.
//
// One provider, one device, two streams: a source stream every camera
// consumer sees, and a sink stream syncd feeds through the CoreMediaIO
// client API (cmio_camera_sink.mm). Sample buffers arriving on the sink are
// forwarded to the source as they are. When nothing arrives for a while the
// relay policy asks for a black frame so the camera never goes dark on a
// consumer that already opened it.
//
// This process is sandboxed and owned by macOS: it is launched when a
// consumer opens the camera and torn down when the last one leaves. It holds
// no Sync state and needs no pairing; the daemon side already enforced that
// only an approved origin can send.

#import <CoreMedia/CoreMedia.h>
#import <CoreMediaIO/CMIOExtension.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOKit/audio/IOAudioTypes.h>

#include <mach/mach_time.h>

#include <string_view>

#include <sync/platform/camera_identity.hpp>
#include <sync/platform/camera_relay_policy.hpp>

namespace camera = noisefactor::sync::camera;

namespace {

NSString* ns(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data()
                                  length:value.size()
                                encoding:NSUTF8StringEncoding];
}

uint64_t host_time_ns() {
  static const mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    return info;
  }();
  return mach_absolute_time() * timebase.numer / timebase.denom;
}

// Stable identifiers. The device UUID and stream UUIDs are what CoreMediaIO
// shows consumers; the legacy device id is the UID the daemon looks up.
NSUUID* device_uuid() {
  return [[NSUUID alloc] initWithUUIDString:@"6B0C2F6E-3B1A-4C8E-9C7D-5A1F2E3D4C5B"];
}
NSUUID* source_stream_uuid() {
  return [[NSUUID alloc] initWithUUIDString:@"1E7A4C2B-8D3F-4A6E-B5C1-2F9D8E7A6B5C"];
}
NSUUID* sink_stream_uuid() {
  return [[NSUUID alloc] initWithUUIDString:@"9C3B5E7A-1F2D-4B8C-A6E5-3D7F9B1C2E4A"];
}

}  // namespace

@class SyncCameraDeviceSource;

// ---------------------------------------------------------------------------
// Source stream: what consumers see.
// ---------------------------------------------------------------------------
@interface SyncCameraSourceStream : NSObject <CMIOExtensionStreamSource>
@property(nonatomic, strong) CMIOExtensionStream* stream;
@property(nonatomic, weak) SyncCameraDeviceSource* device;
@property(nonatomic, strong) CMIOExtensionStreamFormat* format;
- (instancetype)initWithDevice:(SyncCameraDeviceSource*)device
                   description:(CMVideoFormatDescriptionRef)description;
@end

// ---------------------------------------------------------------------------
// Sink stream: what syncd feeds.
// ---------------------------------------------------------------------------
@interface SyncCameraSinkStream : NSObject <CMIOExtensionStreamSource>
@property(nonatomic, strong) CMIOExtensionStream* stream;
@property(nonatomic, weak) SyncCameraDeviceSource* device;
@property(nonatomic, strong) CMIOExtensionStreamFormat* format;
@property(nonatomic, strong) CMIOExtensionClient* client;
@property(nonatomic, assign) BOOL streaming;
- (instancetype)initWithDevice:(SyncCameraDeviceSource*)device
                   description:(CMVideoFormatDescriptionRef)description;
@end

// ---------------------------------------------------------------------------
// Device: owns both streams, the relay policy, and the idle timer.
// ---------------------------------------------------------------------------
@interface SyncCameraDeviceSource : NSObject <CMIOExtensionDeviceSource>
@property(nonatomic, strong) CMIOExtensionDevice* device;
@property(nonatomic, strong) SyncCameraSourceStream* source;
@property(nonatomic, strong) SyncCameraSinkStream* sink;
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic, strong) dispatch_source_t timer;
- (void)sourceStarted;
- (void)sourceStopped;
- (void)relaySampleBuffer:(CMSampleBufferRef)buffer;
@end

@implementation SyncCameraDeviceSource {
  camera::CameraRelayPolicy _policy;
  CVPixelBufferPoolRef _pool;
  CMVideoFormatDescriptionRef _formatDescription;
}

- (instancetype)init {
  self = [super init];
  if (self == nil) return nil;
  _queue = dispatch_queue_create("io.noisefactor.sync.camera.device", DISPATCH_QUEUE_SERIAL);

  CMVideoFormatDescriptionRef description = nullptr;
  if (CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMPixelFormat_32BGRA,
                                     static_cast<int32_t>(camera::kCanvas.width),
                                     static_cast<int32_t>(camera::kCanvas.height), nullptr,
                                     &description) != noErr ||
      description == nullptr) {
    NSLog(@"sync camera: could not describe the BGRA format");
    return nil;
  }
  _formatDescription = description;

  NSDictionary* attributes = @{
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
    (id)kCVPixelBufferWidthKey : @(camera::kCanvas.width),
    (id)kCVPixelBufferHeightKey : @(camera::kCanvas.height),
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
  };
  CVPixelBufferPoolRef pool = nullptr;
  if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attributes,
                              &pool) != kCVReturnSuccess ||
      pool == nullptr) {
    NSLog(@"sync camera: could not create the pixel buffer pool");
    return nil;
  }
  _pool = pool;

  _device = [CMIOExtensionDevice deviceWithLocalizedName:ns(camera::kDeviceName)
                                                 deviceID:device_uuid()
                                           legacyDeviceID:ns(camera::kDeviceUid)
                                                   source:self];
  _source = [[SyncCameraSourceStream alloc] initWithDevice:self description:_formatDescription];
  _sink = [[SyncCameraSinkStream alloc] initWithDevice:self description:_formatDescription];
  NSError* error = nil;
  if (![_device addStream:_source.stream error:&error] ||
      ![_device addStream:_sink.stream error:&error]) {
    NSLog(@"sync camera: could not add streams: %@", error);
    return nil;
  }
  return self;
}

- (void)dealloc {
  if (_timer != nil) dispatch_source_cancel(_timer);
  if (_pool != nullptr) CVPixelBufferPoolRelease(_pool);
  if (_formatDescription != nullptr) CFRelease(_formatDescription);
}

- (NSSet<CMIOExtensionProperty>*)availableProperties {
  return [NSSet setWithObjects:CMIOExtensionPropertyDeviceTransportType,
                               CMIOExtensionPropertyDeviceModel, nil];
}

- (CMIOExtensionDeviceProperties*)devicePropertiesForProperties:
                                      (NSSet<CMIOExtensionProperty>*)properties
                                                          error:(NSError**)outError {
  (void)outError;
  CMIOExtensionDeviceProperties* result =
      [CMIOExtensionDeviceProperties devicePropertiesWithDictionary:@{}];
  if ([properties containsObject:CMIOExtensionPropertyDeviceTransportType]) {
    result.transportType = @(kIOAudioDeviceTransportTypeVirtual);
  }
  if ([properties containsObject:CMIOExtensionPropertyDeviceModel]) {
    result.model = ns(camera::kDeviceName);
  }
  return result;
}

- (BOOL)setDeviceProperties:(CMIOExtensionDeviceProperties*)deviceProperties
                      error:(NSError**)outError {
  (void)deviceProperties;
  (void)outError;
  return YES;
}

- (void)sourceStarted {
  dispatch_async(_queue, ^{
    self->_policy.source_started();
    [self ensureTimer];
  });
}

- (void)sourceStopped {
  dispatch_async(_queue, ^{
    self->_policy.source_stopped();
  });
}

- (void)ensureTimer {
  if (_timer != nil) return;
  _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _queue);
  const uint64_t interval = _policy.idle_interval_ns();
  dispatch_source_set_timer(_timer, dispatch_time(DISPATCH_TIME_NOW, 0), interval, interval / 4);
  __weak SyncCameraDeviceSource* weakSelf = self;
  dispatch_source_set_event_handler(_timer, ^{
    SyncCameraDeviceSource* self = weakSelf;
    if (self == nil) return;
    if (self->_policy.tick(host_time_ns()) == camera::CameraRelayPolicy::Action::EmitBlack) {
      [self sendBlackFrame];
    }
  });
  dispatch_resume(_timer);
}

- (void)sendBlackFrame {
  CVPixelBufferRef pixels = nullptr;
  if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, _pool, &pixels) !=
          kCVReturnSuccess ||
      pixels == nullptr) {
    return;
  }
  CVPixelBufferLockBaseAddress(pixels, 0);
  uint8_t* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(pixels));
  const size_t stride = CVPixelBufferGetBytesPerRow(pixels);
  for (uint32_t row = 0; row < camera::kCanvas.height; ++row) {
    uint8_t* line = base + static_cast<size_t>(row) * stride;
    for (uint32_t x = 0; x < camera::kCanvas.width; ++x) {
      line[static_cast<size_t>(x) * 4 + 0] = 0;
      line[static_cast<size_t>(x) * 4 + 1] = 0;
      line[static_cast<size_t>(x) * 4 + 2] = 0;
      line[static_cast<size_t>(x) * 4 + 3] = 255;
    }
  }
  CVPixelBufferUnlockBaseAddress(pixels, 0);
  const uint64_t now = host_time_ns();
  CMSampleTimingInfo timing{
      .duration = CMTimeMake(1, 30),
      .presentationTimeStamp = CMTimeMake(static_cast<int64_t>(now), 1'000'000'000),
      .decodeTimeStamp = kCMTimeInvalid,
  };
  CMSampleBufferRef sample = nullptr;
  if (CMSampleBufferCreateForImageBuffer(kCFAllocatorDefault, pixels, true, nullptr, nullptr,
                                         _formatDescription, &timing, &sample) == noErr &&
      sample != nullptr) {
    [_source.stream sendSampleBuffer:sample
                       discontinuity:CMIOExtensionStreamDiscontinuityFlagNone
               hostTimeInNanoseconds:now];
    CFRelease(sample);
  }
  CVPixelBufferRelease(pixels);
}

- (void)relaySampleBuffer:(CMSampleBufferRef)buffer {
  const uint64_t now = host_time_ns();
  dispatch_async(_queue, ^{
    self->_policy.client_frame_arrived(now);
  });
  [_source.stream sendSampleBuffer:buffer
                     discontinuity:CMIOExtensionStreamDiscontinuityFlagNone
             hostTimeInNanoseconds:now];
}

@end

@implementation SyncCameraSourceStream

- (instancetype)initWithDevice:(SyncCameraDeviceSource*)device
                   description:(CMVideoFormatDescriptionRef)description {
  self = [super init];
  if (self == nil) return nil;
  _device = device;
  _format = [CMIOExtensionStreamFormat
      streamFormatWithFormatDescription:description
                 maxFrameDuration:CMTimeMake(1, 1)
                 minFrameDuration:CMTimeMake(1, static_cast<int32_t>(camera::kMaximumFramesPerSecond))
              validFrameDurations:nil];
  _stream = [CMIOExtensionStream streamWithLocalizedName:ns(camera::kDeviceName)
                                                streamID:source_stream_uuid()
                                               direction:CMIOExtensionStreamDirectionSource
                                               clockType:CMIOExtensionStreamClockTypeHostTime
                                                  source:self];
  return self;
}

- (NSArray<CMIOExtensionStreamFormat*>*)formats {
  return @[ _format ];
}

- (NSSet<CMIOExtensionProperty>*)availableProperties {
  return [NSSet setWithObjects:CMIOExtensionPropertyStreamActiveFormatIndex,
                               CMIOExtensionPropertyStreamFrameDuration, nil];
}

- (CMIOExtensionStreamProperties*)streamPropertiesForProperties:
                                      (NSSet<CMIOExtensionProperty>*)properties
                                                          error:(NSError**)outError {
  (void)outError;
  CMIOExtensionStreamProperties* result =
      [CMIOExtensionStreamProperties streamPropertiesWithDictionary:@{}];
  if ([properties containsObject:CMIOExtensionPropertyStreamActiveFormatIndex]) {
    result.activeFormatIndex = @(0);
  }
  if ([properties containsObject:CMIOExtensionPropertyStreamFrameDuration]) {
    result.frameDuration =
        (__bridge_transfer NSDictionary*)CMTimeCopyAsDictionary(CMTimeMake(1, 30), kCFAllocatorDefault);
  }
  return result;
}

- (BOOL)setStreamProperties:(CMIOExtensionStreamProperties*)streamProperties
                      error:(NSError**)outError {
  (void)streamProperties;
  (void)outError;
  return YES;
}

- (BOOL)authorizedToStartStreamForClient:(CMIOExtensionClient*)client {
  (void)client;
  return YES;
}

- (BOOL)startStreamAndReturnError:(NSError**)outError {
  (void)outError;
  [_device sourceStarted];
  return YES;
}

- (BOOL)stopStreamAndReturnError:(NSError**)outError {
  (void)outError;
  [_device sourceStopped];
  return YES;
}

@end

@implementation SyncCameraSinkStream

- (instancetype)initWithDevice:(SyncCameraDeviceSource*)device
                   description:(CMVideoFormatDescriptionRef)description {
  self = [super init];
  if (self == nil) return nil;
  _device = device;
  _format = [CMIOExtensionStreamFormat
      streamFormatWithFormatDescription:description
                 maxFrameDuration:CMTimeMake(1, 1)
                 minFrameDuration:CMTimeMake(1, static_cast<int32_t>(camera::kMaximumFramesPerSecond))
              validFrameDurations:nil];
  _stream = [CMIOExtensionStream streamWithLocalizedName:@"Sync Camera Sink"
                                                streamID:sink_stream_uuid()
                                               direction:CMIOExtensionStreamDirectionSink
                                               clockType:CMIOExtensionStreamClockTypeHostTime
                                                  source:self];
  return self;
}

- (NSArray<CMIOExtensionStreamFormat*>*)formats {
  return @[ _format ];
}

- (NSSet<CMIOExtensionProperty>*)availableProperties {
  return [NSSet setWithObjects:CMIOExtensionPropertyStreamActiveFormatIndex,
                               CMIOExtensionPropertyStreamFrameDuration,
                               CMIOExtensionPropertyStreamSinkBufferQueueSize,
                               CMIOExtensionPropertyStreamSinkBuffersRequiredForStartup, nil];
}

- (CMIOExtensionStreamProperties*)streamPropertiesForProperties:
                                      (NSSet<CMIOExtensionProperty>*)properties
                                                          error:(NSError**)outError {
  (void)outError;
  CMIOExtensionStreamProperties* result =
      [CMIOExtensionStreamProperties streamPropertiesWithDictionary:@{}];
  if ([properties containsObject:CMIOExtensionPropertyStreamActiveFormatIndex]) {
    result.activeFormatIndex = @(0);
  }
  if ([properties containsObject:CMIOExtensionPropertyStreamFrameDuration]) {
    result.frameDuration =
        (__bridge_transfer NSDictionary*)CMTimeCopyAsDictionary(CMTimeMake(1, 30), kCFAllocatorDefault);
  }
  if ([properties containsObject:CMIOExtensionPropertyStreamSinkBufferQueueSize]) {
    result.sinkBufferQueueSize = @(4);
  }
  if ([properties containsObject:CMIOExtensionPropertyStreamSinkBuffersRequiredForStartup]) {
    result.sinkBuffersRequiredForStartup = @(1);
  }
  return result;
}

- (BOOL)setStreamProperties:(CMIOExtensionStreamProperties*)streamProperties
                      error:(NSError**)outError {
  (void)streamProperties;
  (void)outError;
  return YES;
}

- (BOOL)authorizedToStartStreamForClient:(CMIOExtensionClient*)client {
  _client = client;
  return YES;
}

- (BOOL)startStreamAndReturnError:(NSError**)outError {
  (void)outError;
  _streaming = YES;
  [self consume];
  return YES;
}

- (BOOL)stopStreamAndReturnError:(NSError**)outError {
  (void)outError;
  _streaming = NO;
  return YES;
}

- (void)consume {
  if (!_streaming || _client == nil) return;
  __weak SyncCameraSinkStream* weakSelf = self;
  [_stream consumeSampleBufferFromClient:_client
                       completionHandler:^(CMSampleBufferRef sampleBuffer, uint64_t sequence,
                                           CMIOExtensionStreamDiscontinuityFlags discontinuity,
                                           BOOL hasMore, NSError* error) {
                         (void)discontinuity;
                         SyncCameraSinkStream* self = weakSelf;
                         if (self == nil) return;
                         if (sampleBuffer != nullptr) {
                           [self.device relaySampleBuffer:sampleBuffer];
                           CMIOExtensionScheduledOutput* output = [CMIOExtensionScheduledOutput
                               scheduledOutputWithSequenceNumber:sequence
                                           hostTimeInNanoseconds:host_time_ns()];
                           [self.stream notifyScheduledOutputChanged:output];
                         }
                         if (error != nil) {
                           NSLog(@"sync camera: sink consume error: %@", error);
                         }
                         if (hasMore) {
                           [self consume];
                         } else if (self.streaming) {
                           // Nothing queued yet: poll again shortly rather
                           // than spin. Four milliseconds is well under one
                           // frame at 60 fps.
                           dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_MSEC),
                                          dispatch_get_main_queue(), ^{
                                            [self consume];
                                          });
                         }
                       }];
}

@end

// ---------------------------------------------------------------------------
// Provider
// ---------------------------------------------------------------------------
@interface SyncCameraProviderSource : NSObject <CMIOExtensionProviderSource>
@property(nonatomic, strong) CMIOExtensionProvider* provider;
@property(nonatomic, strong) SyncCameraDeviceSource* deviceSource;
@end

@implementation SyncCameraProviderSource

- (instancetype)init {
  self = [super init];
  if (self == nil) return nil;
  _provider = [CMIOExtensionProvider providerWithSource:self clientQueue:nil];
  _deviceSource = [[SyncCameraDeviceSource alloc] init];
  NSError* error = nil;
  if (_deviceSource == nil || ![_provider addDevice:_deviceSource.device error:&error]) {
    NSLog(@"sync camera: could not add device: %@", error);
    return nil;
  }
  return self;
}

- (BOOL)connectClient:(CMIOExtensionClient*)client error:(NSError**)outError {
  (void)client;
  (void)outError;
  return YES;
}

- (void)disconnectClient:(CMIOExtensionClient*)client {
  (void)client;
}

- (NSSet<CMIOExtensionProperty>*)availableProperties {
  return [NSSet setWithObjects:CMIOExtensionPropertyProviderManufacturer, nil];
}

- (CMIOExtensionProviderProperties*)providerPropertiesForProperties:
                                        (NSSet<CMIOExtensionProperty>*)properties
                                                              error:(NSError**)outError {
  (void)outError;
  CMIOExtensionProviderProperties* result =
      [CMIOExtensionProviderProperties providerPropertiesWithDictionary:@{}];
  if ([properties containsObject:CMIOExtensionPropertyProviderManufacturer]) {
    result.manufacturer = @"Noise Factor";
  }
  return result;
}

- (BOOL)setProviderProperties:(CMIOExtensionProviderProperties*)providerProperties
                        error:(NSError**)outError {
  (void)providerProperties;
  (void)outError;
  return YES;
}

@end

int main(int argc, const char* argv[]) {
  (void)argc;
  (void)argv;
  @autoreleasepool {
    SyncCameraProviderSource* source = [[SyncCameraProviderSource alloc] init];
    if (source == nil) return 1;
    [CMIOExtensionProvider startServiceWithProvider:source.provider];
    CFRunLoopRun();
  }
  return 0;
}
