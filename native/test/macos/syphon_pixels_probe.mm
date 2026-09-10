#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <vector>

@protocol PixelDirectory <NSObject>
+ (id<PixelDirectory>)sharedDirectory;
- (NSArray<NSDictionary*>*)serversMatchingName:(NSString*)name appName:(NSString*)appName;
@end
@protocol PixelClient <NSObject>
- (id)initWithServerDescription:(NSDictionary*)description device:(id<MTLDevice>)device
                       options:(NSDictionary*)options newFrameHandler:(void (^)(id))handler;
- (id<MTLTexture>)newFrameImage;
- (void)stop;
@end

// Independent receiver for the literal 3 x 2 pattern in interoperability-browser.mjs.
// It loads only Syphon and Metal. It does not link to Sync's publisher or protocol.
int main(int argc, char** argv) {
  @autoreleasepool {
    if (argc != 3) {
      std::cerr << "usage: sync_syphon_pixels_probe <Syphon.framework> <server-name>\n";
      return 2;
    }
    NSError* error = nil;
    if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error]) return 2;
    Class directory_class = NSClassFromString(@"SyphonServerDirectory");
    Class client_class = NSClassFromString(@"SyphonMetalClient");
    if (directory_class == Nil || client_class == Nil) return 2;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (queue == nil) return 2;
    id<PixelDirectory> directory = [(Class<PixelDirectory>)directory_class sharedDirectory];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    NSDictionary* description = nil;
    while (description == nil && std::chrono::steady_clock::now() < deadline) {
      description = [directory serversMatchingName:@(argv[2]) appName:nil].firstObject;
      [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
    if (description == nil) { std::cerr << "The Syphon sender was not found.\n"; return 1; }
    id<PixelClient> client = [(id<PixelClient>)[client_class alloc]
      initWithServerDescription:description device:device options:nil newFrameHandler:nil];
    if (client == nil) return 2;
    id<MTLTexture> texture = nil;
    while (texture == nil && std::chrono::steady_clock::now() < deadline) {
      [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
      texture = [client newFrameImage];
    }
    if (texture == nil || texture.width != 3 || texture.height != 2 ||
        texture.pixelFormat != MTLPixelFormatBGRA8Unorm) {
      [client stop]; std::cerr << "The received texture has an invalid descriptor.\n"; return 1;
    }
    id<MTLBuffer> buffer = [device newBufferWithLength:512 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
      sourceSize:MTLSizeMake(3, 2, 1) toBuffer:buffer destinationOffset:0
      destinationBytesPerRow:256 destinationBytesPerImage:512];
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    [client stop];
    if (command.status != MTLCommandBufferStatusCompleted) return 1;
    const auto* raw = static_cast<const std::uint8_t*>(buffer.contents);
    const std::vector<std::uint8_t> expected{
      255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 128,
      255, 255, 255, 255, 0, 255, 255, 255, 0, 0, 0, 0,
    };
    std::vector<std::uint8_t> actual;
    for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 3; ++x) {
      const auto offset = y * 256 + x * 4;
      actual.insert(actual.end(), {raw[offset + 2], raw[offset + 1], raw[offset], raw[offset + 3]});
    }
    std::cout << "{\"exact\":" << (actual == expected ? "true" : "false") << ",\"rgba\":[";
    for (std::size_t i = 0; i < actual.size(); ++i) {
      if (i) std::cout << ',';
      std::cout << static_cast<unsigned>(actual[i]);
    }
    std::cout << "]}\n";
    return actual == expected ? 0 : 1;
  }
}
