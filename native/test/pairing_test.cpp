#include "test_harness.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <sync/pairing.hpp>
#include <sync/secure_memory.hpp>

namespace {
using noisefactor::sync::pairing::parse_request;
using noisefactor::sync::pairing::PromptDecision;

class HoldingAuthority final
    : public noisefactor::sync::pairing::PairingAuthority {
 public:
  enum class HeldOperation { Authenticate, Issue };

  explicit HoldingAuthority(HeldOperation operation) : operation_(operation) {}

  auto issue(const noisefactor::sync::NormalizedOrigin &,
             noisefactor::sync::PairingCommitGate &gate) noexcept
      -> noisefactor::sync::PairingIssueResult override {
    enter(HeldOperation::Issue);
    ++issue_calls;
    if (!gate.try_begin_commit()) {
      return {.error = noisefactor::sync::PairingStoreError::Canceled};
    }
    return {};
  }

  auto authenticate(const noisefactor::sync::NormalizedOrigin &,
                    std::string_view) noexcept
      -> noisefactor::sync::PairingAuthenticationResult override {
    enter(HeldOperation::Authenticate);
    ++authenticate_calls;
    return {.error = noisefactor::sync::PairingStoreError::None,
            .authenticated = true};
  }

  bool wait_until_entered() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(2),
                               [&] { return entered_; });
  }

  void release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

  std::size_t authenticate_calls = 0;
  std::size_t issue_calls = 0;
  std::thread::id worker_thread{};

 private:
  void enter(HeldOperation operation) noexcept {
    if (operation != operation_) return;
    std::unique_lock lock(mutex_);
    worker_thread = std::this_thread::get_id();
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [&] { return released_; });
  }

  HeldOperation operation_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
};

class RecordingCleanseObserver final
    : public noisefactor::sync::CleanseObserver {
 public:
  void after_cleanse(std::span<const std::byte> bytes) noexcept override {
    ++calls;
    for (const std::byte byte : bytes) {
      if (byte != std::byte{0}) all_zero = false;
    }
  }

  std::size_t calls = 0;
  bool all_zero = true;
};

class ImmediateAuthority final
    : public noisefactor::sync::pairing::PairingAuthority {
 public:
  auto issue(const noisefactor::sync::NormalizedOrigin &,
             noisefactor::sync::PairingCommitGate &gate) noexcept
      -> noisefactor::sync::PairingIssueResult override {
    if (!gate.try_begin_commit())
      return {.error = noisefactor::sync::PairingStoreError::Canceled};
    return {};
  }

  auto authenticate(const noisefactor::sync::NormalizedOrigin &,
                    std::string_view) noexcept
      -> noisefactor::sync::PairingAuthenticationResult override {
    return {.error = noisefactor::sync::PairingStoreError::None,
            .authenticated = true};
  }
};

} // namespace

SYNC_TEST(pairing_parser_accepts_exact_bounded_request_in_any_field_order) {
  const auto first = parse_request(
      R"({"type":"pair","protocolVersions":[2,1],"name":"Noisedeck \ud83c\udfa8"})");
  SYNC_REQUIRE(first.ok());
  SYNC_REQUIRE(first.request.name() == "Noisedeck \xF0\x9F\x8E\xA8");
  SYNC_REQUIRE(first.request.supports(1));
  SYNC_REQUIRE(first.request.supports(2));
  SYNC_REQUIRE(
      parse_request(R"({"name":"Deck","type":"pair","protocolVersions":[1]})")
          .ok());
}

