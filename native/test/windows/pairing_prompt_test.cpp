#include "../test_harness.hpp"

#include <sync/origin.hpp>
#include <sync/pairing.hpp>
#include <sync/platform/pairing_prompt.hpp>

#include "../../src/platform/windows/pairing_prompt_internal.hpp"

#include <windows.h>
// dbghelp.h needs the Windows types above it.
#include <dbghelp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace pairing = noisefactor::sync::pairing;
namespace prompt_test = noisefactor::sync::platform::pairing_prompt_testing;
using namespace std::chrono_literals;

// Most tests use the same Adapter seam the production TaskDialog implements,
// so they need no desktop. The final adapter-level test opens the real dialog
// and closes it programmatically; no human click is required in CI.
enum class AdapterMode {
  Approve,
  Deny,
  Fail,
  // Blocks in show() until force_close() is observed, then returns Denied --
  // the same outcome the real TaskDialog gives when its Deny button is
  // selected programmatically. Stands in for "the dialog is open" in tests.
  BlockUntilClosed,
  // Blocks in show() BEFORE reporting a window, then reports it only once
  // the test allows. Reproduces the real gap between entering TaskDialog and
  // receiving TDN_CREATED -- the window in which a cancel()
  // finds nothing to close.
  BlockBeforeReportingWindow,
  // Blocks in show() until the test explicitly releases it, then approves --
  // used to land a cancel() or a wrong-generation cancel() precisely while a
  // decision is in flight.
  ControlledApprove,
};

struct AdapterRecord {
  explicit AdapterRecord(AdapterMode configured_mode) : mode(configured_mode) {}

  AdapterMode mode;
  std::mutex mutex;
  std::condition_variable condition;
  bool released = false;
  bool closed = false;
  bool may_report_window = false;
  std::atomic<bool> entered_show{false};
  std::vector<std::thread::id> threads;
  std::string title;
  std::string message;
  std::atomic<std::size_t> show_calls{0};
  std::atomic<std::size_t> force_close_calls{0};
};

class FakeAdapter final : public prompt_test::Adapter {
 public:
  explicit FakeAdapter(std::shared_ptr<AdapterRecord> record)
      : record_(std::move(record)) {}

  prompt_test::AdapterResponse show(
      const prompt_test::Presentation& presentation,
      const std::function<void(std::uintptr_t)>& report_window) override {
    {
      std::lock_guard lock(record_->mutex);
      record_->threads.push_back(std::this_thread::get_id());
      record_->title.assign(presentation.title());
      record_->message.assign(presentation.message());
    }
    ++record_->show_calls;
    record_->entered_show.store(true, std::memory_order_release);

    if (record_->mode == AdapterMode::BlockBeforeReportingWindow) {
      // Deliberately do NOT report a window yet.
      {
        std::unique_lock lock(record_->mutex);
        record_->condition.wait(lock, [&] { return record_->may_report_window; });
      }
      report_window(0x1234);
      std::unique_lock lock(record_->mutex);
      record_->condition.wait(lock, [&] { return record_->closed; });
      return prompt_test::AdapterResponse::Denied;
    }

    // A fake but stable non-zero "handle" so force_close() has something to
    // observe; the real adapter reports a genuine HWND the same way.
    report_window(0x1234);

    switch (record_->mode) {
      case AdapterMode::Approve:
        return prompt_test::AdapterResponse::Approved;
      case AdapterMode::Deny:
        return prompt_test::AdapterResponse::Denied;
      case AdapterMode::Fail:
        return prompt_test::AdapterResponse::Failed;
      case AdapterMode::BlockUntilClosed: {
        std::unique_lock lock(record_->mutex);
        record_->condition.wait(lock, [&] { return record_->closed; });
        return prompt_test::AdapterResponse::Denied;
      }
      case AdapterMode::BlockBeforeReportingWindow:
        // Unreachable: handled above, before any window is reported. Listed
        // so -Wswitch stays useful for the next mode added here.
        return prompt_test::AdapterResponse::Denied;
      case AdapterMode::ControlledApprove: {
        std::unique_lock lock(record_->mutex);
        record_->condition.wait(lock, [&] { return record_->released; });
        return prompt_test::AdapterResponse::Approved;
      }
    }
    return prompt_test::AdapterResponse::Failed;
  }

