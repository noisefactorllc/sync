#include "pairing_prompt_internal.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#if defined(_MSC_VER)
// TaskDialogIndirect is available only under the version 6 common-controls
// activation context. Carry the dependency from this object into every MSVC
// executable that consumes the static library; MinGW gets the equivalent
// manifest resource from CMake.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>

namespace noisefactor::sync::platform {
namespace {

namespace prompt_test = pairing_prompt_testing;
using namespace std::chrono_literals;

// Mirrors native/src/server.cpp's kPairingPromptDeadlineMs override, which
// CMake sets to a much shorter value for a fast, deterministic test build.
// MacPairingPrompt hardcodes its own 25s production deadline instead of
// reading that macro (it gets its short test deadlines through the
// pairing_prompt_testing::Factory seam), but WindowsPairingPrompt's own
// production instance honours it too, so a future test target that links
// this file directly (rather than only through Factory) still gets a short
// deadline instead of a real 25s wait.
#if defined(SYNC_PAIRING_PROMPT_DEADLINE_MS)
static_assert(SYNC_PAIRING_PROMPT_DEADLINE_MS > 0);
constexpr auto kProductionUiDeadline =
    std::chrono::milliseconds(SYNC_PAIRING_PROMPT_DEADLINE_MS);
#else
constexpr auto kProductionUiDeadline = 25s;
#endif

bool append(std::span<char> output, std::size_t& length,
            std::string_view value) noexcept {
  if (value.size() > output.size() - length) return false;
  std::memcpy(output.data() + length, value.data(), value.size());
  length += value.size();
  return true;
}

// Wording matches MacPairingPrompt's build_presentation in tone and security
// emphasis (exact identity, exact requested name, never truncated).
bool build_presentation(const pairing::PromptRequest& request,
                        prompt_test::Presentation& output) noexcept {
  constexpr std::string_view title = "Sync pairing request";
  constexpr std::string_view identity = "Security identity: ";
  constexpr std::string_view label = "\nUnverified app label: ";
  constexpr std::string_view question =
      "\n\nAllow this origin to publish video from this machine through "
      "Sync?";
  return append(output.title_bytes, output.title_length, title) &&
         append(output.message_bytes, output.message_length, identity) &&
         append(output.message_bytes, output.message_length,
                request.origin.view()) &&
         append(output.message_bytes, output.message_length, label) &&
         append(output.message_bytes, output.message_length,
                request.name()) &&
         append(output.message_bytes, output.message_length, question);
}

std::wstring to_wide(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int needed = ::MultiByteToWideChar(
      CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), needed);
  return wide;
}

class Win32TaskDialogAdapter final : public prompt_test::Adapter {
 public:
  prompt_test::AdapterResponse show(
      const prompt_test::Presentation& presentation,
      const std::function<void(std::uintptr_t)>& report_window) override {
    const std::wstring wide_title = to_wide(presentation.title());
    const std::wstring wide_message = to_wide(presentation.message());
    if ((!presentation.title().empty() && wide_title.empty()) ||
        (!presentation.message().empty() && wide_message.empty())) {
      return prompt_test::AdapterResponse::Failed;
    }

    const TASKDIALOG_BUTTON buttons[] = {
        {.nButtonID = IDYES, .pszButtonText = L"&Allow"},
        {.nButtonID = IDNO, .pszButtonText = L"&Deny"},
    };
    CallbackContext context{.adapter = this,
                            .report_window = &report_window};
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = wide_title.c_str();
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = L"Review this pairing request";
    config.pszContent = wide_message.c_str();
    config.cButtons = static_cast<UINT>(std::size(buttons));
    config.pButtons = buttons;
    config.nDefaultButton = IDNO;
    config.pfCallback = &dialog_callback;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&context);

    int response = 0;
    const HRESULT result = ::TaskDialogIndirect(&config, &response, nullptr,
                                                nullptr);
    live_window_.store(0, std::memory_order_release);
    if (FAILED(result)) return prompt_test::AdapterResponse::Failed;
    if (response == IDYES) return prompt_test::AdapterResponse::Approved;
    if (response == IDNO || response == IDCANCEL) {
      return prompt_test::AdapterResponse::Denied;
    }
    return prompt_test::AdapterResponse::Failed;
  }

  void force_close(std::uintptr_t window) override {
    if (window == 0) return;
    // TDM_CLICK_BUTTON is handled by TaskDialog's own loop and makes the
    // blocking call return IDNO. The live-window equality check, cleared by
    // TDN_DESTROYED before the HWND can be reused, prevents a late watcher
    // from posting even this private control message to an unrelated window.
    if (live_window_.load(std::memory_order_acquire) != window) return;
    ::PostMessageW(reinterpret_cast<HWND>(window), TDM_CLICK_BUTTON, IDNO, 0);
  }

