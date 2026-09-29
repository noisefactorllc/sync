#import "update_controller.hpp"
#if defined(SYNC_HAS_SPARKLE)
#import <Sparkle.h>
#endif

namespace {
NSString* const enabledKey=@"SyncUpdateChecksEnabled";
NSString* const pauseKey=@"SyncUpdatesPausedUntil";
NSString* const checkedKey=@"SyncUpdateLastCheck";
NSString* const resultKey=@"SyncUpdateLastResult";
}
#if defined(SYNC_HAS_SPARKLE)
@interface SyncUpdateController () <SPUUpdaterDelegate,SPUStandardUserDriverDelegate>
@end
#endif
@implementation SyncUpdateController {
  NSUserDefaults* _defaults;
  NSTimer* _timer;
  NSString* _status;
  BOOL _manual;
#if defined(SYNC_HAS_SPARKLE)
  SPUStandardUpdaterController* _controller;
#endif
}
- (instancetype)initWithDefaults:(NSUserDefaults*)defaults {
  self=[super init]; if(!self) return nil;
  _defaults=defaults;
  [_defaults registerDefaults:@{enabledKey:@YES}];
  _status=@"Automatic updates are not configured in this build";
#if defined(SYNC_HAS_SPARKLE)
  NSDictionary* info=NSBundle.mainBundle.infoDictionary;
  NSString* feed=info[@"SUFeedURL"];
  NSString* key=info[@"SUPublicEDKey"];
  NSData* keyBytes=[key isKindOfClass:NSString.class] ? [[NSData alloc] initWithBase64EncodedString:key options:0] : nil;
  if ([feed isEqualToString:@"https://downloads.noisefactor.io/updates/sync/preview/macos-arm64.xml"] &&
      keyBytes.length==32 && [info[@"SURequireSignedFeed"] boolValue] &&
      [info[@"SUVerifyUpdateBeforeExtraction"] boolValue]) {
    _controller=[[SPUStandardUpdaterController alloc] initWithStartingUpdater:NO updaterDelegate:self userDriverDelegate:self];
    // Sync owns scheduling and pause policy. Information probes cannot launch
    // an installer or leave a downloaded update armed for install-on-quit.
    _controller.updater.automaticallyChecksForUpdates=NO;
    _controller.updater.automaticallyDownloadsUpdates=NO;
    NSError* error=nil;
    if (![_controller.updater startUpdater:&error]) {
      _controller=nil; _status=@"Update checking could not start";
    } else {
      NSString* previousResult = [_defaults stringForKey:resultKey];
      _status = previousResult != nil ? previousResult : @"Updates: not checked yet";
      __weak SyncUpdateController* weakSelf=self;
      _timer=[NSTimer scheduledTimerWithTimeInterval:60 repeats:YES block:^(NSTimer*) {
        SyncUpdateController* owner=weakSelf;
        if(owner && owner.automaticChecksEnabled && !owner.paused &&
            NSDate.date.timeIntervalSince1970-[owner->_defaults doubleForKey:checkedKey]>=86400) {
          [owner startCheck:NO];
        }
      }];
    }
  }
#endif
  return self;
}
- (void)dealloc {[_timer invalidate];}
- (BOOL)configured {
#if defined(SYNC_HAS_SPARKLE)
  return _controller!=nil;
#else
  return NO;
#endif
}
- (BOOL)automaticChecksEnabled {return [_defaults boolForKey:enabledKey];}
- (BOOL)paused {return [_defaults doubleForKey:pauseKey]>NSDate.date.timeIntervalSince1970;}
- (NSString*)statusText {return self.paused ? @"Update checks paused" : _status;}
- (NSString*)lastCheckText {
  const NSTimeInterval started = [_defaults doubleForKey:checkedKey];
  if (started <= 0) return @"Last check: never";
  NSDateFormatter* formatter = [[NSDateFormatter alloc] init];
  formatter.dateStyle = NSDateFormatterMediumStyle;
  formatter.timeStyle = NSDateFormatterShortStyle;
  return [@"Last check started: " stringByAppendingString:
      [formatter stringFromDate:[NSDate dateWithTimeIntervalSince1970:started]]];
}
- (void)toggleAutomaticChecks:(id)sender {(void)sender;[_defaults setBool:!self.automaticChecksEnabled forKey:enabledKey];}
- (void)pauseFor24Hours:(id)sender {(void)sender;[_defaults setDouble:NSDate.date.timeIntervalSince1970+86400 forKey:pauseKey];}
- (void)pauseIndefinitely:(id)sender {(void)sender;[_defaults setDouble:NSDate.distantFuture.timeIntervalSince1970 forKey:pauseKey];}
- (void)resumeUpdates:(id)sender {(void)sender;[_defaults removeObjectForKey:pauseKey];}
- (void)checkForUpdates:(id)sender {(void)sender;[self startCheck:YES];}
- (void)startCheck:(BOOL)manual {
#if defined(SYNC_HAS_SPARKLE)
  if(_controller && _controller.updater.canCheckForUpdates) {
    _manual=manual;
    [_defaults setDouble:NSDate.date.timeIntervalSince1970 forKey:checkedKey];
    _status=@"Checking for updates…";
    [_controller.updater checkForUpdateInformation];
    return;
  }
#endif
  if(manual) [self showStatus];
}
- (void)showStatus {
  NSAlert* alert=[[NSAlert alloc] init];
  alert.messageText=@"Sync Updates";alert.informativeText=_status;
  [alert addButtonWithTitle:@"OK"];[NSApp activateIgnoringOtherApps:YES];[alert runModal];
}
#if defined(SYNC_HAS_SPARKLE)
- (BOOL)supportsGentleScheduledUpdateReminders {return YES;}
- (BOOL)updater:(SPUUpdater*)updater mayPerformUpdateCheck:(SPUUpdateCheck)check error:(NSError**)error {
  (void)updater;
  if(check==SPUUpdateCheckUpdateInformation) return YES;
  if(error) *error=[NSError errorWithDomain:@"io.noisefactor.sync.updates" code:1
      userInfo:@{NSLocalizedDescriptionKey:@"Automatic installation is unavailable in this build."}];
  return NO;
}
- (BOOL)updater:(SPUUpdater*)updater shouldProceedWithUpdate:(SUAppcastItem*)item updateCheck:(SPUUpdateCheck)check error:(NSError**)error {
  (void)updater;(void)item;
  if(check==SPUUpdateCheckUpdateInformation) return YES;
  if(error) *error=[NSError errorWithDomain:@"io.noisefactor.sync.updates" code:1 userInfo:@{NSLocalizedDescriptionKey:@"Installation is not enabled in this build."}];
  return NO;
}
- (void)updater:(SPUUpdater*)updater didFindValidUpdate:(SUAppcastItem*)item {
  (void)updater;
  _status=[NSString stringWithFormat:@"Sync %@ is available. Download it from the Sync website to install it.",item.displayVersionString];
  [_defaults setObject:_status forKey:resultKey];
}
- (void)updaterDidNotFindUpdate:(SPUUpdater*)updater {(void)updater;_status=@"Sync is up to date";[_defaults setObject:_status forKey:resultKey];}
- (void)updater:(SPUUpdater*)updater didFinishUpdateCycleForUpdateCheck:(SPUUpdateCheck)check error:(NSError*)error {
  (void)updater;(void)check;
  if(error && error.code!=SUNoUpdateError) _status=@"Update check failed; Sync is still running normally";
  [_defaults setObject:_status forKey:resultKey];
  if(_manual) {_manual=NO;[self showStatus];}
}
#endif
@end
