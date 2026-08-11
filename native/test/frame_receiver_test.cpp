#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sync/frame_receiver.hpp>

namespace {

using noisefactor::sync::FramePublisher;
using noisefactor::sync::FrameReceiver;
using noisefactor::sync::PublishResult;
using noisefactor::sync::ReceiveStatus;
using noisefactor::sync::protocol::DecodeError;
using noisefactor::sync::protocol::FrameView;

struct PublishedFrame {
  std::string sender_id;
  std::uint16_t version = 0;
  std::uint16_t header_bytes = 0;
  std::uint32_t flags = 0;
  std::uint16_t pixel_format = 0;
  std::uint16_t color_space = 0;
  std::uint16_t alpha_mode = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t row_stride = 0;
  std::uint32_t payload_bytes = 0;
  std::uint64_t sequence = 0;
  std::uint64_t presentation_time_us = 0;
  bool top_down = false;
  std::uint32_t payload_checksum = 0;
};

class RecordingPublisher final : public FramePublisher {
 public:
  explicit RecordingPublisher(std::vector<PublishResult> results = {})
      : results_(std::move(results)) {}

  PublishResult publish(std::string_view sender_id, const FrameView& frame) noexcept override {
    std::uint32_t checksum = 2166136261U;
    for (const std::byte value : frame.payload) {
      checksum ^= std::to_integer<std::uint8_t>(value);
      checksum *= 16777619U;
    }
    records.push_back({
        .sender_id = std::string(sender_id),
        .version = frame.version,
        .header_bytes = frame.header_bytes,
        .flags = frame.flags,
        .pixel_format = frame.pixel_format,
        .color_space = frame.color_space,
        .alpha_mode = frame.alpha_mode,
        .width = frame.width,
        .height = frame.height,
        .row_stride = frame.row_stride,
        .payload_bytes = frame.payload_bytes,
        .sequence = frame.sequence,
        .presentation_time_us = frame.presentation_time_us,
        .top_down = frame.top_down,
        .payload_checksum = checksum,
    });

    if (next_result_ < results_.size()) {
      return results_[next_result_++];
    }
    return PublishResult::Accepted;
  }

  std::vector<PublishedFrame> records;