 private:
  struct CallbackContext {
    Win32TaskDialogAdapter* adapter = nullptr;
    const std::function<void(std::uintptr_t)>* report_window = nullptr;
  };

  static HRESULT CALLBACK dialog_callback(HWND window, UINT notification,
                                          WPARAM, LPARAM,
                                          LONG_PTR callback_data) noexcept {
    auto* context = reinterpret_cast<CallbackContext*>(callback_data);
    if (context == nullptr || context->adapter == nullptr) return S_OK;
    const auto packed = reinterpret_cast<std::uintptr_t>(window);
    if (notification == TDN_CREATED) {
      context->adapter->live_window_.store(packed,
                                           std::memory_order_release);
      if (context->report_window != nullptr) {
        try {
          (*context->report_window)(packed);
        } catch (...) {
          return E_FAIL;
        }
      }
    } else if (notification == TDN_DESTROYED) {
      std::uintptr_t expected = packed;
      (void)context->adapter->live_window_.compare_exchange_strong(
          expected, 0, std::memory_order_acq_rel);
    }
    return S_OK;
  }

  std::atomic<std::uintptr_t> live_window_{0};
};

}  // namespace

struct WindowsPairingPrompt::Impl {
  enum class State { Idle, Pending, Active, Result };

  Impl(std::unique_ptr<prompt_test::Adapter> configured_adapter,
       std::chrono::milliseconds configured_ui_deadline)
      : adapter(std::move(configured_adapter)),
        ui_deadline(configured_ui_deadline),
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
    condition.notify_all();
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
    std::uintptr_t window_to_close = 0;
    {
      std::lock_guard lock(mutex);
      if (generation == 0) return;
      if (state == State::Pending && request.generation == generation) {
        request = {};
        state = State::Idle;
      } else if (state == State::Active && active_generation == generation) {
        cancel_active = true;
        window_to_close = active_window.load(std::memory_order_acquire);
      } else if (state == State::Result && result.generation == generation) {
        result = {};
        state = State::Idle;
      }
      condition.notify_all();
    }
    // Best-effort and deliberately outside the lock: a Win32 call must never
    // run while this->mutex is held, and cancel() must never block waiting
    // on the dialog thread. The discard guarantee below does not
    // depend on this call succeeding.
    if (window_to_close != 0 && adapter != nullptr) {
      adapter->force_close(window_to_close);
    }
  }

  void shutdown() noexcept {
    std::uintptr_t window_to_close = 0;
    {
      std::lock_guard lock(mutex);
      if (stopping) return;
      stopping = true;
      if (state == State::Pending) {
        request = {};
        state = State::Idle;
      } else if (state == State::Active) {
        cancel_active = true;
        window_to_close = active_window.load(std::memory_order_acquire);
      } else if (state == State::Result) {
        result = {};
        state = State::Idle;
      }
      condition.notify_all();
    }
    if (window_to_close != 0 && adapter != nullptr) {
      adapter->force_close(window_to_close);
    }
    // Ownership: `worker` only ever touches members of *this while running,
    // and join() below does not return until it has left run() for good, so
    // there is no window in which the worker could touch a partially- or
    // fully-destroyed Impl. Detaching instead would require keeping `this`
    // (and the adapter it owns) alive indefinitely past this destructor,
    // which is worse than the bounded wait join() performs here: stopping,
    // cancel_active, and the best-effort force_close above together mean
    // the worker's currently blocked show() call (if any) is either already
    // returning or about to be asked to.
    if (worker.joinable()) worker.join();
  }

  void run() noexcept {
    for (;;) {
      pairing::PromptRequest current;
      {
        std::unique_lock lock(mutex);
        condition.wait(
            lock, [this] { return stopping || state == State::Pending; });
        if (stopping) return;
        current = request;
        request = {};
        state = State::Active;
        active_generation = current.generation;
        cancel_active = false;
        active_window.store(0, std::memory_order_release);
      }

      prompt_test::Presentation presentation;
      const bool presentation_ok = build_presentation(current, presentation);
      const bool created = presentation_ok && adapter != nullptr;
      auto response = prompt_test::AdapterResponse::Failed;
      bool deadline_fired = false;

      if (created) {
        // TaskDialogIndirect has no timeout parameter, so a watcher thread
        // races the blocking show() call below: it is the only thing that can
        // turn ui_deadline into an actual forced close. It also reacts to
        // cancel()/shutdown() immediately rather than waiting for show() to
        // return on its own, the same way MacPairingPrompt's run loop polls
        // cancel_active/stopping between receive() slices.
        std::atomic<bool> show_done{false};
        std::thread watcher([&] {
          std::unique_lock watch_lock(mutex);
          const auto deadline = std::chrono::steady_clock::now() + ui_deadline;
          condition.wait_until(watch_lock, deadline, [&] {
            return show_done.load(std::memory_order_acquire) ||
                   cancel_active || stopping;
          });
          if (show_done.load(std::memory_order_acquire)) return;
          const bool timed_out = !cancel_active && !stopping;
          if (timed_out) deadline_fired = true;

          // The window handle only becomes known when TaskDialog reports
          // TDN_CREATED, which is slightly AFTER show() is entered. A cancel or
          // shutdown() landing in that gap reads active_window == 0, closes
          // nothing, and -- because cancel_active/stopping are already
          // latched -- never gets another wake-up. The dialog would then
          // stay on screen until a human clicked it, with shutdown()'s
          // join() blocked behind it. So keep asking rather than looking
          // once: poll until the window appears or show() returns on its
          // own. This cannot outlive show(), which run() joins this thread
          // against, so it adds no hang that was not already there.
          constexpr auto kWindowPollInterval = std::chrono::milliseconds(10);
          for (;;) {
            const std::uintptr_t window =
                active_window.load(std::memory_order_acquire);
            if (window != 0 && adapter != nullptr) {
              watch_lock.unlock();
              adapter->force_close(window);
              return;
            }
            if (condition.wait_for(watch_lock, kWindowPollInterval, [&] {
                  return show_done.load(std::memory_order_acquire);
                })) {
              return;
            }
          }
        });

        response = adapter->show(presentation, [this](std::uintptr_t window) {
          active_window.store(window, std::memory_order_release);
        });

        {
          std::lock_guard lock(mutex);
          show_done.store(true, std::memory_order_release);
        }
        condition.notify_all();
        watcher.join();
      }

      pairing::PromptDecision decision = pairing::PromptDecision::Denied;
      if (deadline_fired) {
        decision = pairing::PromptDecision::TimedOut;
      } else if (response == prompt_test::AdapterResponse::Approved) {
        decision = pairing::PromptDecision::Approved;
      }

      {
        std::lock_guard lock(mutex);
        // A generation the caller cancelled (or a shutdown in progress)
        // never surfaces a decision, however the dialog actually
        // resolved -- this is the guarantee that holds independently of
        // whether force_close managed to close a real window.
        const bool suppressed = stopping || cancel_active;
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
        active_window.store(0, std::memory_order_release);
      }
      condition.notify_all();
    }
  }

  std::unique_ptr<prompt_test::Adapter> adapter;
  std::chrono::milliseconds ui_deadline;
  std::mutex mutex;
  std::condition_variable condition;
  pairing::PromptRequest request{};
  pairing::PromptResult result{};
  State state = State::Idle;
  std::uint64_t active_generation = 0;
  std::atomic<std::uintptr_t> active_window{0};
  bool cancel_active = false;
  bool stopping = false;
  std::thread worker;
};

