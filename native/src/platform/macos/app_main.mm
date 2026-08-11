#include "companion_process.hpp"

#import <AppKit/AppKit.h>
#import <ServiceManagement/ServiceManagement.h>

#include <sync/platform/companion_model.hpp>
#include <sync/server.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace companion = noisefactor::sync::companion;

namespace {

NSString* ns_string(std::string_view value) {
  return [[NSString alloc] initWithBytes:value.data()
                                  length:value.size()
                                encoding:NSUTF8StringEncoding];
}

NSString* status_title(companion::CompanionState state) {
  return [NSString stringWithFormat:@"Sync — %@",
                                    ns_string(companion::state_name(state))];
}

NSMenuItem* disabled_item(NSString* title) {
  NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                               action:nil
                                        keyEquivalent:@""];
  item.enabled = NO;
  return item;
}

void show_error(NSString* title, NSString* message) {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.alertStyle = NSAlertStyleWarning;
  alert.messageText = title;
  alert.informativeText = message;
  [alert addButtonWithTitle:@"OK"];
  [alert runModal];
}

} // namespace

@interface SyncAppDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate>
@end

@implementation SyncAppDelegate {
  NSStatusItem* _statusItem;
  NSMenu* _menu;
  NSTimer* _pollTimer;
  std::unique_ptr<companion::CompanionModel> _model;
  std::unique_ptr<companion::CompanionProcess> _process;
  std::vector<std::string> _pairings;
  std::string _pairingError;
  BOOL _pairingsLoading;
  BOOL _expectedExit;
  BOOL _quitting;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  (void)notification;
  _model = std::make_unique<companion::CompanionModel>(
      std::string(noisefactor::sync::kProductVersion));

  NSBundle* bundle = NSBundle.mainBundle;
  NSString* helper = [bundle.bundlePath
      stringByAppendingPathComponent:@"Contents/MacOS/syncd"];
  NSString* framework = [bundle.privateFrameworksPath
      stringByAppendingPathComponent:@"Syphon.framework"];
  _process = std::make_unique<companion::CompanionProcess>(
      companion::CompanionProcessOptions{
          .helper_path = helper.UTF8String,
          .framework_path = framework.UTF8String,
      });

  _statusItem = [NSStatusBar.systemStatusBar
      statusItemWithLength:NSSquareStatusItemLength];
  NSImage* image = [NSImage imageWithSystemSymbolName:@"arrow.triangle.2.circlepath"
                            accessibilityDescription:@"Sync"];
  [image setTemplate:YES];
  _statusItem.button.image = image;
  _statusItem.button.toolTip = @"Sync Preview";
  _menu = [[NSMenu alloc] initWithTitle:@"Sync"];
  _menu.delegate = self;
  _statusItem.menu = _menu;

  [self probeThenStart];
  _pollTimer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                target:self
                                              selector:@selector(pollStatus:)
                                              userInfo:nil
                                               repeats:YES];
  [self refreshPairings];
  [self showPreviewNoticeIfNeeded];
}

- (void)probeThenStart {
  __weak SyncAppDelegate* weakSelf = self;
  _process->probe([weakSelf](std::optional<companion::HealthSnapshot> health,
                            std::string error) {
    (void)error;
    SyncAppDelegate* self = weakSelf;
    if (self == nil || self->_quitting) return;
    if (health.has_value()) {
      self->_model->observe_health(std::move(*health));
      return;
    }
    [self startHelper];
  });
}

- (void)startHelper {
  if (_quitting || _process->owned_pid().has_value()) return;
  _model->begin_start();
  __weak SyncAppDelegate* weakSelf = self;
  std::string error;
  const bool started = _process->start(
      [weakSelf](std::string_view bytes) {
        SyncAppDelegate* self = weakSelf;
        if (self != nil) self->_model->append_stderr(bytes);
      },
      [weakSelf](int status) {
        SyncAppDelegate* self = weakSelf;
        if (self == nil) return;
        const bool expected = self->_expectedExit || self->_quitting;
        self->_expectedExit = NO;
        self->_model->helper_exited(status, expected);
      },
      error);
  if (!started) {
    _model->helper_exited(-1, false);
    _model->append_stderr(error);
    return;
  }
  if (const auto pid = _process->owned_pid(); pid.has_value()) {
    _model->helper_started(*pid);
  }
}