  void force_close(std::uintptr_t window) override {
    (void)window;
    ++record_->force_close_calls;
    std::lock_guard lock(record_->mutex);
    record_->closed = true;
    record_->condition.notify_all();
  }

 private:
  std::shared_ptr<AdapterRecord> record_;
};

pairing::PromptRequest request(std::uint64_t generation,
                               std::string_view origin,
                               std::string_view name) {
  const auto normalized = noisefactor::sync::normalize_origin(origin);
  SYNC_REQUIRE(normalized.ok());
  pairing::PromptRequest result;
  SYNC_REQUIRE(result.assign(generation, normalized.origin, name));
  return result;
}

template <typename Predicate>
bool wait_until(Predicate predicate,
                std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

void release_controlled_response(AdapterRecord& record) {
  {
    std::lock_guard lock(record.mutex);
    record.released = true;
  }
  record.condition.notify_all();
}

pairing::PromptResult wait_for_result(pairing::PairingPrompt& prompt) {
  pairing::PromptResult result;
  SYNC_REQUIRE(wait_until([&] {
    result = prompt.poll();
    return result.available;
  }));
  return result;
}

std::unique_ptr<noisefactor::sync::platform::WindowsPairingPrompt> make_prompt(
    const std::shared_ptr<AdapterRecord>& record,
    std::chrono::milliseconds deadline = 5s) {
  return prompt_test::Factory::create(std::make_unique<FakeAdapter>(record),
                                      deadline);
}

void require_single_worker_thread(AdapterRecord& record) {
  std::lock_guard lock(record.mutex);
  SYNC_REQUIRE(!record.threads.empty());
  SYNC_REQUIRE(record.threads.front() != std::this_thread::get_id());
  for (const auto thread : record.threads) {
    SYNC_REQUIRE(thread == record.threads.front());
  }
}

SYNC_TEST(windows_prompt_approval_copies_security_identity_into_message) {
  auto record = std::make_shared<AdapterRecord>(AdapterMode::Approve);
  auto prompt = make_prompt(record);
  const auto approved = request(41, "https://Example.COM:443", "Nøise Deck");

  SYNC_REQUIRE(prompt->begin(approved));
  const auto result = wait_for_result(*prompt);
  SYNC_REQUIRE(result.generation == 41);
  SYNC_REQUIRE(result.decision == pairing::PromptDecision::Approved);
  {
    std::lock_guard lock(record->mutex);
    SYNC_REQUIRE(record->title == "Sync pairing request");
    SYNC_REQUIRE(record->message ==
                 "Security identity: https://example.com\n"
                 "Unverified app label: Nøise Deck\n\n"
                 "Allow this origin to publish video and capture audio inputs "
                 "through Sync?");
  }
  require_single_worker_thread(*record);
}

SYNC_TEST(windows_prompt_denial_and_failure_deny) {
  for (const auto mode : {AdapterMode::Deny, AdapterMode::Fail}) {
    auto record = std::make_shared<AdapterRecord>(mode);
    auto prompt = make_prompt(record);
    SYNC_REQUIRE(
        prompt->begin(request(77, "https://client.example", "Noisedeck")));
    const auto result = wait_for_result(*prompt);
    SYNC_REQUIRE(result.generation == 77);
    SYNC_REQUIRE(result.decision == pairing::PromptDecision::Denied);
    require_single_worker_thread(*record);
  }
}

SYNC_TEST(windows_prompt_only_one_outstanding_at_a_time) {
  auto record = std::make_shared<AdapterRecord>(AdapterMode::ControlledApprove);
  auto prompt = make_prompt(record);
  const auto first = request(1, "https://client.example", "Noisedeck");
  const auto second = request(2, "https://other.example", "Other");

  SYNC_REQUIRE(prompt->begin(first));
  SYNC_REQUIRE(wait_until([&] { return record->show_calls.load() == 1; }));
  SYNC_REQUIRE(!prompt->begin(second));

  release_controlled_response(*record);
  const auto result = wait_for_result(*prompt);
  SYNC_REQUIRE(result.generation == 1);
  SYNC_REQUIRE(result.decision == pairing::PromptDecision::Approved);
}

SYNC_TEST(windows_prompt_poll_reports_unavailable_before_a_decision) {
  auto record = std::make_shared<AdapterRecord>(AdapterMode::ControlledApprove);
  auto prompt = make_prompt(record);
  SYNC_REQUIRE(
      prompt->begin(request(5, "https://client.example", "Noisedeck")));
  SYNC_REQUIRE(wait_until([&] { return record->show_calls.load() == 1; }));

  // The adapter is deliberately still blocked in show(): poll() must not
  // report a decision that has not actually been produced yet.
  SYNC_REQUIRE(!prompt->poll().available);
  SYNC_REQUIRE(!prompt->poll().available);

  release_controlled_response(*record);
  const auto result = wait_for_result(*prompt);
  SYNC_REQUIRE(result.generation == 5);
}

SYNC_TEST(windows_prompt_cancel_discards_a_late_decision_and_reuses_the_slot) {
  auto record = std::make_shared<AdapterRecord>(AdapterMode::ControlledApprove);
  auto prompt = make_prompt(record);
  SYNC_REQUIRE(
      prompt->begin(request(101, "https://client.example", "Noisedeck")));
  SYNC_REQUIRE(wait_until([&] { return record->show_calls.load() == 1; }));

  prompt->cancel(101);
  // cancel() best-effort force-closes the box; confirm it tried.
  SYNC_REQUIRE(wait_until(
      [&] { return record->force_close_calls.load() >= 1; }));
  // Whatever the (still in-flight) adapter eventually returns must never
  // surface as a decision for the cancelled generation.
  release_controlled_response(*record);
  std::this_thread::sleep_for(20ms);
  SYNC_REQUIRE(!prompt->poll().available);

  const auto second = request(102, "https://client.example", "Noisedeck");
  SYNC_REQUIRE(wait_until([&] { return prompt->begin(second); }));
  const auto result = wait_for_result(*prompt);
  SYNC_REQUIRE(result.generation == 102);
  SYNC_REQUIRE(result.decision == pairing::PromptDecision::Approved);
}

SYNC_TEST(windows_prompt_wrong_generation_cancel_does_not_suppress_result) {
  auto record = std::make_shared<AdapterRecord>(AdapterMode::ControlledApprove);
  auto prompt = make_prompt(record);
  SYNC_REQUIRE(
      prompt->begin(request(121, "https://client.example", "Noisedeck")));
  SYNC_REQUIRE(wait_until([&] { return record->show_calls.load() == 1; }));

  prompt->cancel(122);
  release_controlled_response(*record);
  const auto result = wait_for_result(*prompt);
  SYNC_REQUIRE(result.generation == 121);
  SYNC_REQUIRE(result.decision == pairing::PromptDecision::Approved);
  SYNC_REQUIRE(record->force_close_calls == 0);
}

SYNC_TEST(windows_prompt_unconsumed_result_blocks_begin_and_can_be_canceled) {
  auto record = std::make_shared<AdapterRecord>(AdapterMode::Approve);
  auto prompt = make_prompt(record);
  const auto first = request(111, "https://client.example", "Noisedeck");
  const auto second = request(112, "https://client.example", "Noisedeck");
  SYNC_REQUIRE(prompt->begin(first));
  SYNC_REQUIRE(wait_until([&] { return record->show_calls.load() == 1; }));
  // Give the worker time to reach State::Result without consuming it yet.
  std::this_thread::sleep_for(20ms);
  SYNC_REQUIRE(!prompt->begin(second));
  prompt->cancel(999);
  SYNC_REQUIRE(!prompt->begin(second));
  prompt->cancel(111);
  SYNC_REQUIRE(!prompt->poll().available);
  SYNC_REQUIRE(prompt->begin(second));
  SYNC_REQUIRE(wait_for_result(*prompt).generation == 112);
}

SYNC_TEST(windows_prompt_deadline_forces_a_close_and_reports_timed_out) {
  auto record = std::make_shared<AdapterRecord>(AdapterMode::BlockUntilClosed);
  auto prompt = make_prompt(record, 80ms);
  SYNC_REQUIRE(
      prompt->begin(request(91, "https://client.example", "Noisedeck")));
  const auto result = wait_for_result(*prompt);
  SYNC_REQUIRE(result.generation == 91);
  SYNC_REQUIRE(result.decision == pairing::PromptDecision::TimedOut);
  SYNC_REQUIRE(record->force_close_calls >= 1);
}

SYNC_TEST(windows_prompt_destruction_with_an_outstanding_prompt_is_safe) {
  auto record = std::make_shared<AdapterRecord>(AdapterMode::BlockUntilClosed);
  const auto started = std::chrono::steady_clock::now();
  {
    auto prompt = make_prompt(record, 5s);
    SYNC_REQUIRE(
        prompt->begin(request(131, "https://client.example", "Noisedeck")));
    SYNC_REQUIRE(wait_until([&] { return record->show_calls.load() == 1; }));
    // prompt is destroyed here while the fake adapter is still blocked in
    // show(); the destructor must force-close it and join the worker rather
    // than hang or leave a dangling thread.
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  SYNC_REQUIRE(elapsed < 2s);
  SYNC_REQUIRE(record->force_close_calls >= 1);
  require_single_worker_thread(*record);
}

// Regression test for a liveness bug, not a correctness one.
//
// A real TaskDialog only reveals its HWND once TDN_CREATED fires, slightly
// after show() is entered. A cancel() or a destructor landing in that gap
// used to read active_window == 0, close nothing, and -- because
// cancel_active/stopping were already latched -- never look again. The dialog
// stayed up until a human clicked it, with the destructor's join() blocked
// behind it. The watcher now keeps asking until the window appears or show()
// returns, so destruction stays bounded even when the cancel wins the race.
SYNC_TEST(windows_prompt_destruction_is_bounded_when_cancel_beats_the_window_handle) {
  auto record = std::make_shared<AdapterRecord>(
      AdapterMode::BlockBeforeReportingWindow);
  const auto started = std::chrono::steady_clock::now();
  {
    auto prompt = make_prompt(record, 5s);
    SYNC_REQUIRE(
        prompt->begin(request(141, "https://client.example", "Noisedeck")));
    // Wait until show() is running but has deliberately NOT reported a window.
    SYNC_REQUIRE(wait_until([&] {
      return record->entered_show.load(std::memory_order_acquire);
    }));

    // Let the window appear only after the destructor below has already
    // asked to cancel, so the cancel provably loses the race to it.
    std::thread releaser([record] {
      std::this_thread::sleep_for(120ms);
      std::lock_guard lock(record->mutex);
      record->may_report_window = true;
      record->condition.notify_all();
    });
    releaser.detach();
    // prompt is destroyed here, while show() is blocked with no window yet.
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  SYNC_REQUIRE(elapsed < 3s);
  SYNC_REQUIRE(record->force_close_calls >= 1);
  require_single_worker_thread(*record);
}

// What the real dialog's worker shares with the test. Held by shared_ptr so a
// worker still inside TaskDialogIndirect when the test gives up on it can be
// detached without later writing into a destroyed stack frame.
struct NativeShow {
  std::shared_ptr<prompt_test::Adapter> adapter;
  prompt_test::Presentation presentation;
  std::atomic<std::uintptr_t> window{0};
  std::atomic<bool> finished{false};
  std::atomic<prompt_test::AdapterResponse> response{
      prompt_test::AdapterResponse::Failed};
};

std::string narrow(const wchar_t* wide) {
  const int needed =
      ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) return {};
  std::string result(static_cast<std::size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), needed, nullptr,
                        nullptr);
  result.resize(static_cast<std::size_t>(needed - 1));
  return result;
}

// Lists the windows a thread owns. Only calls that never send a message to
// the owner are used -- that thread is the one suspected of being stuck, and
// GetWindowText would wait on it.
std::string describe_thread_windows(DWORD thread_id) {
  std::string out;
  ::EnumThreadWindows(
      thread_id,
      [](HWND window, LPARAM parameter) -> BOOL {
        auto& text = *reinterpret_cast<std::string*>(parameter);
        wchar_t class_name[128]{};
        wchar_t caption[128]{};
        ::GetClassNameW(window, class_name,
                        static_cast<int>(std::size(class_name)));
        ::InternalGetWindowText(window, caption,
                                static_cast<int>(std::size(caption)));
        text += "\n  window class=\"" + narrow(class_name) + "\" caption=\"" +
                narrow(caption) +
                "\" visible=" + (::IsWindowVisible(window) ? "1" : "0");
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&out));
  return out.empty() ? "\n  (the thread owns no windows)" : out;
}

// The stuck thread's return addresses, walked while it is suspended. Only the
// unwinder runs during the suspension: symbolizing allocates, and the
// suspended thread may hold the heap lock. dbghelp names the frames after
// ResumeThread.
std::string describe_thread_stack(DWORD thread_id) {
#if defined(_M_X64)
  const HANDLE process = ::GetCurrentProcess();
  ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  const bool symbols = ::SymInitialize(process, nullptr, TRUE) != FALSE;

  const HANDLE thread = ::OpenThread(
      THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
      FALSE, thread_id);
  if (thread == nullptr) return "\n  (OpenThread failed)";
  std::array<DWORD64, 64> frames{};
  std::size_t depth = 0;
  if (::SuspendThread(thread) != static_cast<DWORD>(-1)) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    if (::GetThreadContext(thread, &context) != FALSE) {
      while (depth < frames.size() && context.Rip != 0) {
        frames[depth++] = context.Rip;
        DWORD64 image_base = 0;
        const auto function =
            ::RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function == nullptr) {
          // A leaf function: its return address is on top of the stack.
          context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
          context.Rsp += sizeof(DWORD64);
          continue;
        }
        void* handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip,
                           function, &context, &handler_data,
                           &establisher_frame, nullptr);
      }
    }
    ::ResumeThread(thread);
  }
  ::CloseHandle(thread);

  std::string out;
  for (std::size_t index = 0; index < depth; ++index) {
    const DWORD64 address = frames[index];
    std::string frame = "\n  #" + std::to_string(index) + " ";
    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (symbols && ::SymGetModuleInfo64(process, address, &module) != FALSE) {
      frame += module.ModuleName;
      if (::SymFromAddr(process, address, &displacement, symbol) != FALSE) {
        frame += "!" + std::string(symbol->Name) + "+" +
                 std::to_string(displacement);
      } else {
        frame += "+" + std::to_string(address - module.BaseOfImage);
      }
    } else {
      frame += "address " + std::to_string(address);
    }
    out += frame;
  }
  if (symbols) ::SymCleanup(process);
  return out.empty() ? "\n  (no frames captured)" : out;
