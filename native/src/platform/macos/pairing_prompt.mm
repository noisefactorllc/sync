#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>

#include "pairing_prompt_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include <mach/message.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

namespace noisefactor::sync::platform {
namespace {

namespace prompt_test = pairing_prompt_testing;
using namespace std::chrono_literals;

constexpr auto kProductionUiDeadline = 25s;
constexpr auto kProductionReceiveSlice = 75ms;

bool append(std::span<char> output,
            std::size_t& length,
            std::string_view value) noexcept {
  if (value.size() > output.size() - length) return false;
  std::memcpy(output.data() + length, value.data(), value.size());
  length += value.size();
  return true;
}

bool build_presentation(const pairing::PromptRequest& request,
                        prompt_test::Presentation& output) noexcept {
  output.generation = request.generation;
  constexpr std::string_view header = "Sync pairing request";
  constexpr std::string_view identity = "Security identity: ";
  constexpr std::string_view label = "\nUnverified app label: ";
  constexpr std::string_view question =
      "\n\nAllow this origin to publish video and capture audio inputs through Sync?";
  constexpr std::string_view deny = "Deny";
  constexpr std::string_view allow = "Allow";
  return append(output.header_bytes, output.header_length, header) &&
         append(output.message_bytes, output.message_length, identity) &&
         append(output.message_bytes, output.message_length,
                request.origin.view()) &&
         append(output.message_bytes, output.message_length, label) &&
         append(output.message_bytes, output.message_length, request.name()) &&
         append(output.message_bytes, output.message_length, question) &&
         append(output.default_button_bytes, output.default_button_length,
                deny) &&
         append(output.alternate_button_bytes,
                output.alternate_button_length, allow);
}

class CoreFoundationAdapter final : public prompt_test::Adapter {
 public:
  bool create(const prompt_test::Presentation& presentation,
              std::chrono::milliseconds ui_deadline) override {
    if (notification_ != nullptr) return false;
    CFStringRef header = make_string(presentation.header());
    CFStringRef message = make_string(presentation.message());
    CFStringRef default_button = make_string(presentation.default_button());
    CFStringRef alternate_button =
        make_string(presentation.alternate_button());
    if (header == nullptr || message == nullptr || default_button == nullptr ||
        alternate_button == nullptr) {
      release_value(header);
      release_value(message);
      release_value(default_button);
      release_value(alternate_button);
      return false;
    }
    const void* keys[] = {
        kCFUserNotificationAlertHeaderKey,
        kCFUserNotificationAlertMessageKey,
        kCFUserNotificationDefaultButtonTitleKey,
        kCFUserNotificationAlternateButtonTitleKey,
        kCFUserNotificationAlertTopMostKey,
    };
    const void* values[] = {
        header, message, default_button, alternate_button, kCFBooleanTrue};
    CFDictionaryRef dictionary = CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, 5,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    SInt32 error = 0;
    if (dictionary != nullptr) {
      const CFTimeInterval seconds =
          std::chrono::duration<double>(ui_deadline).count();
      notification_ = CFUserNotificationCreate(
          kCFAllocatorDefault, seconds,
          presentation.caution ? kCFUserNotificationCautionAlertLevel
                               : kCFUserNotificationPlainAlertLevel,
          &error, dictionary);
    }
    release_value(dictionary);
    release_value(header);
    release_value(message);
    release_value(default_button);
    release_value(alternate_button);
    if (error != 0 || notification_ == nullptr) {
      release();
      return false;
    }
    return true;
  }

  prompt_test::AdapterResponse
  receive(std::chrono::milliseconds slice) override {
    if (notification_ == nullptr) return prompt_test::AdapterResponse::Failed;
    CFOptionFlags flags = 0;
    const CFTimeInterval seconds = std::chrono::duration<double>(slice).count();
    const SInt32 status =
        CFUserNotificationReceiveResponse(notification_, seconds, &flags);
    return prompt_test::decode_cf_response(
        status, static_cast<std::uint64_t>(flags));
  }

  void cancel() override {
    if (notification_ != nullptr) {
      (void)CFUserNotificationCancel(notification_);
    }
  }

  void release() override {
    if (notification_ != nullptr) {
      CFRelease(notification_);
      notification_ = nullptr;
    }
  }

 private:
  static CFStringRef make_string(std::string_view value) {
    return CFStringCreateWithBytes(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(value.data()),
        static_cast<CFIndex>(value.size()), kCFStringEncodingUTF8, false);
  }

  static void release_value(CFTypeRef value) {
    if (value != nullptr) CFRelease(value);
  }