SYNC_TEST(
    pairing_parser_rejects_malformed_duplicate_unknown_missing_wrong_and_unsupported_values) {
  const std::array rejected = {
      "",
      "{}",
      R"({"type":"nope","protocolVersions":[1],"name":"Deck"})",
      R"({"type":"pair","type":"pair","protocolVersions":[1],"name":"Deck"})",
      R"({"type":"pair","protocolVersions":[1],"name":"Deck","extra":true})",
      R"({"type":"pair","protocolVersions":"1","name":"Deck"})",
      R"({"type":"pair","protocolVersions":[1,1],"name":"Deck"})",
      R"({"type":"pair","protocolVersions":[-1],"name":"Deck"})",
      R"({"type":"pair","protocolVersions":[1.5],"name":"Deck"})",
      R"({"type":"pair","protocolVersions":[01],"name":"Deck"})",
      R"({"type":"pair","protocolVersions":[65536],"name":"Deck"})",
      R"({"type":"pair","protocolVersions":[1,2,3,4,5,6,7,8,9],"name":"Deck"})",
      R"({"type":"pair","protocolVersions":[1,],"name":"Deck"})",
      R"({"type":"pair","protocolVersions":[1],"name":"Deck",})",
      R"({"type":"pair","protocolVersions":[2],"name":"Deck"})",
      R"({"type":"pair","protocolVersions":[1],"name":""})",
      R"({"type":"pair","protocolVersions":[1],"name":"bad\nname"})",
      R"({"type":"pair","protocolVersions":[1],"name":3})",
      R"({"type":"pair","protocolVersions":[1],"name":"Deck"} trailing)",
  };
  for (const std::string_view value : rejected)
    SYNC_REQUIRE(!parse_request(value).ok());
  std::string overlong(65, 'x');
  SYNC_REQUIRE(
      !parse_request("{\"type\":\"pair\",\"protocolVersions\":[1],\"name\":\"" +
                     overlong + "\"}")
           .ok());
  std::string oversized(
      noisefactor::sync::pairing::kMaximumPairingMessageBytes + 1, ' ');
  SYNC_REQUIRE(!parse_request(oversized).ok());
  const std::array malformed_utf8 = {
      std::string(
          "{\"type\":\"pair\",\"protocolVersions\":[1],\"name\":\"\xC0\xAF\"}"),
      std::string("{\"type\":\"pair\",\"protocolVersions\":[1],\"name\":"
                  "\"\xED\xA0\x80\"}"),
      std::string("{\"type\":\"pair\",\"protocolVersions\":[1],\"name\":"
                  "\"\xF4\x90\x80\x80\"}"),
      std::string(R"({"type":"pair","protocolVersions":[1],"name":"\ud800"})"),
      std::string(R"({"type":"pair","protocolVersions":[1],"name":"\q"})"),
      std::string(R"({"type":"pair","protocolVersions":[1],"name":"\u0085"})"),
  };
  for (const auto &value : malformed_utf8)
    SYNC_REQUIRE(!parse_request(value).ok());
}

SYNC_TEST(pairing_encoders_are_exact_and_never_put_tokens_in_errors) {
  const std::string token(64, 'a');
  SYNC_REQUIRE(noisefactor::sync::pairing::encode_paired(1, token) ==
               "{\"type\":\"paired\",\"protocolVersion\":1,\"token\":\"" +
                   token + "\"}");
  SYNC_REQUIRE(noisefactor::sync::pairing::encode_paired(1, "abc123").empty());
  SYNC_REQUIRE(
      noisefactor::sync::pairing::encode_paired(1, std::string(64, 'A'))
          .empty());
  SYNC_REQUIRE(noisefactor::sync::pairing::encode_paired(2, token).empty());
  SYNC_REQUIRE(
      noisefactor::sync::pairing::encode_error("denied", "Pairing denied") ==
      R"({"type":"error","code":"denied","message":"Pairing denied"})");
  SYNC_REQUIRE(
      noisefactor::sync::pairing::encode_error("bad\"code", "message").empty());
  SYNC_REQUIRE(
      noisefactor::sync::pairing::encode_error("denied", "bad \"message")
          .empty());
}

