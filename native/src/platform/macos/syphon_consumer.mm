#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <sync/platform/syphon_consumer.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

@protocol SyncSyphonMetalServer <NSObject>
- (id)initWithName:(NSString*)name
            device:(id<MTLDevice>)device
           options:(NSDictionary<NSString*, id>*)options;
- (void)publishFrameTexture:(id<MTLTexture>)texture
            onCommandBuffer:(id<MTLCommandBuffer>)commandBuffer
                imageRegion:(NSRect)region
                    flipped:(BOOL)flipped;
- (void)stop;
@end

namespace noisefactor::sync {
namespace {

constexpr std::size_t kMaximumSenderIdBytes = 128;
constexpr std::size_t kMaximumSenderNameBytes = 64;
constexpr std::size_t kMaximumDiscoveryPaths = 4;

auto class_has_required_abi(Class server_class) noexcept -> bool {
  if (server_class == Nil) {
    return false;
  }
  @try {
    return [server_class instancesRespondToSelector:@selector(initWithName:device:options:)] &&
           [server_class
               instancesRespondToSelector:
                   @selector(publishFrameTexture:onCommandBuffer:imageRegion:flipped:)] &&
           [server_class instancesRespondToSelector:@selector(stop)];
  } @catch (NSException*) {
    return false;
  }
}

auto exact_utf8_string(std::string_view bytes) noexcept -> NSString* {
  if (bytes.empty() || bytes.size() > kMaximumSenderNameBytes) {
    return nil;
  }
  @try {
    NSString* value = [[NSString alloc] initWithBytes:bytes.data()
                                               length:bytes.size()
                                             encoding:NSUTF8StringEncoding];
    if (value == nil) {
      return nil;
    }
    NSData* encoded = [value dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
    if (encoded == nil || encoded.length != bytes.size() ||
        std::memcmp(encoded.bytes, bytes.data(), bytes.size()) != 0) {
      return nil;
    }
    return value;
  } @catch (NSException*) {
    return nil;
  }
}

}  // namespace

struct SyphonMetalConsumer::Impl {
  struct SenderEntry {
    bool occupied = false;
    std::size_t sender_id_length = 0;
    std::array<char, kMaximumSenderIdBytes> sender_id{};
    id<SyncSyphonMetalServer> __strong server = nil;
    id<MTLDevice> __weak device = nil;

    [[nodiscard]] auto id_view() const noexcept -> std::string_view {
      return {sender_id.data(), sender_id_length};
    }
  };

  explicit Impl(Options options) : explicit_framework_path(options.framework_path) {
    discover();
  }

  std::string explicit_framework_path;
  Class server_class = Nil;
  NSBundle* __strong loaded_bundle = nil;
  std::array<SenderEntry, SyphonMetalConsumer::kMaximumSenderEntries> senders{};

  void discover() noexcept {
    @autoreleasepool {
      @try {
        Class registered = NSClassFromString(@"SyphonMetalServer");
        if (registered != Nil) {
          if (class_has_required_abi(registered)) {
            server_class = registered;
          }
          return;
        }

        std::array<NSString*, kMaximumDiscoveryPaths> paths{};
        std::size_t path_count = 0;
        const auto add_path = [&](NSString* path) {
          if (path == nil || path.length == 0 || path_count >= paths.size()) {
            return;
          }
          for (std::size_t index = 0; index < path_count; ++index) {
            if ([paths[index] isEqualToString:path]) {
              return;
            }
          }
          paths[path_count++] = path;
        };

        if (!explicit_framework_path.empty()) {
          NSString* explicit_path =
              [[NSString alloc] initWithBytes:explicit_framework_path.data()
                                       length:explicit_framework_path.size()
                                     encoding:NSUTF8StringEncoding];
          add_path(explicit_path);
        }

        NSString* private_frameworks = NSBundle.mainBundle.privateFrameworksPath;
        if (private_frameworks != nil) {
          add_path([private_frameworks stringByAppendingPathComponent:@"Syphon.framework"]);
        }
        NSString* home = NSHomeDirectory();
        if (home != nil) {
          add_path([home stringByAppendingPathComponent:@"Library/Frameworks/Syphon.framework"]);
        }
        add_path(@"/Library/Frameworks/Syphon.framework");

        for (std::size_t index = 0; index < path_count; ++index) {
          NSBundle* candidate = [NSBundle bundleWithPath:paths[index]];
          if (candidate == nil) {
            continue;
          }
          NSError* error = nil;
          if (![candidate loadAndReturnError:&error]) {
            continue;
          }
          (void)error;
          Class discovered = NSClassFromString(@"SyphonMetalServer");
          if (!class_has_required_abi(discovered)) {
            continue;
          }
          loaded_bundle = candidate;
          server_class = discovered;
          return;
        }
      } @catch (NSException*) {
        server_class = Nil;
        loaded_bundle = nil;
      }
    }
  }

