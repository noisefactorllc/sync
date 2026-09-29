#include "test_harness.hpp"
#import "update_controller.hpp"
#import <Foundation/Foundation.h>

SYNC_TEST(update_preferences_preserve_explicit_opt_out_and_pause) {
  @autoreleasepool {
    NSString* name=[@"io.noisefactor.sync.test.update." stringByAppendingString:NSUUID.UUID.UUIDString];
    NSUserDefaults* prefs=[[NSUserDefaults alloc] initWithSuiteName:name];
    SyncUpdateController* first=[[SyncUpdateController alloc] initWithDefaults:prefs];
    SYNC_REQUIRE(first.automaticChecksEnabled);
    [first toggleAutomaticChecks:nil];
    SYNC_REQUIRE(!first.automaticChecksEnabled);
    [first pauseFor24Hours:nil];
    SYNC_REQUIRE(first.paused);
    SyncUpdateController* second=[[SyncUpdateController alloc] initWithDefaults:prefs];
    SYNC_REQUIRE(!second.automaticChecksEnabled);
    SYNC_REQUIRE(second.paused);
    [second resumeUpdates:nil];
    SYNC_REQUIRE(!second.paused);
    [prefs removePersistentDomainForName:name];
  }
}
SYNC_TEST(update_unconfigured_build_reports_configuration_without_installation) {
  @autoreleasepool {
    NSString* name=[@"io.noisefactor.sync.test.update." stringByAppendingString:NSUUID.UUID.UUIDString];
    NSUserDefaults* prefs=[[NSUserDefaults alloc] initWithSuiteName:name];
    SyncUpdateController* controller=[[SyncUpdateController alloc] initWithDefaults:prefs];
    SYNC_REQUIRE(!controller.configured);
    SYNC_REQUIRE([controller.statusText containsString:@"not configured"]);
    [prefs removePersistentDomainForName:name];
  }
}
