#import <Foundation/Foundation.h>

#include "../test_harness.hpp"

#include <sync/origin.hpp>
#include <sync/pairing.hpp>
#include <sync/platform/pairing_prompt.hpp>

#include "../../src/platform/macos/pairing_prompt_internal.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <mach/message.h>

namespace {

namespace pairing = noisefactor::sync::pairing;
namespace prompt_test =
    noisefactor::sync::platform::pairing_prompt_testing;
using namespace std::chrono_literals;

enum class AdapterMode {
  Approve,
  Deny,
  TimeOut,
  Fail,
  SlowApprove,
  SlowDeny,
  ControlledApprove,
  ThrowCppCreate,
  ThrowObjcCreate,
  ThrowCppReceive,
  ThrowObjcReceive,
};

struct AdapterRecord {
  explicit AdapterRecord(AdapterMode configured_mode) : mode(configured_mode) {}

  void record_thread() {
    std::lock_guard lock(mutex);
    threads.push_back(std::this_thread::get_id());
  }

  AdapterMode mode;
  std::mutex mutex;
  std::condition_variable response_condition;
  std::vector<std::thread::id> threads;
  std::string header;
  std::string message;
  std::string default_button;
  std::string alternate_button;
  std::chrono::milliseconds ui_deadline{};
  bool caution = false;
  bool response_released = false;
  std::atomic<std::size_t> create_calls{0};
  std::atomic<std::size_t> receive_calls{0};
  std::atomic<std::size_t> cancel_calls{0};
  std::atomic<std::size_t> release_calls{0};
  std::atomic<std::size_t> objc_allocations{0};
};

class RecordingAdapter final : public prompt_test::Adapter {
 public:
  explicit RecordingAdapter(std::shared_ptr<AdapterRecord> record)
      : record_(std::move(record)) {}

  bool create(const prompt_test::Presentation& presentation,
              std::chrono::milliseconds ui_deadline) override {
    record_->record_thread();
    ++record_->create_calls;
    NSString* marker = [NSString stringWithFormat:@"create-%zu",
                                                  presentation.header().size()];
    record_->objc_allocations.fetch_add(marker.length, std::memory_order_relaxed);
    {
      std::lock_guard lock(record_->mutex);
      record_->header.assign(presentation.header());
      record_->message.assign(presentation.message());
      record_->default_button.assign(presentation.default_button());
      record_->alternate_button.assign(presentation.alternate_button());
      record_->ui_deadline = ui_deadline;
      record_->caution = presentation.caution;
    }
    if (record_->mode == AdapterMode::ThrowCppCreate) {
      throw std::runtime_error("deterministic C++ create failure");
    }
    if (record_->mode == AdapterMode::ThrowObjcCreate) {
      @throw [NSException exceptionWithName:@"SyncPromptCreateFailure"
                                     reason:@"deterministic Objective-C failure"
                                   userInfo:nil];
    }
    return true;
  }

  prompt_test::AdapterResponse
  receive(std::chrono::milliseconds slice) override {
    record_->record_thread();
    ++record_->receive_calls;
    NSString* marker = [NSString stringWithFormat:@"receive-%lld",
                                                  static_cast<long long>(slice.count())];
    record_->objc_allocations.fetch_add(marker.length, std::memory_order_relaxed);
    switch (record_->mode) {
      case AdapterMode::Approve:
        return prompt_test::AdapterResponse::Approved;
      case AdapterMode::Deny:
        return prompt_test::AdapterResponse::Denied;
      case AdapterMode::TimeOut:
        std::this_thread::sleep_for(slice);
        return prompt_test::AdapterResponse::SliceTimedOut;
      case AdapterMode::Fail:
        return prompt_test::AdapterResponse::Failed;
      case AdapterMode::SlowApprove:
        std::this_thread::sleep_for(70ms);
        return prompt_test::AdapterResponse::Approved;
      case AdapterMode::SlowDeny:
        std::this_thread::sleep_for(70ms);
        return prompt_test::AdapterResponse::Denied;
      case AdapterMode::ControlledApprove: {
        std::unique_lock lock(record_->mutex);
        const bool released = record_->response_condition.wait_for(
            lock, 5s, [&] { return record_->response_released; });
        return released ? prompt_test::AdapterResponse::Approved
                        : prompt_test::AdapterResponse::Failed;
      }
      case AdapterMode::ThrowCppReceive:
        throw std::runtime_error("deterministic C++ receive failure");
      case AdapterMode::ThrowObjcReceive:
        @throw [NSException exceptionWithName:@"SyncPromptReceiveFailure"
                                       reason:@"deterministic Objective-C failure"
                                     userInfo:nil];
      case AdapterMode::ThrowCppCreate:
      case AdapterMode::ThrowObjcCreate:
        return prompt_test::AdapterResponse::Failed;
    }
  }

  void cancel() override {
    record_->record_thread();
    ++record_->cancel_calls;
    NSString* marker = [NSString stringWithFormat:@"cancel"];
    record_->objc_allocations.fetch_add(marker.length, std::memory_order_relaxed);
  }