SYNC_TEST(pairing_prompt_values_are_fixed_and_generation_scoped) {
  const auto normalized =
      noisefactor::sync::normalize_origin("https://deck.example");
  SYNC_REQUIRE(normalized.ok());
  noisefactor::sync::pairing::PromptRequest request;
  SYNC_REQUIRE(request.assign(42, normalized.origin, "Deck"));
  SYNC_REQUIRE(request.generation == 42);
  SYNC_REQUIRE(request.origin.view() == "https://deck.example");
  SYNC_REQUIRE(request.name() == "Deck");
  const noisefactor::sync::pairing::PromptResult result{
      .available = true,
      .generation = 42,
      .decision = PromptDecision::Approved};
  SYNC_REQUIRE(result.available);
  SYNC_REQUIRE(result.generation == request.generation);
  const noisefactor::sync::pairing::PromptResult timeout{
      .available = true,
      .generation = 42,
      .decision = PromptDecision::TimedOut};
  SYNC_REQUIRE(timeout.decision != PromptDecision::Denied);
  noisefactor::sync::pairing::PromptRequest invalid;
  SYNC_REQUIRE(!invalid.assign(1, {}, "Deck"));
  SYNC_REQUIRE(!invalid.assign(1, normalized.origin, ""));
  SYNC_REQUIRE(!invalid.assign(1, normalized.origin, "bad\nname"));
  SYNC_REQUIRE(!invalid.assign(1, normalized.origin, std::string("\xC2\x85")));
  SYNC_REQUIRE(
      !invalid.assign(1, normalized.origin, std::string("\xED\xA0\x80")));
}

SYNC_TEST(
    pairing_authority_worker_is_single_threaded_bounded_and_generation_scoped) {
  const auto normalized =
      noisefactor::sync::normalize_origin("https://deck.example");
  SYNC_REQUIRE(normalized.ok());
  HoldingAuthority authority(HoldingAuthority::HeldOperation::Authenticate);
  noisefactor::sync::pairing::AuthorityWorker worker(authority);
  const auto caller_thread = std::this_thread::get_id();

  SYNC_REQUIRE(worker.submit_authenticate(1, normalized.origin, "token-1"));
  SYNC_REQUIRE(authority.wait_until_entered());
  SYNC_REQUIRE(!worker.submit_authenticate(1, normalized.origin,
                                           "duplicate-generation"));
  for (std::uint64_t generation = 2;
       generation <= noisefactor::sync::pairing::kMaximumAuthorityRequests;
       ++generation) {
    SYNC_REQUIRE(worker.submit_authenticate(generation, normalized.origin,
                                            "queued-token"));
  }
  SYNC_REQUIRE(!worker.submit_authenticate(
      noisefactor::sync::pairing::kMaximumAuthorityRequests + 1,
      normalized.origin, "saturated-token"));
  authority.release();

  std::size_t results = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (results < noisefactor::sync::pairing::kMaximumAuthorityRequests &&
         std::chrono::steady_clock::now() < deadline) {
    noisefactor::sync::pairing::AuthorityResult result;
    if (!worker.poll(result)) {
      std::this_thread::yield();
      continue;
    }
    SYNC_REQUIRE(result.operation ==
                 noisefactor::sync::pairing::AuthorityOperation::Authenticate);
    SYNC_REQUIRE(result.generation >= 1);
    SYNC_REQUIRE(result.authentication.authenticated);
    ++results;
  }
  SYNC_REQUIRE(results ==
               noisefactor::sync::pairing::kMaximumAuthorityRequests);
  SYNC_REQUIRE(authority.authenticate_calls == results);
  SYNC_REQUIRE(authority.worker_thread != caller_thread);
  worker.shutdown();
  SYNC_REQUIRE(!worker.submit_issue(100, normalized.origin));
}

SYNC_TEST(pairing_authority_worker_serializes_issue_without_callbacks) {
  const auto normalized =
      noisefactor::sync::normalize_origin("https://deck.example");
  SYNC_REQUIRE(normalized.ok());
  HoldingAuthority authority(HoldingAuthority::HeldOperation::Issue);
  noisefactor::sync::pairing::AuthorityWorker worker(authority);
  SYNC_REQUIRE(worker.submit_issue(99, normalized.origin));
  SYNC_REQUIRE(authority.wait_until_entered());
  noisefactor::sync::pairing::AuthorityResult unavailable;
  SYNC_REQUIRE(!worker.poll(unavailable));
  authority.release();

  noisefactor::sync::pairing::AuthorityResult result;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!worker.poll(result) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  SYNC_REQUIRE(result.generation == 99);
  SYNC_REQUIRE(result.operation ==
               noisefactor::sync::pairing::AuthorityOperation::Issue);
  SYNC_REQUIRE(authority.issue_calls == 1);
}