  CFUserNotificationRef notification_ = nullptr;
};

// The helper's prompt worker owns this adapter. Neither pipe IO nor human
// interaction runs on the helper's libuv/platform-event-pump thread.
class ParentPipeAdapter final : public prompt_test::Adapter {
 public:
  explicit ParentPipeAdapter(std::string session) : session_(std::move(session)) {
    struct stat input{}, output{};
    usable_ = session_.size() == 36 &&
              ::fstat(STDIN_FILENO, &input) == 0 && S_ISFIFO(input.st_mode) &&
              ::fstat(STDOUT_FILENO, &output) == 0 && S_ISFIFO(output.st_mode);
    for (const int descriptor : {STDIN_FILENO, STDOUT_FILENO}) {
      const int flags = ::fcntl(descriptor, F_GETFL);
      if (!usable_ || flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0 ||
          ::fcntl(descriptor, F_SETNOSIGPIPE, 1) != 0) usable_ = false;
    }
  }

  bool create(const prompt_test::Presentation& presentation,
              std::chrono::milliseconds ui_deadline) override {
    if (!usable_ || generation_ != 0 || presentation.generation == 0) return false;
    generation_ = presentation.generation;
    const auto deadline = std::chrono::duration_cast<std::chrono::milliseconds>(
        (std::chrono::steady_clock::now() + ui_deadline).time_since_epoch()).count();
    return send(@{ @"type": @"pairingRequest", @"session": string(session_),
                   @"generation": string(std::to_string(generation_)),
                   @"deadlineMs": @(deadline), @"header": string(presentation.header()),
                   @"message": string(presentation.message()) });
  }

  prompt_test::AdapterResponse receive(std::chrono::milliseconds slice) override {
    using Response = prompt_test::AdapterResponse;
    if (!usable_) return Response::Failed;
    const auto deadline = std::chrono::steady_clock::now() + slice;
    for (;;) {
      const auto newline = pending_.find('\n');
      if (newline != std::string::npos) {
        const auto line = pending_.substr(0, newline);
        pending_.erase(0, newline + 1);
        NSData* bytes = [NSData dataWithBytes:line.data() length:line.size()];
        id decoded = [NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil];
        if (![decoded isKindOfClass:NSDictionary.class]) return fail();
        NSDictionary* record = decoded;
        NSSet* keys = [NSSet setWithArray:record.allKeys];
        if (![keys isEqualToSet:[NSSet setWithArray:@[@"type", @"session", @"generation", @"decision"]]] ||
            ![record[@"type"] isEqual:@"pairingDecision"] ||
            ![record[@"session"] isKindOfClass:NSString.class] ||
            ![record[@"generation"] isKindOfClass:NSString.class] ||
            ![record[@"decision"] isKindOfClass:NSString.class]) return fail();
        const bool allow = [record[@"decision"] isEqual:@"allow"];
        if (!allow && ![record[@"decision"] isEqual:@"deny"]) return fail();
        // An already-cancelled generation or an old app instance cannot approve
        // the new request. Continue waiting within this bounded receive slice.
        if ([record[@"session"] isEqual:string(session_)] &&
            [record[@"generation"] isEqual:string(std::to_string(generation_))])
          return allow ? Response::Approved : Response::Denied;
      } else {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return Response::SliceTimedOut;
        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
        if (ready < 0) { if (errno == EINTR) continue; return fail(); }
        if (ready == 0) return Response::SliceTimedOut;
        char bytes[512];
        const auto count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) return fail();
        pending_.append(bytes, static_cast<std::size_t>(count));
        if (pending_.size() > prompt_test::kMaximumParentPairingFrameBytes) return fail();
      }
      if (std::chrono::steady_clock::now() >= deadline) return Response::SliceTimedOut;
    }
  }

  void cancel() override { release(); }
  void release() override {
    if (generation_ == 0) return;
    // A broken/malformed decision input must still dismiss the parent window
    // when the independently owned output pipe remains writable.
    (void)send(@{ @"type": @"pairingCancel", @"session": string(session_),
                 @"generation": string(std::to_string(generation_)) });
    generation_ = 0;
  }

