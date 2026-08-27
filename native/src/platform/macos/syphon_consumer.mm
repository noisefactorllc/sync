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
  // Only meaningful when discovery failed. unavailable_reason() answers None
  // whenever available() is true, so success never has to clear this and no
  // path can leave the two disagreeing.
  SyphonUnavailableReason reason = SyphonUnavailableReason::None;
  Class server_class = Nil;
  NSBundle* __strong loaded_bundle = nil;
  std::array<SenderEntry, SyphonMetalConsumer::kMaximumSenderEntries> senders{};

  // Discovery keeps the most specific failure it reached, because that is the
  // one an operator can act on: "a framework loaded but has no
  // SyphonMetalServer" points at a bad framework, while "nothing was found"
  // points at an install step that never ran. A later candidate that merely
  // does not exist must not overwrite an earlier one that loaded and was
  // wrong.
  void note(SyphonUnavailableReason candidate) noexcept {
    const auto rank = [](SyphonUnavailableReason value) noexcept -> int {
      switch (value) {
        case SyphonUnavailableReason::None:
          return 0;
        case SyphonUnavailableReason::FrameworkNotFound:
          return 1;
        case SyphonUnavailableReason::FrameworkLoadFailed:
          return 2;
        case SyphonUnavailableReason::ServerClassMissing:
          return 3;
        case SyphonUnavailableReason::ServerClassIncompatible:
          return 4;
        case SyphonUnavailableReason::DiscoveryFailed:
          return 5;
      }
      return 0;
    };
    if (rank(candidate) > rank(reason)) {
      reason = candidate;
    }
  }

  void discover() noexcept {
    reason = SyphonUnavailableReason::FrameworkNotFound;
    @autoreleasepool {
      @try {
        Class registered = NSClassFromString(@"SyphonMetalServer");
        if (registered != Nil) {
          if (class_has_required_abi(registered)) {
            server_class = registered;
          } else {
            // Something already in this process answers to the name but not to
            // the ABI -- a Syphon too old or too new for this daemon. Loading
            // another copy cannot displace it, so discovery stops here.
            reason = SyphonUnavailableReason::ServerClassIncompatible;
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
        // Deliberately no per-user ~/Library/Frameworks entry. Loading a
        // bundle executes its code, and a user-writable search path gives any
        // process running as this user a standing injection point into the
        // daemon. The packaged app ships a pinned framework in its own bundle;
        // a developer build passes --syphon-framework explicitly.
        add_path(@"/Library/Frameworks/Syphon.framework");

        for (std::size_t index = 0; index < path_count; ++index) {
          NSBundle* candidate = [NSBundle bundleWithPath:paths[index]];
          if (candidate == nil) {
            note(SyphonUnavailableReason::FrameworkNotFound);
            continue;
          }
          NSError* error = nil;
          if (![candidate loadAndReturnError:&error]) {
            // The NSError is deliberately not carried out of here. It quotes
            // absolute paths and loader detail, and this phrase is printed to
            // stderr and pasted into bug reports.
            (void)error;
            note(SyphonUnavailableReason::FrameworkLoadFailed);
            continue;
          }
          Class discovered = NSClassFromString(@"SyphonMetalServer");
          if (discovered == Nil) {
            // A framework that loads cleanly and registers nothing. The
            // placeholder shipped inside Sync.app 0.2.0 behaved exactly this
            // way, and reported only `available:false`.
            note(SyphonUnavailableReason::ServerClassMissing);
            continue;
          }
          if (!class_has_required_abi(discovered)) {
            note(SyphonUnavailableReason::ServerClassIncompatible);
            continue;
          }
          loaded_bundle = candidate;
          server_class = discovered;
          return;
        }
      } @catch (NSException*) {
        server_class = Nil;
        loaded_bundle = nil;
        reason = SyphonUnavailableReason::DiscoveryFailed;
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

auto describe(SyphonUnavailableReason reason) noexcept -> const char* {
  switch (reason) {
    case SyphonUnavailableReason::None:
      return "the Syphon runtime is available";
    case SyphonUnavailableReason::FrameworkNotFound:
      return "no Syphon.framework was found in any searched location";
    case SyphonUnavailableReason::FrameworkLoadFailed:
      return "a Syphon.framework was found but could not be loaded";
    case SyphonUnavailableReason::ServerClassMissing:
      return "the Syphon.framework that loaded does not provide SyphonMetalServer";
    case SyphonUnavailableReason::ServerClassIncompatible:
      return "the Syphon.framework that loaded provides an incompatible SyphonMetalServer";
    case SyphonUnavailableReason::DiscoveryFailed:
      return "Syphon discovery stopped on an unexpected error";
  }
  return "Syphon is unavailable for an unrecognized reason";
}

auto SyphonMetalConsumer::available() const noexcept -> bool {
  return impl_ != nullptr && impl_->server_class != Nil;
}

auto SyphonMetalConsumer::unavailable_reason() const noexcept -> SyphonUnavailableReason {
  if (available()) {
    return SyphonUnavailableReason::None;
  }
  return impl_ == nullptr ? SyphonUnavailableReason::DiscoveryFailed : impl_->reason;
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