WindowsPairingPrompt::WindowsPairingPrompt()
    : WindowsPairingPrompt(std::make_unique<Impl>(
          std::make_unique<Win32TaskDialogAdapter>(),
          std::chrono::duration_cast<std::chrono::milliseconds>(
              kProductionUiDeadline))) {}

WindowsPairingPrompt::WindowsPairingPrompt(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

WindowsPairingPrompt::~WindowsPairingPrompt() noexcept = default;

bool WindowsPairingPrompt::begin(
    const pairing::PromptRequest& request) noexcept {
  return impl_ != nullptr && impl_->begin(request);
}

pairing::PromptResult WindowsPairingPrompt::poll() noexcept {
  return impl_ == nullptr ? pairing::PromptResult{} : impl_->poll();
}

void WindowsPairingPrompt::cancel(std::uint64_t generation) noexcept {
  if (impl_ != nullptr) impl_->cancel(generation);
}

namespace pairing_prompt_testing {

std::unique_ptr<WindowsPairingPrompt> Factory::create(
    std::unique_ptr<Adapter> adapter, std::chrono::milliseconds ui_deadline) {
  if (adapter == nullptr || ui_deadline <= 0ms) return nullptr;
  return std::unique_ptr<WindowsPairingPrompt>(new WindowsPairingPrompt(
      std::make_unique<WindowsPairingPrompt::Impl>(std::move(adapter),
                                                    ui_deadline)));
}

std::unique_ptr<Adapter> Factory::create_native_adapter() {
  return std::make_unique<Win32TaskDialogAdapter>();
}

}  // namespace pairing_prompt_testing
}  // namespace noisefactor::sync::platform