 private:
  static NSString* string(std::string_view value) {
    return [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
  }
  prompt_test::AdapterResponse fail() {
    usable_ = false;
    return prompt_test::AdapterResponse::Failed;
  }
  bool send(NSDictionary* record) {
    NSData* bytes = [NSJSONSerialization dataWithJSONObject:record options:0 error:nil];
    if (bytes == nil || bytes.length + 1 > prompt_test::kMaximumParentPairingFrameBytes) {
      usable_ = false;
      return false;
    }
    std::string frame(static_cast<const char*>(bytes.bytes), bytes.length);
    frame += '\n';
    const auto deadline = std::chrono::steady_clock::now() + kProductionReceiveSlice;
    std::size_t offset = 0;
    while (offset < frame.size()) {
      if (std::chrono::steady_clock::now() >= deadline) { usable_ = false; return false; }
      const auto count = ::write(STDOUT_FILENO, frame.data() + offset, frame.size() - offset);
      if (count > 0) { offset += static_cast<std::size_t>(count); continue; }
      if (count < 0 && errno == EINTR) continue;
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
      if (count < 0 && errno == EAGAIN && remaining > 0) {
        pollfd descriptor{STDOUT_FILENO, POLLOUT, 0};
        if (::poll(&descriptor, 1, static_cast<int>(remaining)) > 0) continue;
      }
      usable_ = false;
      return false;
    }
    return true;
  }
  std::string session_;
  std::string pending_;
  std::uint64_t generation_ = 0;
  bool usable_ = false;
};

std::unique_ptr<prompt_test::Adapter> production_adapter() {
  const char* session = std::getenv(prompt_test::kParentPairingSessionEnvironment);
  if (session != nullptr) return std::make_unique<ParentPipeAdapter>(session);
  return std::make_unique<CoreFoundationAdapter>();
}

template <typename Operation>
bool invoke_bool(Operation&& operation) noexcept {
  bool result = false;
  @autoreleasepool {
    @try {
      try {
        result = operation();
      } catch (...) {
        result = false;
      }
    } @catch (id exception) {
      (void)exception;
      result = false;
    }
  }
  return result;
}

template <typename Operation>
prompt_test::AdapterResponse invoke_response(Operation&& operation) noexcept {
  auto result = prompt_test::AdapterResponse::Failed;
  @autoreleasepool {
    @try {
      try {
        result = operation();
      } catch (...) {
        result = prompt_test::AdapterResponse::Failed;
      }
    } @catch (id exception) {
      (void)exception;
      result = prompt_test::AdapterResponse::Failed;
    }
  }
  return result;
}

template <typename Operation>
void invoke_void(Operation&& operation) noexcept {
  @autoreleasepool {
    @try {
      try {
        operation();
      } catch (...) {
      }
    } @catch (id exception) {
      (void)exception;
    }
  }
}

}  // namespace

struct MacPairingPrompt::Impl {
  enum class State { Idle, Pending, Active, Result };

  Impl(std::unique_ptr<prompt_test::Adapter> configured_adapter,
       std::chrono::milliseconds configured_ui_deadline,
       std::chrono::milliseconds configured_receive_slice)
      : adapter(std::move(configured_adapter)),
        ui_deadline(configured_ui_deadline),
        receive_slice(configured_receive_slice),
        worker([this] { run(); }) {}

  ~Impl() noexcept { shutdown(); }

  bool begin(const pairing::PromptRequest& next) noexcept {
    std::lock_guard lock(mutex);
    if (stopping || state != State::Idle || next.generation == 0 ||
        next.origin.empty() || next.name().empty()) {
      return false;
    }
    request = next;
    state = State::Pending;
    condition.notify_one();
    return true;
  }

  pairing::PromptResult poll() noexcept {
    std::lock_guard lock(mutex);
    if (state != State::Result) return {};
    const pairing::PromptResult current = result;
    result = {};
    state = State::Idle;
    return current;
  }

  void cancel(std::uint64_t generation) noexcept {
    std::lock_guard lock(mutex);
    if (generation == 0) return;
    if (state == State::Pending && request.generation == generation) {
      request = {};
      state = State::Idle;
    } else if (state == State::Active && active_generation == generation) {
      cancel_active = true;
    } else if (state == State::Result && result.generation == generation) {
      result = {};
      state = State::Idle;
    }
    condition.notify_one();
  }

  void shutdown() noexcept {
    {
      std::lock_guard lock(mutex);
      if (stopping) return;
      stopping = true;
      if (state == State::Pending) {
        request = {};
        state = State::Idle;
      } else if (state == State::Active) {
        cancel_active = true;
      } else if (state == State::Result) {
        result = {};
        state = State::Idle;
      }
      condition.notify_one();
    }
    if (worker.joinable()) worker.join();
  }