 private:
  std::vector<PublishResult> results_;
  std::size_t next_result_ = 0;
};

std::vector<std::byte> load_golden_frame() {
  const auto path = std::filesystem::path(SYNC_SOURCE_DIR) / "test/fixtures/frame-v1.bin";
  std::ifstream input(path, std::ios::binary);
  SYNC_REQUIRE(input.good());
  std::vector<std::byte> frame;
  char value = 0;
  while (input.get(value)) {
    frame.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
  }
  return frame;
}

void write_u32(std::vector<std::byte>& frame, std::size_t offset, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    frame[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

void write_u64(std::vector<std::byte>& frame, std::size_t offset, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    frame[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
  }
}

}  // namespace

SYNC_TEST(receiver_publishes_the_golden_frame_and_records_literal_metadata) {
  RecordingPublisher publisher;
  FrameReceiver receiver(publisher);
  auto frame = load_golden_frame();

  const auto result = receiver.receive("golden-sender", frame);

  SYNC_REQUIRE(result.status == ReceiveStatus::Accepted);
  SYNC_REQUIRE(result.decode_error == DecodeError::None);
  SYNC_REQUIRE(publisher.records.size() == 1);
  const auto& published = publisher.records.front();
  SYNC_REQUIRE(published.sender_id == "golden-sender");
  SYNC_REQUIRE(published.version == 1);
  SYNC_REQUIRE(published.header_bytes == 64);
  SYNC_REQUIRE(published.flags == 1);
  SYNC_REQUIRE(published.pixel_format == 1);
  SYNC_REQUIRE(published.color_space == 1);
  SYNC_REQUIRE(published.alpha_mode == 3);
  SYNC_REQUIRE(published.width == 2);
  SYNC_REQUIRE(published.height == 2);
  SYNC_REQUIRE(published.row_stride == 8);
  SYNC_REQUIRE(published.payload_bytes == 16);
  SYNC_REQUIRE(published.sequence == 4294967301ULL);
  SYNC_REQUIRE(published.presentation_time_us == 1723305600123456ULL);
  SYNC_REQUIRE(published.top_down);
  SYNC_REQUIRE(published.payload_checksum == 1186813628U);

  const auto* stats = receiver.stats("golden-sender");
  SYNC_REQUIRE(stats != nullptr);
  SYNC_REQUIRE(stats->accepted == 1);
  SYNC_REQUIRE(stats->dropped == 0);
  SYNC_REQUIRE(stats->rejected == 0);
  SYNC_REQUIRE(stats->failed == 0);
  SYNC_REQUIRE(stats->has_last_sequence);
  SYNC_REQUIRE(stats->last_sequence == 4294967301ULL);
  SYNC_REQUIRE(stats->last_presentation_time_us == 1723305600123456ULL);
}

SYNC_TEST(receiver_rejects_malformed_frames_without_publishing) {
  RecordingPublisher publisher;
  FrameReceiver receiver(publisher);
  auto frame = load_golden_frame();
  write_u32(frame, 0, 0);

  const auto result = receiver.receive("malformed-sender", frame);

  SYNC_REQUIRE(result.status == ReceiveStatus::RejectedMalformed);
  SYNC_REQUIRE(result.decode_error == DecodeError::BadMagic);
  SYNC_REQUIRE(publisher.records.empty());
  const auto* stats = receiver.stats("malformed-sender");
  SYNC_REQUIRE(stats != nullptr);
  SYNC_REQUIRE(stats->accepted == 0);
  SYNC_REQUIRE(stats->dropped == 0);
  SYNC_REQUIRE(stats->rejected == 1);
  SYNC_REQUIRE(stats->failed == 0);
  SYNC_REQUIRE(!stats->has_last_sequence);
}

SYNC_TEST(receiver_rejects_invalid_sender_ids_without_creating_state) {
  RecordingPublisher publisher;
  FrameReceiver receiver(publisher);
  const auto frame = load_golden_frame();
  const std::string oversized_sender(129, 's');

  const auto empty = receiver.receive("", frame);
  const auto oversized = receiver.receive(oversized_sender, frame);

  SYNC_REQUIRE(empty.status == ReceiveStatus::RejectedSender);
  SYNC_REQUIRE(empty.decode_error == DecodeError::None);
  SYNC_REQUIRE(oversized.status == ReceiveStatus::RejectedSender);
  SYNC_REQUIRE(oversized.decode_error == DecodeError::None);
  SYNC_REQUIRE(publisher.records.empty());
  SYNC_REQUIRE(receiver.stats("") == nullptr);
  SYNC_REQUIRE(receiver.stats(oversized_sender) == nullptr);
}

SYNC_TEST(receiver_accepts_an_exactly_128_byte_sender_id_and_removes_its_state) {
  RecordingPublisher publisher;
  FrameReceiver receiver(publisher);
  const auto frame = load_golden_frame();
  const std::string maximum_sender_id(128, 's');

  const auto result = receiver.receive(maximum_sender_id, frame);

  SYNC_REQUIRE(result.status == ReceiveStatus::Accepted);
  SYNC_REQUIRE(result.decode_error == DecodeError::None);
  SYNC_REQUIRE(publisher.records.size() == 1);
  SYNC_REQUIRE(publisher.records.front().sender_id == maximum_sender_id);
  const auto* stats = receiver.stats(maximum_sender_id);
  SYNC_REQUIRE(stats != nullptr);
  SYNC_REQUIRE(stats->accepted == 1);
  SYNC_REQUIRE(stats->dropped == 0);
  SYNC_REQUIRE(stats->rejected == 0);
  SYNC_REQUIRE(stats->failed == 0);
  SYNC_REQUIRE(stats->has_last_sequence);
  SYNC_REQUIRE(stats->last_sequence == 4294967301ULL);

  SYNC_REQUIRE(receiver.remove_sender(maximum_sender_id));
  SYNC_REQUIRE(receiver.stats(maximum_sender_id) == nullptr);
  SYNC_REQUIRE(!receiver.remove_sender(maximum_sender_id));
}

SYNC_TEST(receiver_drops_duplicate_and_lower_sequences_without_moving_timestamp) {
  RecordingPublisher publisher;
  FrameReceiver receiver(publisher);
  auto first = load_golden_frame();
  write_u64(first, 36, 50);
  write_u64(first, 44, 5000);
  auto duplicate = load_golden_frame();
  write_u64(duplicate, 36, 50);
  write_u64(duplicate, 44, 4000);
  auto lower = load_golden_frame();
  write_u64(lower, 36, 49);
  write_u64(lower, 44, 3000);

  SYNC_REQUIRE(receiver.receive("ordered-sender", first).status == ReceiveStatus::Accepted);
  SYNC_REQUIRE(receiver.receive("ordered-sender", duplicate).status ==
               ReceiveStatus::DroppedStale);
  SYNC_REQUIRE(receiver.receive("ordered-sender", lower).status == ReceiveStatus::DroppedStale);

  SYNC_REQUIRE(publisher.records.size() == 1);
  const auto* stats = receiver.stats("ordered-sender");
  SYNC_REQUIRE(stats != nullptr);
  SYNC_REQUIRE(stats->accepted == 1);
  SYNC_REQUIRE(stats->dropped == 2);
  SYNC_REQUIRE(stats->last_sequence == 50);
  SYNC_REQUIRE(stats->last_presentation_time_us == 5000);
}

SYNC_TEST(receiver_advances_the_watermark_before_backpressured_publication) {
  RecordingPublisher publisher({PublishResult::Backpressured, PublishResult::Accepted});
  FrameReceiver receiver(publisher);
  auto first = load_golden_frame();
  write_u64(first, 36, 70);
  write_u64(first, 44, 7000);
  auto duplicate = first;
  auto higher = load_golden_frame();
  write_u64(higher, 36, 71);
  write_u64(higher, 44, 7100);

  SYNC_REQUIRE(receiver.receive("pressure-sender", first).status ==
               ReceiveStatus::DroppedBackpressure);
  SYNC_REQUIRE(receiver.receive("pressure-sender", duplicate).status ==
               ReceiveStatus::DroppedStale);
  SYNC_REQUIRE(receiver.receive("pressure-sender", higher).status == ReceiveStatus::Accepted);

  SYNC_REQUIRE(publisher.records.size() == 2);
  SYNC_REQUIRE(publisher.records[0].sequence == 70);
  SYNC_REQUIRE(publisher.records[1].sequence == 71);
  const auto* stats = receiver.stats("pressure-sender");
  SYNC_REQUIRE(stats != nullptr);
  SYNC_REQUIRE(stats->accepted == 1);
  SYNC_REQUIRE(stats->dropped == 2);
  SYNC_REQUIRE(stats->last_sequence == 71);
  SYNC_REQUIRE(stats->last_presentation_time_us == 7100);
}

SYNC_TEST(receiver_completes_backpressure_and_failure_publication_synchronously) {
  RecordingPublisher publisher({PublishResult::Backpressured, PublishResult::Failed});
  FrameReceiver receiver(publisher);

  {
    auto frame = load_golden_frame();
    write_u64(frame, 36, 80);
    SYNC_REQUIRE(receiver.receive("transient-sender", frame).status ==
                 ReceiveStatus::DroppedBackpressure);
    for (std::byte& value : frame) {
      value = std::byte{0};
    }
  }
  {
    auto frame = load_golden_frame();
    write_u64(frame, 36, 81);
    SYNC_REQUIRE(receiver.receive("transient-sender", frame).status == ReceiveStatus::PublishFailed);
    for (std::byte& value : frame) {
      value = std::byte{0};
    }
  }

  SYNC_REQUIRE(publisher.records.size() == 2);
  SYNC_REQUIRE(publisher.records[0].payload_checksum == 1186813628U);
  SYNC_REQUIRE(publisher.records[1].payload_checksum == 1186813628U);
  const auto* stats = receiver.stats("transient-sender");
  SYNC_REQUIRE(stats != nullptr);
  SYNC_REQUIRE(stats->accepted == 0);
  SYNC_REQUIRE(stats->dropped == 1);
  SYNC_REQUIRE(stats->rejected == 0);
  SYNC_REQUIRE(stats->failed == 1);
  SYNC_REQUIRE(stats->last_sequence == 81);
}

SYNC_TEST(receiver_tracks_sender_statistics_independently) {
  RecordingPublisher publisher;
  FrameReceiver receiver(publisher);
  auto alpha = load_golden_frame();
  write_u64(alpha, 36, 10);
  write_u64(alpha, 44, 1000);
  auto beta = load_golden_frame();
  write_u64(beta, 36, 3);
  write_u64(beta, 44, 300);

  SYNC_REQUIRE(receiver.receive("alpha", alpha).status == ReceiveStatus::Accepted);
  SYNC_REQUIRE(receiver.receive("beta", beta).status == ReceiveStatus::Accepted);
  SYNC_REQUIRE(receiver.receive("alpha", alpha).status == ReceiveStatus::DroppedStale);

  const auto* alpha_stats = receiver.stats("alpha");
  const auto* beta_stats = receiver.stats("beta");
  SYNC_REQUIRE(alpha_stats != nullptr);
  SYNC_REQUIRE(beta_stats != nullptr);
  SYNC_REQUIRE(alpha_stats->accepted == 1);
  SYNC_REQUIRE(alpha_stats->dropped == 1);
  SYNC_REQUIRE(alpha_stats->last_sequence == 10);
  SYNC_REQUIRE(alpha_stats->last_presentation_time_us == 1000);
  SYNC_REQUIRE(beta_stats->accepted == 1);
  SYNC_REQUIRE(beta_stats->dropped == 0);
  SYNC_REQUIRE(beta_stats->last_sequence == 3);
  SYNC_REQUIRE(beta_stats->last_presentation_time_us == 300);
  SYNC_REQUIRE(receiver.stats("absent") == nullptr);
}

SYNC_TEST(receiver_applies_configured_protocol_limits) {
  RecordingPublisher publisher;
  FrameReceiver receiver(
      publisher, {.max_width = 1, .max_height = 2, .max_payload_bytes = 16});
  const auto frame = load_golden_frame();

  const auto result = receiver.receive("limited-sender", frame);

  SYNC_REQUIRE(result.status == ReceiveStatus::RejectedMalformed);
  SYNC_REQUIRE(result.decode_error == DecodeError::WidthLimitExceeded);
  SYNC_REQUIRE(publisher.records.empty());
  const auto* stats = receiver.stats("limited-sender");
  SYNC_REQUIRE(stats != nullptr);
  SYNC_REQUIRE(stats->rejected == 1);
  SYNC_REQUIRE(!stats->has_last_sequence);
}

SYNC_TEST(receiver_caps_sender_state_at_64_stable_entries) {
  RecordingPublisher publisher;
  FrameReceiver receiver(publisher);
  const auto frame = load_golden_frame();

  SYNC_REQUIRE(receiver.receive("sender-0", frame).status == ReceiveStatus::Accepted);
  const auto* first_stats = receiver.stats("sender-0");
  SYNC_REQUIRE(first_stats != nullptr);
  for (std::size_t index = 1; index < 64; ++index) {
    const std::string sender_id = "sender-" + std::to_string(index);
    SYNC_REQUIRE(receiver.receive(sender_id, frame).status == ReceiveStatus::Accepted);
  }
  SYNC_REQUIRE(publisher.records.size() == 64);
  SYNC_REQUIRE(receiver.stats("sender-0") == first_stats);

  const auto overflow = receiver.receive("sender-64", frame);

  SYNC_REQUIRE(overflow.status == ReceiveStatus::RejectedSender);
  SYNC_REQUIRE(overflow.decode_error == DecodeError::None);
  SYNC_REQUIRE(receiver.stats("sender-64") == nullptr);
  SYNC_REQUIRE(publisher.records.size() == 64);

  auto higher = load_golden_frame();
  write_u64(higher, 36, 4294967302ULL);
  SYNC_REQUIRE(receiver.receive("sender-0", higher).status == ReceiveStatus::Accepted);
  SYNC_REQUIRE(receiver.stats("sender-0") == first_stats);
  SYNC_REQUIRE(first_stats->accepted == 2);
  SYNC_REQUIRE(first_stats->last_sequence == 4294967302ULL);
  SYNC_REQUIRE(publisher.records.size() == 65);
}

SYNC_TEST(receiver_removal_is_idempotent_and_resets_sequence_and_statistics) {
  RecordingPublisher publisher;
  FrameReceiver receiver(publisher);
  auto higher = load_golden_frame();
  write_u64(higher, 36, 100);
  write_u64(higher, 44, 10000);
  auto fresh = load_golden_frame();
  write_u64(fresh, 36, 1);
  write_u64(fresh, 44, 100);
  const std::string oversized_sender(129, 's');

  SYNC_REQUIRE(!receiver.remove_sender(""));
  SYNC_REQUIRE(!receiver.remove_sender(oversized_sender));
  SYNC_REQUIRE(!receiver.remove_sender("missing"));
  SYNC_REQUIRE(receiver.receive("reusable", higher).status == ReceiveStatus::Accepted);
  SYNC_REQUIRE(receiver.stats("reusable")->last_sequence == 100);

  SYNC_REQUIRE(receiver.remove_sender("reusable"));
  SYNC_REQUIRE(receiver.stats("reusable") == nullptr);
  SYNC_REQUIRE(!receiver.remove_sender("reusable"));
  SYNC_REQUIRE(receiver.receive("reusable", fresh).status == ReceiveStatus::Accepted);

  const auto* reset = receiver.stats("reusable");
  SYNC_REQUIRE(reset != nullptr);
  SYNC_REQUIRE(reset->accepted == 1);
  SYNC_REQUIRE(reset->dropped == 0);
  SYNC_REQUIRE(reset->rejected == 0);
  SYNC_REQUIRE(reset->failed == 0);
  SYNC_REQUIRE(reset->has_last_sequence);
  SYNC_REQUIRE(reset->last_sequence == 1);
  SYNC_REQUIRE(reset->last_presentation_time_us == 100);
}

SYNC_TEST(receiver_removal_recycles_a_full_fixed_sender_table) {
  RecordingPublisher publisher;
  FrameReceiver receiver(publisher);
  const auto frame = load_golden_frame();

  for (std::size_t index = 0; index < 64; ++index) {
    SYNC_REQUIRE(receiver.receive("full-" + std::to_string(index), frame).status ==
                 ReceiveStatus::Accepted);
  }
  SYNC_REQUIRE(receiver.receive("replacement", frame).status == ReceiveStatus::RejectedSender);
  SYNC_REQUIRE(receiver.remove_sender("full-31"));
  SYNC_REQUIRE(receiver.stats("full-31") == nullptr);
  SYNC_REQUIRE(receiver.receive("replacement", frame).status == ReceiveStatus::Accepted);
  SYNC_REQUIRE(receiver.stats("replacement") != nullptr);
  SYNC_REQUIRE(receiver.stats("replacement")->accepted == 1);
}
