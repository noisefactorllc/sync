#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sync/publisher_hub.hpp>

namespace {

using noisefactor::sync::FramePublisher;
using noisefactor::sync::ProviderFailure;
using noisefactor::sync::ProviderFailureKind;
using noisefactor::sync::PublishResult;
using noisefactor::sync::PublisherHub;
using noisefactor::sync::protocol::FrameView;

class DefaultLifecyclePublisher final : public FramePublisher {
 public:
  auto publish(std::string_view, const FrameView&) noexcept -> PublishResult override {
    return PublishResult::Accepted;
  }
};

class RecordingProvider final : public FramePublisher {
 public:
  explicit RecordingProvider(std::string label = {}, std::vector<std::string>* order = nullptr)
      : label_(std::move(label)), order_(order) {}

  auto open_sender(std::string_view sender_id, std::string_view name) noexcept -> bool override {
    ++open_calls;
    opened_ids.emplace_back(sender_id);
    opened_names.emplace_back(name);
    record("open");
    return open_result;
  }

  void close_sender(std::string_view sender_id) noexcept override {
    ++close_calls;
    closed_ids.emplace_back(sender_id);
    record("close");
  }

  auto publish(std::string_view sender_id, const FrameView& frame) noexcept
      -> PublishResult override {
    ++publish_calls;
    published_ids.emplace_back(sender_id);
    observed_frame = &frame;
    observed_payload = frame.payload.data();
    observed_payload_size = frame.payload.size();
    record("publish");
    return publish_result;
  }

  auto diagnostic_checksum(std::string_view sender_id) const noexcept
      -> std::uint64_t override {
    ++checksum_calls;
    checksum_ids.emplace_back(sender_id);
    return checksum;
  }

  auto poll_failure(std::uint64_t now_ms) noexcept
      -> std::optional<ProviderFailure> override {
    ++poll_calls;
    last_poll_ms = now_ms;
    return failure;
  }

  bool open_result = true;
  PublishResult publish_result = PublishResult::Accepted;
  std::uint64_t checksum = 0;
  std::optional<ProviderFailure> failure;
  std::uint64_t last_poll_ms = 0;
  std::size_t open_calls = 0;
  std::size_t close_calls = 0;
  std::size_t publish_calls = 0;
  mutable std::size_t checksum_calls = 0;
  std::size_t poll_calls = 0;
  std::vector<std::string> opened_ids;
  std::vector<std::string> opened_names;
  std::vector<std::string> closed_ids;
  std::vector<std::string> published_ids;
  mutable std::vector<std::string> checksum_ids;
  const FrameView* observed_frame = nullptr;
  const std::byte* observed_payload = nullptr;
  std::size_t observed_payload_size = 0;

 private:
  void record(std::string_view operation) {
    if (order_ != nullptr) {
      order_->push_back(std::string(operation) + ":" + label_);
    }
  }

  std::string label_;
  std::vector<std::string>* order_ = nullptr;
};

auto make_frame(std::span<const std::byte> payload) -> FrameView {
  return {
      .version = 1,
      .header_bytes = 64,
      .flags = 1,
      .pixel_format = 1,
      .color_space = 1,
      .alpha_mode = 3,
      .width = 1,
      .height = 1,
      .row_stride = 4,
      .payload_bytes = 4,
      .sequence = 1,
      .presentation_time_us = 2,
      .top_down = true,
      .payload = payload,
  };
}

}  // namespace

SYNC_TEST(frame_publisher_default_lifecycle_is_source_compatible_and_safe) {
  DefaultLifecyclePublisher publisher;

  SYNC_REQUIRE(publisher.open_sender("sender", "Name"));
  publisher.close_sender("sender");
  SYNC_REQUIRE(publisher.diagnostic_checksum("sender") == 0);
}