  void run() noexcept {
    for (;;) {
      pairing::PromptRequest current;
      {
        std::unique_lock lock(mutex);
        condition.wait(lock,
                       [this] { return stopping || state == State::Pending; });
        if (stopping) return;
        current = request;
        request = {};
        state = State::Active;
        active_generation = current.generation;
        cancel_active = false;
      }

      prompt_test::Presentation presentation;
      const bool presentation_ok = build_presentation(current, presentation);
      const bool created = presentation_ok && adapter != nullptr &&
                           invoke_bool([&] {
                             return adapter->create(presentation, ui_deadline);
                           });
      auto decision = pairing::PromptDecision::Denied;
      bool terminal = !created;
      bool should_cancel = false;
      const auto deadline = std::chrono::steady_clock::now() + ui_deadline;

      while (!terminal) {
        {
          std::lock_guard lock(mutex);
          if (stopping || cancel_active) {
            should_cancel = true;
            break;
          }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          decision = pairing::PromptDecision::TimedOut;
          should_cancel = true;
          terminal = true;
          break;
        }
        const auto response = invoke_response(
            [&] { return adapter->receive(receive_slice); });
        {
          std::lock_guard lock(mutex);
          if (stopping || cancel_active) {
            should_cancel = true;
            break;
          }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          decision = pairing::PromptDecision::TimedOut;
          should_cancel = true;
          terminal = true;
          break;
        }
        switch (response) {
          case prompt_test::AdapterResponse::Approved:
            decision = pairing::PromptDecision::Approved;
            terminal = true;
            break;
          case prompt_test::AdapterResponse::Denied:
          case prompt_test::AdapterResponse::Failed:
            decision = pairing::PromptDecision::Denied;
            terminal = true;
            break;
          case prompt_test::AdapterResponse::SliceTimedOut:
            if (std::chrono::steady_clock::now() >= deadline) {
              decision = pairing::PromptDecision::TimedOut;
              should_cancel = true;
              terminal = true;
            }
            break;
        }
      }

      if (should_cancel && created) {
        invoke_void([&] { adapter->cancel(); });
      }
      if (adapter != nullptr) {
        invoke_void([&] { adapter->release(); });
      }

      {
        std::lock_guard lock(mutex);
        const bool suppressed = stopping || cancel_active || !terminal;
        if (state == State::Active && active_generation == current.generation) {
          if (suppressed) {
            state = State::Idle;
          } else {
            result = {.available = true,
                      .generation = current.generation,
                      .decision = decision};
            state = State::Result;
          }
        }
        active_generation = 0;
        cancel_active = false;
      }
      condition.notify_one();
    }
  }

  std::unique_ptr<prompt_test::Adapter> adapter;
  std::chrono::milliseconds ui_deadline;
  std::chrono::milliseconds receive_slice;
  std::mutex mutex;
  std::condition_variable condition;
  pairing::PromptRequest request{};
  pairing::PromptResult result{};
  State state = State::Idle;
  std::uint64_t active_generation = 0;
  bool cancel_active = false;
  bool stopping = false;
  std::thread worker;
};

MacPairingPrompt::MacPairingPrompt()
    : MacPairingPrompt(std::make_unique<Impl>(
          production_adapter(),
          std::chrono::duration_cast<std::chrono::milliseconds>(
              kProductionUiDeadline),
          kProductionReceiveSlice)) {}

MacPairingPrompt::MacPairingPrompt(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

MacPairingPrompt::~MacPairingPrompt() noexcept = default;

bool MacPairingPrompt::begin(
    const pairing::PromptRequest& request) noexcept {
  return impl_ != nullptr && impl_->begin(request);
}

pairing::PromptResult MacPairingPrompt::poll() noexcept {
  return impl_ == nullptr ? pairing::PromptResult{} : impl_->poll();
}

void MacPairingPrompt::cancel(std::uint64_t generation) noexcept {
  if (impl_ != nullptr) impl_->cancel(generation);
}

namespace pairing_prompt_testing {

AdapterResponse decode_cf_response(std::int32_t status,
                                   std::uint64_t response_flags) noexcept {
  if (status == MACH_RCV_TIMED_OUT) return AdapterResponse::SliceTimedOut;
  if (status != 0) return AdapterResponse::Failed;
  constexpr std::uint64_t response_mask = 0x3U;
  return (response_flags & response_mask) ==
                 kCFUserNotificationAlternateResponse
             ? AdapterResponse::Approved
             : AdapterResponse::Denied;
}

std::unique_ptr<MacPairingPrompt> Factory::create(
    std::unique_ptr<Adapter> adapter,
    std::chrono::milliseconds ui_deadline,
    std::chrono::milliseconds receive_slice) {
  if (adapter == nullptr || ui_deadline <= 0ms || receive_slice < 50ms ||
      receive_slice > 100ms) {
    return nullptr;
  }
  return std::unique_ptr<MacPairingPrompt>(new MacPairingPrompt(
      std::make_unique<MacPairingPrompt::Impl>(
          std::move(adapter), ui_deadline, receive_slice)));
}

}  // namespace pairing_prompt_testing
}  // namespace noisefactor::sync::platform