- (void)pollStatus:(NSTimer*)timer {
  (void)timer;
  if (_quitting) return;
  __weak SyncAppDelegate* weakSelf = self;
  _process->probe([weakSelf](std::optional<companion::HealthSnapshot> health,
                            std::string error) {
    (void)error;
    SyncAppDelegate* self = weakSelf;
    if (self == nil || self->_quitting) return;
    if (health.has_value()) {
      self->_model->observe_health(std::move(*health));
    } else {
      self->_model->observe_health_failure();
    }
  });
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
  (void)menu;
  [_menu removeAllItems];
  [_menu addItem:disabled_item(status_title(_model->state()))];

  const companion::HealthSnapshot& health = _model->health();
  NSString* syphon = health.reachable
                         ? (health.syphon_available ? @"Available" : @"Unavailable")
                         : @"Unknown";
  [_menu addItem:disabled_item(
                     [NSString stringWithFormat:@"Syphon: %@", syphon])];
  NSString* senders = health.active_senders.has_value()
                          ? [NSString stringWithFormat:@"%zu",
                                                       *health.active_senders]
                          : @"Unavailable";
  [_menu addItem:disabled_item(
                     [NSString stringWithFormat:@"Active senders: %@", senders])];
  [_menu addItem:NSMenuItem.separatorItem];

  NSMenuItem* restart = [[NSMenuItem alloc] initWithTitle:@"Restart Sync"
                                                  action:@selector(restartSync:)
                                           keyEquivalent:@""];
  restart.target = self;
  restart.enabled = _model->state() != companion::CompanionState::External;
  [_menu addItem:restart];

  NSMenuItem* pairingsItem = [[NSMenuItem alloc] initWithTitle:@"Pairings"
                                                       action:nil
                                                keyEquivalent:@""];
  NSMenu* pairingsMenu = [[NSMenu alloc] initWithTitle:@"Pairings"];
  if (_pairingsLoading) {
    [pairingsMenu addItem:disabled_item(@"Loading…")];
  } else if (!_pairingError.empty()) {
    [pairingsMenu addItem:disabled_item(ns_string(_pairingError))];
  } else if (_pairings.empty()) {
    [pairingsMenu addItem:disabled_item(@"No paired apps")];
  } else {
    for (const std::string& origin : _pairings) {
      NSMenuItem* revoke = [[NSMenuItem alloc] initWithTitle:ns_string(origin)
                                                     action:@selector(revokePairing:)
                                              keyEquivalent:@""];
      revoke.target = self;
      revoke.representedObject = ns_string(origin);
      [pairingsMenu addItem:revoke];
    }
  }
  [pairingsMenu addItem:NSMenuItem.separatorItem];
  NSMenuItem* refresh = [[NSMenuItem alloc] initWithTitle:@"Refresh"
                                                   action:@selector(refreshPairingsAction:)
                                            keyEquivalent:@""];
  refresh.target = self;
  [pairingsMenu addItem:refresh];
  pairingsItem.submenu = pairingsMenu;
  [_menu addItem:pairingsItem];

  NSMenuItem* login = [[NSMenuItem alloc] initWithTitle:@"Launch at Login"
                                                 action:@selector(toggleLaunchAtLogin:)
                                          keyEquivalent:@""];
  login.target = self;
  login.state = SMAppService.mainAppService.status == SMAppServiceStatusEnabled
                    ? NSControlStateValueOn
                    : NSControlStateValueOff;
  [_menu addItem:login];

  NSMenuItem* diagnostics =
      [[NSMenuItem alloc] initWithTitle:@"Copy Diagnostics"
                                action:@selector(copyDiagnostics:)
                         keyEquivalent:@""];
  diagnostics.target = self;
  [_menu addItem:diagnostics];
  [_menu addItem:NSMenuItem.separatorItem];

  NSMenuItem* guide = [[NSMenuItem alloc] initWithTitle:@"Sync Guide"
                                                 action:@selector(openGuide:)
                                          keyEquivalent:@""];
  guide.target = self;
  [_menu addItem:guide];
  NSMenuItem* source = [[NSMenuItem alloc] initWithTitle:@"Source Code"
                                                  action:@selector(openSource:)
                                           keyEquivalent:@""];
  source.target = self;
  [_menu addItem:source];
  NSMenuItem* about = [[NSMenuItem alloc] initWithTitle:@"About Sync"
                                                 action:@selector(showAbout:)
                                          keyEquivalent:@""];
  about.target = self;
  [_menu addItem:about];
  NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:@"Quit Sync"
                                                action:@selector(quitSync:)
                                         keyEquivalent:@"q"];
  quit.target = self;
  [_menu addItem:quit];

  if (!_pairingsLoading) [self refreshPairings];
}

- (void)restartSync:(id)sender {
  (void)sender;
  if (_process->owned_pid().has_value()) {
    _expectedExit = YES;
    __weak SyncAppDelegate* weakSelf = self;
    _process->terminate([weakSelf] {
      SyncAppDelegate* self = weakSelf;
      if (self == nil || self->_quitting) return;
      self->_model->manual_restart();
      [self startHelper];
    });
  } else {
    _model->manual_restart();
    [self probeThenStart];
  }
}

