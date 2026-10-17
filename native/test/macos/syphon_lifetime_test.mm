#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../test_harness.hpp"
#include <sync/platform/syphon_consumer.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <string>
using namespace noisefactor::sync;
using namespace std::chrono_literals;
namespace {
struct Observation { std::atomic<unsigned> stops{0}; std::atomic<bool> block_stop{false}, release_stop{false}, stop_finished{false}; std::atomic<unsigned> finished{0}, active_publications{0}, completed_publications{0}, overlaps{0}; std::atomic<bool> gate_publish{false}, release_publish{false}; };
std::shared_ptr<Observation> next_observation;
bool wait_until(const auto&predicate,std::chrono::milliseconds bound=1000ms){auto end=std::chrono::steady_clock::now()+bound;while(!predicate()&&std::chrono::steady_clock::now()<end)std::this_thread::sleep_for(1ms);return predicate();}
}
@interface SyphonMetalServer : NSObject { std::shared_ptr<Observation> observation_; }
-(id)initWithName:(NSString*)name device:(id<MTLDevice>)device options:(NSDictionary*)options;
-(void)publishFrameTexture:(id<MTLTexture>)texture onCommandBuffer:(id<MTLCommandBuffer>)buffer imageRegion:(NSRect)region flipped:(BOOL)flipped;
-(void)stop;
@end
@implementation SyphonMetalServer
-(id)initWithName:(NSString*)name device:(id<MTLDevice>)device options:(NSDictionary*)options { (void)name;(void)device;(void)options;self=[super init];if(self)observation_=next_observation;return self; }
-(void)publishFrameTexture:(id<MTLTexture>)texture onCommandBuffer:(id<MTLCommandBuffer>)buffer imageRegion:(NSRect)region flipped:(BOOL)flipped {(void)texture;(void)region;(void)flipped;
 auto observation=observation_;
 [buffer addCompletedHandler:^(id<MTLCommandBuffer>){
   observation->active_publications.fetch_add(1);
   while(observation->gate_publish&&!observation->release_publish)std::this_thread::sleep_for(1ms);
   observation->active_publications.fetch_sub(1);
   observation->completed_publications.fetch_add(1);
 }];}
