#import <CoreFoundation/CoreFoundation.h>
#import <Foundation/Foundation.h>

#include "pairing_prompt_internal.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

#include <mach/message.h>

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
  constexpr std::string_view header = "Sync pairing request";
  constexpr std::string_view identity = "Security identity: ";
  constexpr std::string_view label = "\nUnverified app label: ";
  constexpr std::string_view question =
      "\n\nAllow this origin to publish through Sync?";
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
    };
    const void* values[] = {header, message, default_button, alternate_button};
    CFDictionaryRef dictionary = CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, 4,
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
          std::make_unique<CoreFoundationAdapter>(),
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