- (void)refreshPairingsAction:(id)sender {
  (void)sender;
  [self refreshPairings];
}

- (void)refreshPairings {
  if (_pairingsLoading) return;
  _pairingsLoading = YES;
  __weak SyncAppDelegate* weakSelf = self;
  _process->list_pairings(
      [weakSelf](std::vector<std::string> origins, std::string error) {
        SyncAppDelegate* self = weakSelf;
        if (self == nil) return;
        self->_pairings = std::move(origins);
        self->_pairingError = std::move(error);
        self->_pairingsLoading = NO;
      });
}

- (void)revokePairing:(NSMenuItem*)sender {
  NSString* origin = sender.representedObject;
  if (![origin isKindOfClass:NSString.class]) return;
  NSAlert* alert = [[NSAlert alloc] init];
  alert.alertStyle = NSAlertStyleWarning;
  alert.messageText = @"Revoke this pairing?";
  alert.informativeText = [NSString stringWithFormat:
      @"%@ will need your approval before it can use Sync again.", origin];
  [alert addButtonWithTitle:@"Revoke"];
  [alert addButtonWithTitle:@"Cancel"];
  if ([alert runModal] != NSAlertFirstButtonReturn) return;
  __weak SyncAppDelegate* weakSelf = self;
  _process->revoke_pairing(origin.UTF8String, [weakSelf](bool revoked,
                                                        std::string error) {
    SyncAppDelegate* self = weakSelf;
    if (self == nil) return;
    if (!revoked) {
      show_error(@"Could not revoke pairing", ns_string(error));
    }
    [self refreshPairings];
  });
}

- (void)toggleLaunchAtLogin:(id)sender {
  (void)sender;
  NSError* error = nil;
  SMAppService* service = SMAppService.mainAppService;
  const BOOL enabled = service.status == SMAppServiceStatusEnabled;
  const BOOL changed = enabled ? [service unregisterAndReturnError:&error]
                               : [service registerAndReturnError:&error];
  if (!changed) {
    show_error(@"Could not update Launch at Login",
               error.localizedDescription ?: @"Unknown error");
  }
}

- (void)copyDiagnostics:(id)sender {
  (void)sender;
  NSPasteboard* pasteboard = NSPasteboard.generalPasteboard;
  [pasteboard clearContents];
  [pasteboard setString:ns_string(_model->diagnostics())
                forType:NSPasteboardTypeString];
}

- (void)openGuide:(id)sender {
  (void)sender;
  [NSWorkspace.sharedWorkspace
      openURL:[NSURL URLWithString:@"https://noisedeck.app/docs/Sync"]];
}

- (void)openSource:(id)sender {
  (void)sender;
  [NSWorkspace.sharedWorkspace
      openURL:[NSURL URLWithString:@"https://github.com/noisefactorllc/sync"]];
}

- (void)showAbout:(id)sender {
  (void)sender;
  [NSApp orderFrontStandardAboutPanelWithOptions:@{
    NSAboutPanelOptionApplicationName : @"Sync Preview",
    NSAboutPanelOptionVersion : ns_string(noisefactor::sync::kProductVersion),
    NSAboutPanelOptionCredits : [[NSAttributedString alloc]
        initWithString:@"A public MIT-licensed preview from Noise Factor LLC."]
  }];
}

- (void)quitSync:(id)sender {
  (void)sender;
  [NSApp terminate:self];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
  (void)sender;
  _quitting = YES;
  [_pollTimer invalidate];
  if (!_process->owned_pid().has_value()) return NSTerminateNow;
  _expectedExit = YES;
  _process->terminate([] { [NSApp replyToApplicationShouldTerminate:YES]; });
  return NSTerminateLater;
}

- (void)showPreviewNoticeIfNeeded {
  NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
  if ([defaults boolForKey:@"SyncPreviewNoticeShown"]) return;
  [defaults setBool:YES forKey:@"SyncPreviewNoticeShown"];
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"Sync is a preview";
  alert.informativeText =
      @"Sync is not ready for general use. Expect rough edges, dropped frames, "
       "and protocol changes while we validate the bridge.";
  [alert addButtonWithTitle:@"Continue"];
  [alert addButtonWithTitle:@"Open Guide"];
  if ([alert runModal] == NSAlertSecondButtonReturn) [self openGuide:nil];
}

@end

int main(int argc, const char* argv[]) {
  (void)argc;
  (void)argv;
  @autoreleasepool {
    NSApplication* application = NSApplication.sharedApplication;
    [application setActivationPolicy:NSApplicationActivationPolicyAccessory];
    SyncAppDelegate* delegate = [[SyncAppDelegate alloc] init];
    application.delegate = delegate;
    [application run];
  }
  return 0;
}
