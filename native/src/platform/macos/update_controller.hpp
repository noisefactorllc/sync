#pragma once
#import <AppKit/AppKit.h>

// The bootstrap only uses Sparkle's information probe. No installation path is
// exposed until all owned-process and external-camera interlocks are qualified.
@interface SyncUpdateController : NSObject
- (instancetype)initWithDefaults:(NSUserDefaults*)defaults;
@property(nonatomic, readonly) NSString* statusText;
@property(nonatomic, readonly) NSString* lastCheckText;
@property(nonatomic, readonly) BOOL configured;
@property(nonatomic, readonly) BOOL automaticChecksEnabled;
@property(nonatomic, readonly) BOOL paused;
- (void)checkForUpdates:(id)sender;
- (void)toggleAutomaticChecks:(id)sender;
- (void)pauseFor24Hours:(id)sender;
- (void)pauseIndefinitely:(id)sender;
- (void)resumeUpdates:(id)sender;
@end