SYNC_TEST(pairing_authority_worker_shutdown_discards_late_results_and_joins) {
  const auto normalized =
      noisefactor::sync::normalize_origin("https://deck.example");
  SYNC_REQUIRE(normalized.ok());
  HoldingAuthority authority(HoldingAuthority::HeldOperation::Authenticate);
  noisefactor::sync::pairing::AuthorityWorker worker(authority);
  SYNC_REQUIRE(worker.submit_authenticate(7, normalized.origin, "held-token"));
  SYNC_REQUIRE(authority.wait_until_entered());

  std::atomic<bool> shutdown_returned = false;
  std::thread shutdown([&] {
    worker.shutdown();
    shutdown_returned.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  SYNC_REQUIRE(!shutdown_returned.load());
  authority.release();
  shutdown.join();
  SYNC_REQUIRE(shutdown_returned.load());
  noisefactor::sync::pairing::AuthorityResult discarded;
  SYNC_REQUIRE(!worker.poll(discarded));
}

SYNC_TEST(pairing_authority_worker_cancels_queued_tokens_and_active_issue) {
  const auto normalized =
      noisefactor::sync::normalize_origin("https://deck.example");
  SYNC_REQUIRE(normalized.ok());
  HoldingAuthority authentication(
      HoldingAuthority::HeldOperation::Authenticate);
  RecordingCleanseObserver observer;
  noisefactor::sync::pairing::AuthorityWorker worker(authentication,
                                                      &observer);
  SYNC_REQUIRE(worker.submit_authenticate(1, normalized.origin,
                                          "active-secret"));
  SYNC_REQUIRE(authentication.wait_until_entered());
  SYNC_REQUIRE(worker.submit_authenticate(2, normalized.origin,
                                          "queued-secret"));
  const std::size_t before_cancel = observer.calls;
  SYNC_REQUIRE(worker.cancel(2));
  SYNC_REQUIRE(observer.calls > before_cancel);
  SYNC_REQUIRE(observer.all_zero);
  authentication.release();

  noisefactor::sync::pairing::AuthorityResult authentication_result;
  const auto authentication_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!worker.poll(authentication_result) &&
         std::chrono::steady_clock::now() < authentication_deadline) {
    std::this_thread::yield();
  }
  SYNC_REQUIRE(authentication_result.generation == 1);
  SYNC_REQUIRE(authentication.authenticate_calls == 1);

  HoldingAuthority issuance(HoldingAuthority::HeldOperation::Issue);
  noisefactor::sync::pairing::AuthorityWorker issue_worker(issuance);
  SYNC_REQUIRE(issue_worker.submit_issue(3, normalized.origin));
  SYNC_REQUIRE(issuance.wait_until_entered());
  SYNC_REQUIRE(issue_worker.cancel(3));
  issuance.release();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  noisefactor::sync::pairing::AuthorityResult canceled_issue;
  SYNC_REQUIRE(!issue_worker.poll(canceled_issue));
}

SYNC_TEST(pairing_authority_worker_cancel_suppresses_an_already_queued_result) {
  const auto normalized =
      noisefactor::sync::normalize_origin("https://deck.example");
  SYNC_REQUIRE(normalized.ok());
  ImmediateAuthority authority;
  noisefactor::sync::pairing::AuthorityWorker worker(authority);
  SYNC_REQUIRE(worker.submit_authenticate(55, normalized.origin, "token"));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!worker.has_result(55) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  SYNC_REQUIRE(worker.has_result(55));
  SYNC_REQUIRE(worker.cancel(55));
  noisefactor::sync::pairing::AuthorityResult suppressed;
  SYNC_REQUIRE(!worker.poll(suppressed));
  SYNC_REQUIRE(!worker.cancel(55));
}