SYNC_TEST(publisher_hub_opens_publishes_borrowed_frame_and_closes_in_reverse_order) {
  std::vector<std::string> order;
  RecordingProvider first("first", &order);
  RecordingProvider second("second", &order);
  std::array<FramePublisher*, 2> providers{&first, &second};
  PublisherHub hub(providers);

  SYNC_REQUIRE(hub.open_sender("sender-1", "Exact Name"));
  SYNC_REQUIRE(first.opened_ids == std::vector<std::string>{"sender-1"});
  SYNC_REQUIRE(first.opened_names == std::vector<std::string>{"Exact Name"});
  SYNC_REQUIRE(second.opened_ids == first.opened_ids);
  SYNC_REQUIRE(second.opened_names == first.opened_names);

  const std::array<std::byte, 4> payload{
      std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const FrameView frame = make_frame(payload);
  SYNC_REQUIRE(hub.publish("sender-1", frame) == PublishResult::Accepted);
  SYNC_REQUIRE(first.observed_frame == &frame);
  SYNC_REQUIRE(second.observed_frame == &frame);
  SYNC_REQUIRE(first.observed_payload == payload.data());
  SYNC_REQUIRE(second.observed_payload == payload.data());
  SYNC_REQUIRE(first.observed_payload_size == payload.size());

  hub.close_sender("sender-1");
  const std::vector<std::string> expected_order{
      "open:first", "open:second", "publish:first", "publish:second",
      "close:second", "close:first"};
  SYNC_REQUIRE(order == expected_order);
}

SYNC_TEST(publisher_hub_rolls_back_partial_open_in_reverse_order_without_retaining_sender) {
  std::vector<std::string> order;
  RecordingProvider first("first", &order);
  RecordingProvider second("second", &order);
  RecordingProvider rejecting("rejecting", &order);
  RecordingProvider untouched("untouched", &order);
  rejecting.open_result = false;
  std::array<FramePublisher*, 4> providers{&first, &second, &rejecting, &untouched};
  PublisherHub hub(providers);

  SYNC_REQUIRE(!hub.open_sender("rollback", "Rollback"));
  const std::vector<std::string> expected_order{
      "open:first", "open:second", "open:rejecting", "close:second", "close:first"};
  SYNC_REQUIRE(order == expected_order);
  SYNC_REQUIRE(untouched.open_calls == 0);

  const std::array<std::byte, 4> payload{};
  const FrameView frame = make_frame(payload);
  SYNC_REQUIRE(hub.publish("rollback", frame) == PublishResult::Failed);
  hub.close_sender("rollback");
  SYNC_REQUIRE(first.close_calls == 1);
  SYNC_REQUIRE(second.close_calls == 1);
}

SYNC_TEST(publisher_hub_validates_lifecycle_bounds_and_recycles_all_64_sender_slots) {
  RecordingProvider provider;
  std::array<FramePublisher*, 1> providers{&provider};
  PublisherHub hub(providers);
  const std::string id128(128, 'i');
  const std::string id129(129, 'i');
  const std::string name64(64, 'n');
  const std::string name65(65, 'n');

  SYNC_REQUIRE(!hub.open_sender("", "Name"));
  SYNC_REQUIRE(!hub.open_sender(id129, "Name"));
  SYNC_REQUIRE(!hub.open_sender("id", ""));
  SYNC_REQUIRE(!hub.open_sender("id", name65));
  SYNC_REQUIRE(hub.open_sender(id128, name64));
  SYNC_REQUIRE(!hub.open_sender(id128, name64));
  hub.close_sender(id128);

  for (std::size_t index = 0; index < 64; ++index) {
    SYNC_REQUIRE(hub.open_sender("full-" + std::to_string(index), "Name"));
  }
  SYNC_REQUIRE(!hub.open_sender("overflow", "Name"));
  hub.close_sender("full-31");
  SYNC_REQUIRE(hub.open_sender("replacement", "Replacement"));
  hub.close_sender("missing");
  hub.close_sender("replacement");
  hub.close_sender("replacement");
  SYNC_REQUIRE(provider.close_calls == 3);
}

SYNC_TEST(publisher_hub_copies_at_most_four_non_null_providers_and_rejects_overflow) {
  RecordingProvider first;
  RecordingProvider second;
  RecordingProvider third;
  RecordingProvider fourth;
  RecordingProvider fifth;

  std::array<FramePublisher*, 5> too_many{&first, &second, &third, &fourth, &fifth};
  bool rejected = false;
  try {
    PublisherHub invalid(too_many);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  SYNC_REQUIRE(rejected);
  SYNC_REQUIRE(first.open_calls == 0);

  std::array<FramePublisher*, 4> external{&first, nullptr, &second, nullptr};
  PublisherHub copied(external);
  external = {&third, &fourth, &fifth, nullptr};
  SYNC_REQUIRE(copied.open_sender("copied", "Copied"));
  SYNC_REQUIRE(first.open_calls == 1);
  SYNC_REQUIRE(second.open_calls == 1);
  SYNC_REQUIRE(third.open_calls == 0);
  copied.close_sender("copied");

  std::array<FramePublisher*, 6> four_non_null{nullptr, &first, nullptr, &second, &third, &fourth};
  PublisherHub bounded(four_non_null);
  SYNC_REQUIRE(bounded.open_sender("four", "Four"));
  SYNC_REQUIRE(fourth.open_calls == 1);
  bounded.close_sender("four");
}

SYNC_TEST(publisher_hub_aggregates_results_and_returns_first_nonzero_checksum) {
  RecordingProvider first;
  RecordingProvider second;
  RecordingProvider third;
  first.publish_result = PublishResult::Failed;
  second.publish_result = PublishResult::Backpressured;
  third.publish_result = PublishResult::Accepted;
  first.checksum = 0;
  second.checksum = 22;
  third.checksum = 33;
  std::array<FramePublisher*, 3> providers{&first, &second, &third};
  PublisherHub hub(providers);
  const std::array<std::byte, 4> payload{};
  const FrameView frame = make_frame(payload);

  SYNC_REQUIRE(hub.publish("missing", frame) == PublishResult::Failed);
  SYNC_REQUIRE(hub.diagnostic_checksum("missing") == 0);
  SYNC_REQUIRE(hub.open_sender("mixed", "Mixed"));
  SYNC_REQUIRE(hub.publish("mixed", frame) == PublishResult::Accepted);
  SYNC_REQUIRE(hub.diagnostic_checksum("mixed") == 22);
  SYNC_REQUIRE(first.checksum_calls == 1);
  SYNC_REQUIRE(second.checksum_calls == 1);
  SYNC_REQUIRE(third.checksum_calls == 0);

  third.publish_result = PublishResult::Failed;
  SYNC_REQUIRE(hub.publish("mixed", frame) == PublishResult::Backpressured);
  second.publish_result = PublishResult::Failed;
  SYNC_REQUIRE(hub.publish("mixed", frame) == PublishResult::Failed);

  std::array<FramePublisher*, 0> no_providers{};
  PublisherHub empty(no_providers);
  SYNC_REQUIRE(empty.open_sender("empty", "Empty"));
  SYNC_REQUIRE(empty.publish("empty", frame) == PublishResult::Failed);
}

SYNC_TEST(publisher_hub_closed_senders_receive_no_calls_and_churn_has_no_stale_provider_mask) {
  RecordingProvider first;
  RecordingProvider second;
  std::array<FramePublisher*, 2> providers{&first, &second};
  PublisherHub hub(providers);
  const std::array<std::byte, 4> payload{};
  const FrameView frame = make_frame(payload);

  SYNC_REQUIRE(hub.open_sender("reused", "First generation"));
  SYNC_REQUIRE(hub.publish("reused", frame) == PublishResult::Accepted);
  hub.close_sender("reused");
  const std::size_t first_publish_calls = first.publish_calls;
  const std::size_t second_publish_calls = second.publish_calls;
  const std::size_t first_checksum_calls = first.checksum_calls;
  SYNC_REQUIRE(hub.publish("reused", frame) == PublishResult::Failed);
  SYNC_REQUIRE(hub.diagnostic_checksum("reused") == 0);
  hub.close_sender("reused");
  SYNC_REQUIRE(first.publish_calls == first_publish_calls);
  SYNC_REQUIRE(second.publish_calls == second_publish_calls);
  SYNC_REQUIRE(first.checksum_calls == first_checksum_calls);

  second.open_result = false;
  SYNC_REQUIRE(!hub.open_sender("reused", "Rejected generation"));
  SYNC_REQUIRE(hub.publish("reused", frame) == PublishResult::Failed);
  second.open_result = true;
  SYNC_REQUIRE(hub.open_sender("reused", "Second generation"));
  SYNC_REQUIRE(hub.publish("reused", frame) == PublishResult::Accepted);
  SYNC_REQUIRE(first.publish_calls == first_publish_calls + 1);
  SYNC_REQUIRE(second.publish_calls == second_publish_calls + 1);
  hub.close_sender("reused");
  SYNC_REQUIRE(first.close_calls == 3);
  SYNC_REQUIRE(second.close_calls == 2);
}

SYNC_TEST(publisher_hub_propagates_the_first_provider_failure_in_stable_order) {
  RecordingProvider first;
  RecordingProvider second;
  first.failure = ProviderFailure{
      .kind = ProviderFailureKind::MetalCommandFailed,
      .native_status = 5,
      .native_error_code = -9,
  };
  second.failure = ProviderFailure{
      .kind = ProviderFailureKind::MetalWatchdogTimeout,
  };
  std::array<FramePublisher*, 2> providers{&first, &second};
  PublisherHub hub(providers);

  const auto observed = hub.poll_failure(1'234);
  SYNC_REQUIRE(observed.has_value());
  SYNC_REQUIRE(observed->kind == ProviderFailureKind::MetalCommandFailed);
  SYNC_REQUIRE(observed->native_status == 5);
  SYNC_REQUIRE(observed->native_error_code == -9);
  SYNC_REQUIRE(first.last_poll_ms == 1'234);
  SYNC_REQUIRE(first.poll_calls == 1);
  SYNC_REQUIRE(second.poll_calls == 0);
  SYNC_REQUIRE(first.publish_calls == 0);
  SYNC_REQUIRE(second.publish_calls == 0);
}