  void release() override {
    record_->record_thread();
    ++record_->release_calls;
    NSString* marker = [NSString stringWithFormat:@"release"];
    record_->objc_allocations.fetch_add(marker.length, std::memory_order_relaxed);
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
    record.response_released = true;
  }
  record.response_condition.notify_all();
}

pairing::PromptResult wait_for_result(pairing::PairingPrompt& prompt) {
  pairing::PromptResult result;
  SYNC_REQUIRE(wait_until([&] {
    result = prompt.poll();
    return result.available;
  }));
  return result;
}

std::unique_ptr<noisefactor::sync::platform::MacPairingPrompt>
make_prompt(const std::shared_ptr<AdapterRecord>& record,
            std::chrono::milliseconds deadline = 200ms,
            std::chrono::milliseconds slice = 50ms) {
  return prompt_test::Factory::create(
      std::make_unique<RecordingAdapter>(record), deadline, slice);
}

void require_one_worker_thread(AdapterRecord& record) {
  std::lock_guard lock(record.mutex);
  SYNC_REQUIRE(!record.threads.empty());
  SYNC_REQUIRE(record.threads.front() != std::this_thread::get_id());
  for (const auto thread : record.threads) {
    SYNC_REQUIRE(thread == record.threads.front());
  }
}

SYNC_TEST(mac_prompt_approval_copies_security_identity_and_uses_deny_default) {
  auto record = std::make_shared<AdapterRecord>(AdapterMode::Approve);
  auto prompt = make_prompt(record);
  const auto first = request(41, "https://Example.COM:443", "Nøise Deck");
  const auto duplicate = request(42, "https://other.example", "Other");

  SYNC_REQUIRE(prompt->begin(first));
  SYNC_REQUIRE(!prompt->begin(duplicate));
  const auto result = wait_for_result(*prompt);
  SYNC_REQUIRE(result.generation == 41);
  SYNC_REQUIRE(result.decision == pairing::PromptDecision::Approved);
  SYNC_REQUIRE(record->create_calls == 1);
  SYNC_REQUIRE(record->receive_calls == 1);
  SYNC_REQUIRE(record->cancel_calls == 0);
  SYNC_REQUIRE(record->release_calls == 1);
  {
    std::lock_guard lock(record->mutex);
    SYNC_REQUIRE(record->header == "Sync pairing request");
    SYNC_REQUIRE(record->message ==
                 "Security identity: https://example.com\n"
                 "Unverified app label: Nøise Deck\n\n"
                 "Allow this origin to publish through Sync?");
    SYNC_REQUIRE(record->default_button == "Deny");
    SYNC_REQUIRE(record->alternate_button == "Allow");
    SYNC_REQUIRE(record->ui_deadline == 200ms);
    SYNC_REQUIRE(record->caution);
  }
  require_one_worker_thread(*record);
  SYNC_REQUIRE(record->objc_allocations > 0);
}

SYNC_TEST(mac_prompt_cf_response_mapping_defaults_every_non_allow_path_to_denial) {
  SYNC_REQUIRE(prompt_test::decode_cf_response(0, 1) ==
               prompt_test::AdapterResponse::Approved);
  SYNC_REQUIRE(prompt_test::decode_cf_response(0, 0) ==
               prompt_test::AdapterResponse::Denied);
  SYNC_REQUIRE(prompt_test::decode_cf_response(0, 2) ==
               prompt_test::AdapterResponse::Denied);
  SYNC_REQUIRE(prompt_test::decode_cf_response(0, 3) ==
               prompt_test::AdapterResponse::Denied);
  SYNC_REQUIRE(prompt_test::decode_cf_response(MACH_RCV_TIMED_OUT, 0) ==
               prompt_test::AdapterResponse::SliceTimedOut);
  SYNC_REQUIRE(prompt_test::decode_cf_response(17, 1) ==
               prompt_test::AdapterResponse::Failed);
}

SYNC_TEST(mac_prompt_denial_and_failure_are_generation_exact_and_fail_closed) {
  for (const auto mode : {AdapterMode::Deny, AdapterMode::Fail,
                          AdapterMode::ThrowCppCreate,
                          AdapterMode::ThrowObjcCreate,
                          AdapterMode::ThrowCppReceive,
                          AdapterMode::ThrowObjcReceive}) {
    auto record = std::make_shared<AdapterRecord>(mode);
    auto prompt = make_prompt(record);
    SYNC_REQUIRE(prompt->begin(request(77, "https://client.example", "Noisedeck")));
    const auto result = wait_for_result(*prompt);
    SYNC_REQUIRE(result.generation == 77);
    SYNC_REQUIRE(result.decision == pairing::PromptDecision::Denied);
    SYNC_REQUIRE(record->release_calls == 1);
    require_one_worker_thread(*record);
  }
}

SYNC_TEST(mac_prompt_worker_deadline_reports_timed_out) {
  auto record = std::make_shared<AdapterRecord>(AdapterMode::TimeOut);
  auto prompt = make_prompt(record, 110ms, 50ms);
  SYNC_REQUIRE(prompt->begin(request(91, "https://client.example", "Noisedeck")));
  const auto result = wait_for_result(*prompt);
  SYNC_REQUIRE(result.generation == 91);
  SYNC_REQUIRE(result.decision == pairing::PromptDecision::TimedOut);
  SYNC_REQUIRE(record->cancel_calls == 1);
  SYNC_REQUIRE(record->release_calls == 1);
  require_one_worker_thread(*record);
}

SYNC_TEST(mac_prompt_response_at_or_after_worker_deadline_is_timed_out) {
  for (const auto mode : {AdapterMode::SlowApprove, AdapterMode::SlowDeny}) {
    auto record = std::make_shared<AdapterRecord>(mode);
    auto prompt = make_prompt(record, 50ms, 50ms);
    SYNC_REQUIRE(prompt->begin(
        request(96, "https://client.example", "Noisedeck")));
    const auto result = wait_for_result(*prompt);
    SYNC_REQUIRE(result.generation == 96);
    SYNC_REQUIRE(result.decision == pairing::PromptDecision::TimedOut);
    SYNC_REQUIRE(record->cancel_calls == 1);
    SYNC_REQUIRE(record->release_calls == 1);
  }
}

SYNC_TEST(mac_prompt_cancel_suppresses_an_inflight_late_approval_and_reuses_slot) {
  auto record = std::make_shared<AdapterRecord>(AdapterMode::ControlledApprove);
  auto prompt = make_prompt(record, 5s, 50ms);
  SYNC_REQUIRE(prompt->begin(request(101, "https://client.example", "Noisedeck")));
  SYNC_REQUIRE(wait_until([&] { return record->receive_calls.load() == 1; }));
  prompt->cancel(101);
  release_controlled_response(*record);
  SYNC_REQUIRE(wait_until([&] { return record->release_calls.load() == 1; }));
  SYNC_REQUIRE(!prompt->poll().available);
  SYNC_REQUIRE(record->cancel_calls == 1);

  const auto second = request(102, "https://client.example", "Noisedeck");
  SYNC_REQUIRE(wait_until([&] { return prompt->begin(second); }));
  const auto result = wait_for_result(*prompt);
  SYNC_REQUIRE(result.generation == 102);
  SYNC_REQUIRE(result.decision == pairing::PromptDecision::Approved);
}

SYNC_TEST(mac_prompt_unconsumed_result_blocks_begin_and_can_be_canceled) {
  auto record = std::make_shared<AdapterRecord>(AdapterMode::Approve);
  auto prompt = make_prompt(record);
  const auto first = request(111, "https://client.example", "Noisedeck");
  const auto second = request(112, "https://client.example", "Noisedeck");
  SYNC_REQUIRE(prompt->begin(first));
  SYNC_REQUIRE(wait_until([&] { return record->release_calls.load() == 1; }));
  SYNC_REQUIRE(!prompt->begin(second));
  prompt->cancel(999);
  SYNC_REQUIRE(!prompt->begin(second));
  prompt->cancel(111);
  SYNC_REQUIRE(!prompt->poll().available);
  SYNC_REQUIRE(prompt->begin(second));
  SYNC_REQUIRE(wait_for_result(*prompt).generation == 112);
}

SYNC_TEST(mac_prompt_wrong_generation_cancel_does_not_suppress_result) {
  auto record = std::make_shared<AdapterRecord>(AdapterMode::ControlledApprove);
  auto prompt = make_prompt(record, 5s, 50ms);
  SYNC_REQUIRE(prompt->begin(request(121, "https://client.example", "Noisedeck")));
  SYNC_REQUIRE(wait_until([&] { return record->receive_calls.load() == 1; }));
  prompt->cancel(122);
  release_controlled_response(*record);
  const auto result = wait_for_result(*prompt);
  SYNC_REQUIRE(result.generation == 121);
  SYNC_REQUIRE(result.decision == pairing::PromptDecision::Approved);
  SYNC_REQUIRE(record->cancel_calls == 0);
}

SYNC_TEST(mac_prompt_destruction_cancels_releases_and_joins_active_worker) {
  auto record = std::make_shared<AdapterRecord>(AdapterMode::SlowApprove);
  const auto started = std::chrono::steady_clock::now();
  {
    auto prompt = make_prompt(record, 200ms, 50ms);
    SYNC_REQUIRE(prompt->begin(request(131, "https://client.example", "Noisedeck")));
    SYNC_REQUIRE(wait_until([&] { return record->receive_calls.load() == 1; }));
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  SYNC_REQUIRE(elapsed < 500ms);
  SYNC_REQUIRE(record->cancel_calls == 1);
  SYNC_REQUIRE(record->release_calls == 1);
  require_one_worker_thread(*record);
}

}  // namespace
