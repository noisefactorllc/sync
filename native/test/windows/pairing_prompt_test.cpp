#include "../test_harness.hpp"

#include <sync/origin.hpp>
#include <sync/pairing.hpp>
#include <sync/platform/pairing_prompt.hpp>

#include "../../src/platform/windows/pairing_prompt_internal.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace pairing = noisefactor::sync::pairing;
namespace prompt_test = noisefactor::sync::platform::pairing_prompt_testing;
using namespace std::chrono_literals;

// None of these tests ever show a real MessageBoxW: they go through the
// same Adapter seam (native/src/platform/windows/pairing_prompt_internal.hpp)
// that Win32MessageBoxAdapter implements in production, so nothing here
// requires a human to click anything in CI.
enum class AdapterMode {
  Approve,
  Deny,
  Fail,
  // Blocks in show() until force_close() is observed, then returns Denied --
  // the same outcome a real MB_YESNO box gives when its window is closed via
  // WM_CLOSE. Stands in for "the box is still open" in tests.
  BlockUntilClosed,
  // Blocks in show() BEFORE reporting a window, then reports it only once
  // the test allows. Reproduces the real gap between entering MessageBoxW
  // and the CBT hook delivering its HWND -- the window in which a cancel()
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
                 "Allow this origin to publish video from this machine "
                 "through Sync?\n\n"
                 "Click Yes to allow, No to deny.");
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
// A real MessageBoxW only reveals its HWND once the CBT hook fires, slightly
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

}  // namespace