  [[nodiscard]] auto find_sender(std::string_view sender_id) noexcept -> SenderEntry* {
    for (SenderEntry& entry : senders) {
      if (entry.occupied && entry.sender_id_length == sender_id.size() &&
          entry.id_view() == sender_id) {
        return &entry;
      }
    }
    return nullptr;
  }

  void stop_and_clear(SenderEntry& entry) noexcept {
    if (!entry.occupied) {
      return;
    }
    @autoreleasepool {
      id<SyncSyphonMetalServer> server = entry.server;
      entry.occupied = false;
      entry.sender_id_length = 0;
      entry.server = nil;
      entry.device = nil;
      if (server == nil) {
        return;
      }
      @try {
        [server stop];
      } @catch (NSException*) {
      }
    }
  }
};

SyphonMetalConsumer::SyphonMetalConsumer() : SyphonMetalConsumer(Options{}) {}

SyphonMetalConsumer::SyphonMetalConsumer(Options options)
    : impl_(std::make_unique<Impl>(options)) {}

SyphonMetalConsumer::~SyphonMetalConsumer() {
  if (impl_ == nullptr) {
    return;
  }
  for (std::size_t index = impl_->senders.size(); index > 0; --index) {
    impl_->stop_and_clear(impl_->senders[index - 1U]);
  }
}

auto SyphonMetalConsumer::available() const noexcept -> bool {
  return impl_ != nullptr && impl_->server_class != Nil;
}

auto SyphonMetalConsumer::open_sender(std::string_view sender_id,
                                      std::string_view name,
                                      id<MTLDevice> device) noexcept -> bool {
  if (!available() || sender_id.empty() || sender_id.size() > kMaximumSenderIdBytes ||
      name.empty() || name.size() > kMaximumSenderNameBytes || device == nil ||
      impl_->find_sender(sender_id) != nullptr) {
    return false;
  }

  Impl::SenderEntry* entry = nullptr;
  for (Impl::SenderEntry& candidate : impl_->senders) {
    if (!candidate.occupied) {
      entry = &candidate;
      break;
    }
  }
  if (entry == nullptr) {
    return false;
  }

  @autoreleasepool {
    NSString* exact_name = exact_utf8_string(name);
    if (exact_name == nil) {
      return false;
    }
    @try {
      id allocated = [impl_->server_class alloc];
      id<SyncSyphonMetalServer> server =
          [(id<SyncSyphonMetalServer>)allocated initWithName:exact_name device:device options:nil];
      if (server == nil) {
        return false;
      }
      entry->sender_id_length = sender_id.size();
      std::memcpy(entry->sender_id.data(), sender_id.data(), sender_id.size());
      entry->server = server;
      entry->device = device;
      entry->occupied = true;
      return true;
    } @catch (NSException*) {
      return false;
    }
  }
}

void SyphonMetalConsumer::close_sender(std::string_view sender_id) noexcept {
  if (impl_ == nullptr) {
    return;
  }
  Impl::SenderEntry* entry = impl_->find_sender(sender_id);
  if (entry != nullptr) {
    impl_->stop_and_clear(*entry);
  }
}

auto SyphonMetalConsumer::encode_frame(std::string_view sender_id,
                                       id<MTLTexture> texture,
                                       id<MTLCommandBuffer> command_buffer,
                                       const MetalFrameMetadata& metadata) noexcept -> bool {
  if (impl_ == nullptr || texture == nil || command_buffer == nil || !metadata.top_down ||
      metadata.width == 0 || metadata.height == 0) {
    return false;
  }
  Impl::SenderEntry* entry = impl_->find_sender(sender_id);
  if (entry == nullptr || entry->server == nil) {
    return false;
  }

  @autoreleasepool {
    @try {
      id<MTLDevice> device = entry->device;
      if (device == nil || texture.device != device || command_buffer.device != device ||
          texture.width != metadata.width || texture.height != metadata.height ||
          command_buffer.status != MTLCommandBufferStatusNotEnqueued) {
        return false;
      }
      [entry->server publishFrameTexture:texture
                         onCommandBuffer:command_buffer
                             imageRegion:NSMakeRect(0, 0, metadata.width, metadata.height)
                                 flipped:NO];
      return true;
    } @catch (NSException*) {
      return false;
    }
  }
}

}  // namespace noisefactor::sync
