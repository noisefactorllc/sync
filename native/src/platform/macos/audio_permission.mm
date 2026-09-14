#import <AVFoundation/AVFoundation.h>

#include <chrono>
#include <future>
#include <memory>

namespace noisefactor::sync::audio {
// Called on the audio worker. The server's main loop continues pumping macOS
// events while the system presents its microphone permission dialog.
bool request_audio_permission() {
  const auto status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
  if (status == AVAuthorizationStatusAuthorized) return true;
  if (status != AVAuthorizationStatusNotDetermined) return false;
  auto answer = std::make_shared<std::promise<bool>>();
  auto ready = answer->get_future();
  [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
    answer->set_value(granted == YES);
  }];
  return ready.wait_for(std::chrono::seconds(55)) == std::future_status::ready && ready.get();
}
} // namespace noisefactor::sync::audio