-(void)stop {if(observation_->active_publications.load())observation_->overlaps.fetch_add(1);observation_->stops.fetch_add(1);while(observation_->block_stop&&!observation_->release_stop)std::this_thread::sleep_for(1ms);observation_->stop_finished=true;observation_->finished.fetch_add(1);}
@end
@interface LifetimeTexture : NSObject
@property(nonatomic,strong) id<MTLDevice> device;
@end
@implementation LifetimeTexture
-(NSUInteger)width{return 2;}-(NSUInteger)height{return 2;}
@end
@interface LifetimeBuffer : NSObject
@property(nonatomic,strong) id<MTLDevice> device;
@property(nonatomic,strong) NSMutableArray* handlers;
-(void)addCompletedHandler:(MTLCommandBufferHandler)handler;
-(void)complete;
@end
@implementation LifetimeBuffer
-(id)init{self=[super init];if(self)_handlers=[NSMutableArray new];return self;}
-(MTLCommandBufferStatus)status{return MTLCommandBufferStatusNotEnqueued;}
-(void)addCompletedHandler:(MTLCommandBufferHandler)handler{[self.handlers addObject:[handler copy]];}
-(void)complete{for(MTLCommandBufferHandler handler in self.handlers)handler((id<MTLCommandBuffer>)self);}
@end
namespace {
struct Fixture {
 id<MTLDevice> __strong device=(id<MTLDevice>)[NSObject new];
 LifetimeTexture* __strong texture=[LifetimeTexture new];
 std::shared_ptr<Observation> observation=std::make_shared<Observation>();
 SyphonMetalConsumer consumer;
 Fixture(){texture.device=device;next_observation=observation;SYNC_REQUIRE(consumer.open_sender("sender","Lifetime",device));}
 LifetimeBuffer* buffer(){LifetimeBuffer*b=[LifetimeBuffer new];b.device=device;SYNC_REQUIRE(consumer.encode_frame("sender",(id<MTLTexture>)texture,(id<MTLCommandBuffer>)b,MetalFrameMetadata{.width=2,.height=2}));return b;}
};
SYNC_TEST(syphon_close_defers_stop_until_completion_even_when_the_buffer_is_retained){@autoreleasepool{
 Fixture f;LifetimeBuffer*buffer=f.buffer();f.consumer.close_sender("sender");SYNC_REQUIRE(f.observation->stops==0);
 [buffer complete];SYNC_REQUIRE(wait_until([&]{return f.observation->stop_finished.load();}));SYNC_REQUIRE(f.observation->stops==1);SYNC_REQUIRE(buffer!=nil);SYNC_REQUIRE(f.observation->completed_publications==1);
}}
SYNC_TEST(syphon_close_waits_for_every_inflight_buffer){@autoreleasepool{
 Fixture f;LifetimeBuffer*first=f.buffer();LifetimeBuffer*last=f.buffer();f.consumer.close_sender("sender");SYNC_REQUIRE(f.observation->stops==0);
 [first complete];SYNC_REQUIRE(f.observation->stops==0);[last complete];SYNC_REQUIRE(wait_until([&]{return f.observation->stop_finished.load();}));SYNC_REQUIRE(f.observation->stops==1);
}}
SYNC_TEST(syphon_uncommitted_buffer_disposal_releases_the_closed_server){@autoreleasepool{
 Fixture f;@autoreleasepool{LifetimeBuffer*buffer=f.buffer();SYNC_REQUIRE(buffer!=nil);f.consumer.close_sender("sender");SYNC_REQUIRE(f.observation->stops==0);buffer=nil;}
 SYNC_REQUIRE(wait_until([&]{return f.observation->stop_finished.load();}));SYNC_REQUIRE(f.observation->stops==1);
}}
SYNC_TEST(syphon_completion_does_not_run_blocking_stop_on_the_completion_thread){@autoreleasepool{
 Fixture f;LifetimeBuffer*buffer=f.buffer();f.consumer.close_sender("sender");SYNC_REQUIRE(f.observation->stops==0);f.observation->block_stop=true;
 std::atomic<bool> returned{false};std::thread completion([&]{@autoreleasepool{[buffer complete];returned=true;}});
 const bool prompt=wait_until([&]{return returned.load();},200ms);f.observation->release_stop=true;completion.join();
 SYNC_REQUIRE(prompt);SYNC_REQUIRE(wait_until([&]{return f.observation->stop_finished.load();}));SYNC_REQUIRE(f.observation->stops==1);
}}
SYNC_TEST(syphon_pending_frame_lifetime_survives_consumer_destruction){@autoreleasepool{
 auto observation=std::make_shared<Observation>();LifetimeBuffer*buffer=nil;
 {Fixture f;observation=f.observation;buffer=f.buffer();}SYNC_REQUIRE(observation->stops==0);
 [buffer complete];SYNC_REQUIRE(wait_until([&]{return observation->stop_finished.load();}));SYNC_REQUIRE(observation->stops==1);
}}
SYNC_TEST(syphon_stop_waits_until_publication_handler_returns){@autoreleasepool{
 Fixture f;LifetimeBuffer*buffer=f.buffer();f.observation->gate_publish=true;
 std::thread completion([&]{@autoreleasepool{[buffer complete];}});
 const bool entered=wait_until([&]{return f.observation->active_publications==1;});
 f.consumer.close_sender("sender");const auto stopped_while_active=f.observation->stops.load();
 f.observation->release_publish=true;completion.join();
 SYNC_REQUIRE(entered);SYNC_REQUIRE(stopped_while_active==0);
 SYNC_REQUIRE(wait_until([&]{return f.observation->finished==1;}));SYNC_REQUIRE(f.observation->overlaps==0);
}}
SYNC_TEST(syphon_pending_frame_survives_creator_thread_exit){@autoreleasepool{
 LifetimeBuffer* __strong buffer=nil;std::shared_ptr<Observation> observation;
 std::thread creator([&]{@autoreleasepool{Fixture f;buffer=f.buffer();observation=f.observation;}});creator.join();
 SYNC_REQUIRE(observation->stops==0);[buffer complete];
 SYNC_REQUIRE(wait_until([&]{return observation->finished==1;}));SYNC_REQUIRE(observation->overlaps==0);
}}
SYNC_TEST(syphon_retiring_servers_keep_the_bounded_sender_slots){@autoreleasepool{
 Fixture f;NSMutableArray<LifetimeBuffer*>*buffers=[NSMutableArray new];
 [buffers addObject:f.buffer()];f.consumer.close_sender("sender");
 for(unsigned i=1;i<SyphonMetalConsumer::kMaximumSenderEntries;++i){
   std::string sender_id="sender-"+std::to_string(i);
   SYNC_REQUIRE(f.consumer.open_sender(sender_id,sender_id,f.device));
   LifetimeBuffer*buffer=[LifetimeBuffer new];buffer.device=f.device;
   SYNC_REQUIRE(f.consumer.encode_frame(sender_id,(id<MTLTexture>)f.texture,(id<MTLCommandBuffer>)buffer,MetalFrameMetadata{.width=2,.height=2}));
   [buffers addObject:buffer];f.consumer.close_sender(sender_id);
 }
 const bool admitted_while_pending=f.consumer.open_sender("pending-overflow","pending-overflow",f.device);
 if(admitted_while_pending)f.consumer.close_sender("pending-overflow");
 const auto before=f.observation->finished.load();f.observation->block_stop=true;
 std::thread completion([&]{@autoreleasepool{for(LifetimeBuffer*buffer in buffers)[buffer complete];}});completion.join();
 const bool admitted_while_stopping=f.consumer.open_sender("stopping-overflow","stopping-overflow",f.device);
 f.observation->release_stop=true;
 const bool retired=wait_until([&]{return f.observation->finished==before+SyphonMetalConsumer::kMaximumSenderEntries;});
 if(admitted_while_stopping)f.consumer.close_sender("stopping-overflow");
 SYNC_REQUIRE(!admitted_while_pending);SYNC_REQUIRE(!admitted_while_stopping);SYNC_REQUIRE(retired);
 SYNC_REQUIRE(f.consumer.open_sender("replacement","replacement",f.device));
}}
}