#else
  (void)thread_id;
  return "\n  (stack capture is implemented for x64 only)";
#endif
}

SYNC_TEST(windows_native_prompt_force_close_is_bounded) {
  auto show = std::make_shared<NativeShow>();
  show->adapter = prompt_test::Factory::create_native_adapter();
  constexpr std::string_view title = "Sync pairing request";
  constexpr std::string_view message =
      "Security identity: https://ci.example\nUnverified app label: CI\n\n"
      "Allow this origin to publish video from this machine through Sync?";
  auto& presentation = show->presentation;
  std::copy(title.begin(), title.end(), presentation.title_bytes.begin());
  presentation.title_length = title.size();
  std::copy(message.begin(), message.end(), presentation.message_bytes.begin());
  presentation.message_length = message.size();

  // Two intervals, bounded separately. Creating the dialog is Windows' work,
  // and on a hosted runner it has a long tail: 2.5 s on 2026-09-21, over 5 s
  // on 2026-09-25, against a usual few tens of milliseconds. Timing it as
  // part of force_close's bound turned that tail into test failures. It gets
  // its own ceiling, which only a dialog that never comes up can reach; a
  // slow one is logged with its thread's stack so the tail can be explained.
  // force_close() to show() returning is the path this test is named for,
  // and is ours, so it keeps a tight bound.
  constexpr auto slow_creation = 5s;
  constexpr auto creation_ceiling = 60s;
  constexpr auto force_close_bound = 2s;

  const auto started = std::chrono::steady_clock::now();
  std::thread worker([show] {
    show->response.store(
        show->adapter->show(show->presentation,
                            [&show](std::uintptr_t reported) {
                              show->window.store(reported,
                                                 std::memory_order_release);
                            }),
        std::memory_order_release);
    show->finished.store(true, std::memory_order_release);
  });
  // Taken from the handle now, not reported by the worker: a thread that has
  // not been scheduled yet must still be nameable, and detach() drops the
  // handle.
  const DWORD thread_id = ::GetThreadId(worker.native_handle());
  const auto window_or_return = [&show] {
    return show->window.load(std::memory_order_acquire) != 0 ||
           show->finished.load(std::memory_order_acquire);
  };

  const auto elapsed_ms = [](std::chrono::steady_clock::time_point since) {
    return std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - since)
            .count());
  };

  if (!wait_until(window_or_return, slow_creation)) {
    std::string report =
        "the native dialog reported no window and had not returned after " +
        elapsed_ms(started) + " ms\nwindows owned by the dialog thread:" +
        describe_thread_windows(thread_id) +
        "\ndialog thread stack:" + describe_thread_stack(thread_id);
    if (!wait_until(window_or_return, creation_ceiling - slow_creation)) {
      // Never std::terminate on a still-joinable worker -- a bare
      // SYNC_REQUIRE here once left only "TERMINATE" in the log. The worker
      // owns what it touches, so it can outlive this frame.
      worker.detach();
      throw std::runtime_error(report + "\nstill nothing after " +
                               elapsed_ms(started) +
                               " ms; the dialog thread is left detached");
    }
    std::cerr << "NOTE slow native dialog creation: " << report
              << "\nit arrived after " << elapsed_ms(started) << " ms"
              << std::endl;
  }

  const auto reported = show->window.load(std::memory_order_acquire);
  const auto closing = std::chrono::steady_clock::now();
  if (reported != 0) show->adapter->force_close(reported);
  if (!wait_until(
          [&show] { return show->finished.load(std::memory_order_acquire); },
          force_close_bound)) {
    worker.detach();
    throw std::runtime_error(
        "show() had not returned " + elapsed_ms(closing) +
        " ms after force_close; dialog thread stack:" +
        describe_thread_stack(thread_id));
  }
  worker.join();

  SYNC_REQUIRE(reported != 0);
  SYNC_REQUIRE(show->response.load(std::memory_order_acquire) ==
               prompt_test::AdapterResponse::Denied);
}

}  // namespace
