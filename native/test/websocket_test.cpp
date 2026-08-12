#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include <sync/websocket.hpp>
#include <sync/secure_memory.hpp>

namespace {

namespace ws = noisefactor::sync::websocket;

class RecordingCleanseObserver final
    : public noisefactor::sync::CleanseObserver {
 public:
  void after_cleanse(std::span<const std::byte> bytes) noexcept override {
    ++calls;
    cleansed_bytes += bytes.size();
    for (const std::byte byte : bytes) {
      if (byte != std::byte{0}) all_zero = false;
    }
  }

  void before_sensitive_fragment_reserve(
      std::size_t current_size,
      std::size_t requested_capacity) noexcept override {
    ++fragment_reserve_calls;
    fragment_current_size = current_size;
    fragment_requested_capacity = requested_capacity;
  }

  std::size_t calls = 0;
  std::size_t cleansed_bytes = 0;
  bool all_zero = true;
  std::size_t fragment_reserve_calls = 0;
  std::size_t fragment_current_size = 0;
  std::size_t fragment_requested_capacity = 0;
};

std::vector<std::byte> bytes(std::initializer_list<std::uint8_t> values) {
  std::vector<std::byte> result;
  result.reserve(values.size());
  for (const auto value : values) {
    result.push_back(static_cast<std::byte>(value));
  }
  return result;
}

std::uint8_t value(std::byte byte) {
  return std::to_integer<std::uint8_t>(byte);
}

void require_payload(const ws::Message& message, std::initializer_list<std::uint8_t> expected) {
  SYNC_REQUIRE(message.payload.size() == expected.size());
  std::size_t index = 0;
  for (const auto byte : expected) {
    SYNC_REQUIRE(value(message.payload[index++]) == byte);
  }
}

void require_http_error(std::string_view request, ws::HttpError error) {
  const auto result = ws::parse_upgrade_request(request);
  SYNC_REQUIRE(result.error == error);
  SYNC_REQUIRE(!result.request.has_value());
}

void require_decode_error(std::initializer_list<std::uint8_t> encoded,
                          ws::DecodeError error,
                          std::size_t limit = 1024) {
  ws::ClientFrameDecoder decoder(limit);
  std::vector<ws::Message> output;
  const auto frame = bytes(encoded);
  SYNC_REQUIRE(decoder.feed(frame, output) == error);
  SYNC_REQUIRE(output.empty());
}

std::vector<std::byte> masked_close(std::initializer_list<std::uint8_t> payload) {
  const std::array<std::uint8_t, 4> mask = {0x11, 0x22, 0x33, 0x44};
  std::vector<std::byte> frame = {
      std::byte{0x88},
      static_cast<std::byte>(0x80U | payload.size()),
      static_cast<std::byte>(mask[0]),
      static_cast<std::byte>(mask[1]),
      static_cast<std::byte>(mask[2]),
      static_cast<std::byte>(mask[3]),
  };
  std::size_t index = 0;
  for (const auto byte : payload) {
    frame.push_back(static_cast<std::byte>(byte ^ mask[index++ % mask.size()]));
  }
  return frame;
}

std::vector<std::byte> masked_frame(bool final,
                                    ws::Opcode opcode,
                                    std::string_view payload) {
  SYNC_REQUIRE(payload.size() <= 125);
  const std::array<std::uint8_t, 4> mask = {0x11, 0x22, 0x33, 0x44};
  std::vector<std::byte> frame = {
      static_cast<std::byte>((final ? 0x80U : 0U) |
                             static_cast<std::uint8_t>(opcode)),
      static_cast<std::byte>(0x80U | payload.size()),
      static_cast<std::byte>(mask[0]),
      static_cast<std::byte>(mask[1]),
      static_cast<std::byte>(mask[2]),
      static_cast<std::byte>(mask[3]),
  };
  for (std::size_t index = 0; index < payload.size(); ++index) {
    frame.push_back(static_cast<std::byte>(
        static_cast<std::uint8_t>(payload[index]) ^ mask[index % mask.size()]));
  }
  return frame;
}

std::vector<std::byte> masked_binary_frame(std::size_t payload_size) {
  const std::array<std::uint8_t, 4> mask = {0x11, 0x22, 0x33, 0x44};
  std::vector<std::byte> frame = {std::byte{0x82}};
  if (payload_size <= 65'535) {
    frame.push_back(std::byte{0xfe});
    frame.push_back(static_cast<std::byte>((payload_size >> 8U) & 0xffU));
    frame.push_back(static_cast<std::byte>(payload_size & 0xffU));
  } else {
    frame.push_back(std::byte{0xff});
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<std::byte>((payload_size >> shift) & 0xffU));
    }
  }
  for (const std::uint8_t byte : mask) {
    frame.push_back(static_cast<std::byte>(byte));
  }
  frame.reserve(frame.size() + payload_size);
  for (std::size_t index = 0; index < payload_size; ++index) {
    const auto value = static_cast<std::uint8_t>(index & 0xffU);
    frame.push_back(static_cast<std::byte>(value ^ mask[index % mask.size()]));
  }
  return frame;
}

void require_invalid_client_close(std::initializer_list<std::uint8_t> payload) {
  ws::ClientFrameDecoder decoder(256);
  std::vector<ws::Message> output;
  const auto frame = masked_close(payload);
  SYNC_REQUIRE(decoder.feed(frame, output) == ws::DecodeError::InvalidControlFrame);
  SYNC_REQUIRE(output.empty());
}

void require_invalid_server_close(std::initializer_list<std::uint8_t> payload) {
  const auto unencoded = bytes(payload);
  SYNC_REQUIRE(ws::encode_server_frame(ws::Opcode::Close, unencoded).empty());
}

std::string upgrade_with_subprotocol(std::string_view subprotocol) {
  std::string request =
      "GET /control HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Origin: https://example.test\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Sec-WebSocket-Key: AAECAwQFBgcICQoLDA0ODw==\r\n"
      "Sec-WebSocket-Protocol: ";
  request.append(subprotocol);
  request.append("\r\n\r\n");
  return request;
}

}  // namespace

SYNC_TEST(parses_a_complete_http_upgrade_request_and_rfc_accept_key) {
  constexpr std::string_view request =
      "GET /control?mode=test HTTP/1.1\r\n"
      "Host: 127.0.0.1:49152\r\n"
      "Origin: https://deck.example\r\n"
      "Upgrade: websocket\r\n"
      "Connection: keep-alive, Upgrade\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Protocol: sync.control.v1, sync.control.v2\r\n"
      "\r\n";

  const auto result = ws::parse_upgrade_request(request);

  SYNC_REQUIRE(result.error == ws::HttpError::None);
  SYNC_REQUIRE(result.request.has_value());
  SYNC_REQUIRE(result.request->path == "/control?mode=test");
  SYNC_REQUIRE(result.request->host == "127.0.0.1:49152");
  SYNC_REQUIRE(result.request->origin == "https://deck.example");
  SYNC_REQUIRE(result.request->key == "dGhlIHNhbXBsZSBub25jZQ==");
  SYNC_REQUIRE(result.request->subprotocols.size() == 2);
  SYNC_REQUIRE(result.request->subprotocols[0] == "sync.control.v1");
  SYNC_REQUIRE(result.request->subprotocols[1] == "sync.control.v2");
  SYNC_REQUIRE(ws::websocket_accept_key(result.request->key) ==
               "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

SYNC_TEST(http_header_names_tokens_and_whitespace_are_case_insensitive_or_trimmed) {
  constexpr std::string_view request =
      "GET /senders/a HTTP/1.1\r\n"
      "hOsT:\t localhost \t\r\n"
      "oRiGiN:  https://example.test  \r\n"
      "uPgRaDe: WebSocket\r\n"
      "cOnNeCtIoN: keep-alive,  uPgRaDe \r\n"
      "sEc-WeBsOcKeT-vErSiOn: 13\r\n"
      "sEc-WeBsOcKeT-kEy: AAECAwQFBgcICQoLDA0ODw==\r\n"
      "sEc-WeBsOcKeT-pRoToCoL: one ,\ttwo\r\n"
      "\r\n";

  const auto result = ws::parse_upgrade_request(request);

  SYNC_REQUIRE(result.error == ws::HttpError::None);
  SYNC_REQUIRE(result.request->host == "localhost");
  SYNC_REQUIRE(result.request->origin == "https://example.test");
  SYNC_REQUIRE(result.request->subprotocols.size() == 2);
  SYNC_REQUIRE(result.request->subprotocols[0] == "one");
  SYNC_REQUIRE(result.request->subprotocols[1] == "two");
}

SYNC_TEST(http_upgrade_reports_each_required_request_error) {
  require_http_error("POST /control HTTP/1.1\r\n\r\n", ws::HttpError::WrongMethod);
  require_http_error("GET /control HTTP/1.0\r\n\r\n", ws::HttpError::WrongVersion);
  require_http_error(
      "GET / HTTP/1.1\r\nConnection: Upgrade\r\nHost: h\r\nOrigin: o\r\n"
      "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n",
      ws::HttpError::MissingUpgrade);
  require_http_error(
      "GET / HTTP/1.1\r\nUpgrade: websocket\r\nHost: h\r\nOrigin: o\r\n"
      "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n",
      ws::HttpError::MissingConnectionUpgrade);
  require_http_error(
      "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nOrigin: o\r\n"
      "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n",
      ws::HttpError::MissingHost);
  require_http_error(
      "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nHost: h\r\n"
      "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n",
      ws::HttpError::MissingOrigin);
  require_http_error(
      "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nHost: h\r\nOrigin: o\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n",
      ws::HttpError::MissingKey);
  require_http_error(
      "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nHost: h\r\nOrigin: o\r\n"
      "Sec-WebSocket-Version: 12\r\nSec-WebSocket-Key: AAECAwQFBgcICQoLDA0ODw==\r\n\r\n",
      ws::HttpError::UnsupportedWebSocketVersion);
}

SYNC_TEST(http_upgrade_rejects_oversize_malformed_and_invalid_keys) {
  require_http_error(std::string(ws::kMaximumHttpUpgradeBytes + 1, 'x'),
                     ws::HttpError::TooLarge);
  require_http_error("GET / HTTP/1.1\nHost: h\n\n", ws::HttpError::Malformed);
  require_http_error("GET / HTTP/1.1\r\nBroken\r\n\r\n", ws::HttpError::Malformed);
  require_http_error("GET / HTTP/1.1 extra\r\n\r\n", ws::HttpError::Malformed);
  require_http_error("GET / HTTP/1.1\r\n\r\n\r\n", ws::HttpError::Malformed);
  require_http_error(
      std::string("GET /bad") + static_cast<char>(0x01) + " HTTP/1.1\r\n\r\n",
      ws::HttpError::Malformed);
  require_http_error(
      "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nHost: h\r\nOrigin: o\r\n"
      "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: not-base64!!!!\r\n\r\n",
      ws::HttpError::InvalidKey);
  require_http_error(
      "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nHost: h\r\nOrigin: o\r\n"
      "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: AAECAwQFBgcICQoLDA0ODx==\r\n\r\n",
      ws::HttpError::InvalidKey);
  require_http_error(
      "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nHost: h\r\nOrigin: o\r\n"
      "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: AAECAwQFBgcICQoLDA0ODw==\r\n"
      "Sec-WebSocket-Protocol: one,\r\n\r\n",
      ws::HttpError::Malformed);
  SYNC_REQUIRE(ws::websocket_accept_key("too-short") == "");
}

SYNC_TEST(http_upgrade_accepts_only_rfc_tokens_as_subprotocol_items) {
  const auto valid = ws::parse_upgrade_request(
      upgrade_with_subprotocol("sync.sender.ticket-token_123, \tsecond+token"));
  SYNC_REQUIRE(valid.error == ws::HttpError::None);
  SYNC_REQUIRE(valid.request->subprotocols.size() == 2);
  SYNC_REQUIRE(valid.request->subprotocols[0] == "sync.sender.ticket-token_123");
  SYNC_REQUIRE(valid.request->subprotocols[1] == "second+token");

  const std::array invalid = {
      std::string("bad token"),
      std::string("bad\"token"),
      std::string("bad/token"),
      std::string("bad(token)"),
      std::string("bad;token"),
      std::string("bad") + static_cast<char>(0x01) + "token",
      std::string("\xc3\xa9"),
  };
  for (const auto& subprotocol : invalid) {
    require_http_error(upgrade_with_subprotocol(subprotocol), ws::HttpError::Malformed);
  }
}

SYNC_TEST(client_decoder_rejects_a_zero_message_limit) {
  bool threw = false;
  try {
    ws::ClientFrameDecoder decoder(0);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  SYNC_REQUIRE(threw);
}

SYNC_TEST(client_decoder_accepts_text_and_binary_frames_one_tcp_byte_at_a_time) {
  const auto input = bytes({
      0x81, 0x82, 0x01, 0x02, 0x03, 0x04, 0x69, 0x6b,
      0x82, 0x83, 0xaa, 0xbb, 0xcc, 0xdd, 0xaa, 0xba, 0xce,
  });
  ws::ClientFrameDecoder decoder(32);
  std::vector<ws::Message> output;

  for (const auto byte : input) {
    const std::array one{byte};
    SYNC_REQUIRE(decoder.feed(one, output) == ws::DecodeError::None);
  }

  SYNC_REQUIRE(output.size() == 2);
  SYNC_REQUIRE(output[0].opcode == ws::Opcode::Text);
  require_payload(output[0], {'h', 'i'});
  SYNC_REQUIRE(output[1].opcode == ws::Opcode::Binary);
  require_payload(output[1], {0x00, 0x01, 0x02});
}

SYNC_TEST(client_decoder_accepts_literal_125_and_126_byte_lengths) {
  std::vector<std::byte> input = bytes({0x82, 0xfd, 0x01, 0x02, 0x03, 0x04});
  for (std::size_t index = 0; index < 125; ++index) {
    input.push_back(static_cast<std::byte>(0x5aU ^ (index % 4 == 0 ? 1U : index % 4 == 1 ? 2U : index % 4 == 2 ? 3U : 4U)));
  }
  const auto second_header = bytes({0x82, 0xfe, 0x00, 0x7e, 0x05, 0x06, 0x07, 0x08});
  input.insert(input.end(), second_header.begin(), second_header.end());
  for (std::size_t index = 0; index < 126; ++index) {
    const std::array mask = {0x05U, 0x06U, 0x07U, 0x08U};
    input.push_back(static_cast<std::byte>(0xa5U ^ mask[index % 4]));
  }

  ws::ClientFrameDecoder decoder(126);
  std::vector<ws::Message> output;
  SYNC_REQUIRE(decoder.feed(input, output) == ws::DecodeError::None);
  SYNC_REQUIRE(output.size() == 2);
  SYNC_REQUIRE(output[0].payload.size() == 125);
  SYNC_REQUIRE(output[1].payload.size() == 126);
  SYNC_REQUIRE(value(output[0].payload.front()) == 0x5a);
  SYNC_REQUIRE(value(output[1].payload.back()) == 0xa5);
}

SYNC_TEST(client_decoder_accepts_a_literal_64_bit_65536_byte_length) {
  std::vector<std::byte> input = bytes({
      0x82, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
      0x10, 0x20, 0x30, 0x40,
  });
  const std::array mask = {0x10U, 0x20U, 0x30U, 0x40U};
  for (std::size_t index = 0; index < 65536; ++index) {
    input.push_back(static_cast<std::byte>(0xc3U ^ mask[index % 4]));
  }

  ws::ClientFrameDecoder decoder(65536);
  std::vector<ws::Message> output;
  SYNC_REQUIRE(decoder.feed(input, output) == ws::DecodeError::None);
  SYNC_REQUIRE(output.size() == 1);
  SYNC_REQUIRE(output[0].payload.size() == 65536);
  SYNC_REQUIRE(value(output[0].payload.front()) == 0xc3);
  SYNC_REQUIRE(value(output[0].payload.back()) == 0xc3);
}

SYNC_TEST(client_decoder_preserves_mask_alignment_across_large_tcp_chunks) {
  constexpr std::size_t kPayloadBytes = 100'003;
  const auto input = masked_binary_frame(kPayloadBytes);
  ws::ClientFrameDecoder decoder(kPayloadBytes);
  std::vector<ws::Message> output;
  constexpr std::array<std::size_t, 7> chunk_sizes = {1, 2, 7, 16'381, 5, 32'769, 8191};
  std::size_t offset = 0;
  std::size_t chunk_index = 0;
  while (offset < input.size()) {
    const std::size_t count =
        std::min(chunk_sizes[chunk_index++ % chunk_sizes.size()], input.size() - offset);
    SYNC_REQUIRE(decoder.feed(std::span(input).subspan(offset, count), output) ==
                 ws::DecodeError::None);
    offset += count;
  }

  SYNC_REQUIRE(output.size() == 1);
  SYNC_REQUIRE(output[0].payload.size() == kPayloadBytes);
  for (std::size_t index = 0; index < output[0].payload.size(); ++index) {
    SYNC_REQUIRE(value(output[0].payload[index]) == (index & 0xffU));
  }
}

SYNC_TEST(client_decoder_reuses_recycled_nonsensitive_message_storage) {
  constexpr std::size_t kPayloadBytes = 65'537;
  const auto input = masked_binary_frame(kPayloadBytes);
  ws::ClientFrameDecoder decoder(kPayloadBytes);
  std::vector<ws::Message> output;

  SYNC_REQUIRE(decoder.feed(input, output) == ws::DecodeError::None);
  SYNC_REQUIRE(output.size() == 1);
  const std::byte* const first_storage = output[0].payload.data();
  decoder.recycle_payload(output[0].payload);
  SYNC_REQUIRE(output[0].payload.empty());

  output.clear();
  SYNC_REQUIRE(decoder.feed(input, output) == ws::DecodeError::None);
  SYNC_REQUIRE(output.size() == 1);
  SYNC_REQUIRE(output[0].payload.data() == first_storage);
  SYNC_REQUIRE(value(output[0].payload[65'536]) == 0);
}

SYNC_TEST(client_decoder_does_not_retain_recycled_storage_above_its_reuse_limit) {
  constexpr std::size_t kPayloadBytes = 257;
  const auto input = masked_binary_frame(kPayloadBytes);
  ws::ClientFrameDecoder decoder(
      1024, ws::PayloadSensitivity::NonSensitive, nullptr, 256);
  std::vector<ws::Message> output;

  SYNC_REQUIRE(decoder.feed(input, output) == ws::DecodeError::None);
  SYNC_REQUIRE(output.size() == 1);
  decoder.recycle_payload(output[0].payload);

  SYNC_REQUIRE(output[0].payload.size() == kPayloadBytes);
}

SYNC_TEST(client_decoder_rejects_text_payloads_that_are_not_utf8) {
  // RFC 6455 section 5.6. Binary carries arbitrary bytes; text does not.
  const std::string invalid("bad\xC0\xAFtext");
  ws::ClientFrameDecoder text_decoder(1024);
  std::vector<ws::Message> output;
  SYNC_REQUIRE(text_decoder.feed(masked_frame(true, ws::Opcode::Text, invalid),
                                 output) == ws::DecodeError::InvalidTextPayload);
  SYNC_REQUIRE(output.empty());

  ws::ClientFrameDecoder binary_decoder(1024);
  output.clear();
  SYNC_REQUIRE(binary_decoder.feed(masked_frame(true, ws::Opcode::Binary, invalid),
                                   output) == ws::DecodeError::None);
  SYNC_REQUIRE(output.size() == 1);

  // A split sequence is only judged once the message is whole, so valid text
  // that straddles a fragment boundary still decodes.
  ws::ClientFrameDecoder split_decoder(1024);
  output.clear();
  SYNC_REQUIRE(split_decoder.feed(masked_frame(false, ws::Opcode::Text, "\xE2\x82"),
                                  output) == ws::DecodeError::None);
  SYNC_REQUIRE(split_decoder.feed(masked_frame(true, ws::Opcode::Continuation, "\xAC ok"),
                                  output) == ws::DecodeError::None);
  SYNC_REQUIRE(output.size() == 1);
  SYNC_REQUIRE(output[0].payload.size() == 6);

  // The same rule applies to a message assembled from fragments.
  ws::ClientFrameDecoder fragmented_decoder(1024);
  output.clear();
  SYNC_REQUIRE(fragmented_decoder.feed(masked_frame(false, ws::Opcode::Text, "ok"),
                                       output) == ws::DecodeError::None);
  SYNC_REQUIRE(fragmented_decoder.feed(
                   masked_frame(true, ws::Opcode::Continuation, "\xED\xA0\x80"),
                   output) == ws::DecodeError::InvalidTextPayload);
  SYNC_REQUIRE(output.empty());
}

SYNC_TEST(fragment_capacity_grows_geometrically_within_the_message_limit) {
  constexpr std::size_t kMaximum = 64U * 1024U * 1024U;
  // Never below what the next fragment needs, never above the message limit.
  SYNC_REQUIRE(ws::next_fragment_capacity(0, 1, kMaximum) ==
               ws::kMinimumFragmentCapacityBytes);
  SYNC_REQUIRE(ws::next_fragment_capacity(4096, 4097, kMaximum) == 8192);
  SYNC_REQUIRE(ws::next_fragment_capacity(8192, 8193, kMaximum) == 16384);
  SYNC_REQUIRE(ws::next_fragment_capacity(0, kMaximum, kMaximum) == kMaximum);
  SYNC_REQUIRE(ws::next_fragment_capacity(kMaximum / 2, kMaximum / 2 + 1,
                                          kMaximum) == kMaximum);
  // A decoder whose limit is under the growth floor never over-reserves.
  SYNC_REQUIRE(ws::next_fragment_capacity(0, 32, 128) == 128);

  // Reaching a target from zero must take a logarithmic number of steps, which
  // is what bounds total copying while a fragmented message is reassembled.
  std::size_t capacity = 0;
  std::size_t steps = 0;
  while (capacity < 1U << 20U) {
    capacity = ws::next_fragment_capacity(capacity, capacity + 1, kMaximum);
    ++steps;
    SYNC_REQUIRE(steps < 32);
  }
}

SYNC_TEST(client_decoder_reassembles_many_small_fragments_without_quadratic_copying) {
  constexpr std::size_t kFragments = 512;
  constexpr std::size_t kFragmentBytes = 64;
  const std::string chunk(kFragmentBytes, 'q');

  ws::ClientFrameDecoder decoder(1U << 20U, ws::PayloadSensitivity::NonSensitive);
  std::vector<ws::Message> output;
  for (std::size_t index = 0; index < kFragments; ++index) {
    const bool final = index + 1 == kFragments;
    const ws::Opcode opcode =
        index == 0 ? ws::Opcode::Binary : ws::Opcode::Continuation;
    SYNC_REQUIRE(decoder.feed(masked_frame(final, opcode, chunk), output) ==
                 ws::DecodeError::None);
    // Reserving only what the next fragment needs leaves capacity tracking size
    // exactly, which reallocates and copies on every continuation frame. The
    // final frame moves the buffer out, so it has no capacity to check.
    if (!final) {
      SYNC_REQUIRE(decoder.fragment_capacity() >=
                   ws::kMinimumFragmentCapacityBytes);
    }
  }

  SYNC_REQUIRE(output.size() == 1);
  SYNC_REQUIRE(output[0].opcode == ws::Opcode::Binary);
  SYNC_REQUIRE(output[0].payload.size() == kFragments * kFragmentBytes);
  for (const std::byte byte : output[0].payload) {
    SYNC_REQUIRE(byte == std::byte{'q'});
  }
}

SYNC_TEST(client_decoder_consumes_recycled_storage_for_a_fragmented_message) {
  const auto complete = masked_frame(
      true, ws::Opcode::Binary, std::string(64, 'x'));
  ws::ClientFrameDecoder decoder(
      128, ws::PayloadSensitivity::NonSensitive, nullptr, 128);
  std::vector<ws::Message> output;

  SYNC_REQUIRE(decoder.feed(complete, output) == ws::DecodeError::None);
  SYNC_REQUIRE(output.size() == 1);
  const std::byte* const recycled_storage = output[0].payload.data();
  decoder.recycle_payload(output[0].payload);
  output.clear();

  const auto first = masked_frame(false, ws::Opcode::Binary,
                                  "0123456789abcdefghijklmnopqrstuv");
  const auto final = masked_frame(true, ws::Opcode::Continuation,
                                  "wxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01");
  SYNC_REQUIRE(decoder.feed(first, output) == ws::DecodeError::None);
  SYNC_REQUIRE(decoder.feed(final, output) == ws::DecodeError::None);
  SYNC_REQUIRE(output.size() == 1);
  SYNC_REQUIRE(output[0].payload.size() == 64);
  SYNC_REQUIRE(output[0].payload.data() == recycled_storage);
}

SYNC_TEST(client_decoder_reassembles_fragmented_binary_around_an_interleaved_ping) {
  const auto input = bytes({
      0x02, 0x82, 0x01, 0x02, 0x03, 0x04, 0x11, 0x22,
      0x89, 0x81, 0x05, 0x06, 0x07, 0x08, 0x7f,
      0x80, 0x82, 0x09, 0x0a, 0x0b, 0x0c, 0x3a, 0x4e,
  });
  ws::ClientFrameDecoder decoder(8);
  std::vector<ws::Message> output;

  SYNC_REQUIRE(decoder.feed(input, output) == ws::DecodeError::None);

  SYNC_REQUIRE(output.size() == 2);
  SYNC_REQUIRE(output[0].opcode == ws::Opcode::Ping);
  require_payload(output[0], {'z'});
  SYNC_REQUIRE(output[1].opcode == ws::Opcode::Binary);
  require_payload(output[1], {0x10, 0x20, 0x33, 0x44});
}

SYNC_TEST(client_decoder_emits_final_close_control_frames) {
  const auto input = bytes({0x88, 0x82, 0x12, 0x34, 0x56, 0x78, 0x11, 0xdc});
  ws::ClientFrameDecoder decoder(16);
  std::vector<ws::Message> output;
  SYNC_REQUIRE(decoder.feed(input, output) == ws::DecodeError::None);
  SYNC_REQUIRE(output.size() == 1);
  SYNC_REQUIRE(output[0].opcode == ws::Opcode::Close);
  require_payload(output[0], {0x03, 0xe8});
}

SYNC_TEST(client_decoder_rejects_one_byte_and_endpoint_forbidden_close_codes) {
  require_invalid_client_close({0x03});
  require_invalid_client_close({0x03, 0xe7});  // 999
  require_invalid_client_close({0x03, 0xec});  // 1004
  require_invalid_client_close({0x03, 0xed});  // 1005
  require_invalid_client_close({0x03, 0xee});  // 1006
  require_invalid_client_close({0x03, 0xf7});  // 1015
  require_invalid_client_close({0x03, 0xf8});  // 1016
  require_invalid_client_close({0x0b, 0xb7});  // 2999
  require_invalid_client_close({0x13, 0x88});  // 5000
}

SYNC_TEST(client_decoder_rejects_malformed_utf8_close_reasons) {
  require_invalid_client_close({0x03, 0xe8, 0x80});
  require_invalid_client_close({0x03, 0xe8, 0xc2});
  require_invalid_client_close({0x03, 0xe8, 0xc0, 0xaf});
  require_invalid_client_close({0x03, 0xe8, 0xed, 0xa0, 0x80});
  require_invalid_client_close({0x03, 0xe8, 0xf4, 0x90, 0x80, 0x80});
}

SYNC_TEST(client_decoder_accepts_empty_and_endpoint_valid_close_payloads) {
  const std::array valid_frames = {
      masked_close({}),
      masked_close({0x03, 0xe8}),
      masked_close({0x03, 0xf6, 0xf0, 0x9f, 0x92, 0xa5}),  // 1014, U+1F4A5
      masked_close({0x0b, 0xb8, 0x01, 0xc2, 0x80}),        // 3000, C0/C1 controls
      masked_close({0x13, 0x87}),                          // 4999
  };

  for (const auto& frame : valid_frames) {
    ws::ClientFrameDecoder decoder(256);
    std::vector<ws::Message> output;
    SYNC_REQUIRE(decoder.feed(frame, output) == ws::DecodeError::None);
    SYNC_REQUIRE(output.size() == 1);
    SYNC_REQUIRE(output[0].opcode == ws::Opcode::Close);
  }
}

SYNC_TEST(client_decoder_emits_pong_control_frames) {
  const auto input = bytes({0x8a, 0x81, 0x01, 0x02, 0x03, 0x04, 0x70});
  ws::ClientFrameDecoder decoder(16);
  std::vector<ws::Message> output;
  SYNC_REQUIRE(decoder.feed(input, output) == ws::DecodeError::None);
  SYNC_REQUIRE(output.size() == 1);
  SYNC_REQUIRE(output[0].opcode == ws::Opcode::Pong);
  require_payload(output[0], {'q'});
}

SYNC_TEST(client_decoder_rejects_unmasked_reserved_and_unsupported_frames) {
  require_decode_error({0x81, 0x00}, ws::DecodeError::UnmaskedClientFrame);
  require_decode_error({0xc1, 0x80, 1, 2, 3, 4}, ws::DecodeError::ReservedBits);
  require_decode_error({0x83, 0x80, 1, 2, 3, 4}, ws::DecodeError::UnsupportedOpcode);
}

SYNC_TEST(client_decoder_rejects_fragmented_or_oversized_control_frames) {
  require_decode_error({0x09, 0x80, 1, 2, 3, 4}, ws::DecodeError::InvalidControlFrame);
  require_decode_error({0x89, 0xfe, 0x00, 0x7e, 1, 2, 3, 4},
                       ws::DecodeError::InvalidControlFrame);
}

SYNC_TEST(client_decoder_rejects_invalid_fragment_sequences) {
  require_decode_error({0x80, 0x80, 1, 2, 3, 4}, ws::DecodeError::UnexpectedContinuation);

  ws::ClientFrameDecoder decoder(16);
  std::vector<ws::Message> output;
  const auto input = bytes({
      0x01, 0x81, 1, 2, 3, 4, 0x60,
      0x82, 0x81, 5, 6, 7, 8, 0x67,
  });
  SYNC_REQUIRE(decoder.feed(input, output) == ws::DecodeError::FragmentAlreadyOpen);
  SYNC_REQUIRE(output.empty());
}

SYNC_TEST(client_decoder_rejects_message_limits_from_headers_before_payload_arrives) {
  ws::ClientFrameDecoder decoder(4);
  std::vector<ws::Message> output;
  const auto oversized = bytes({0x82, 0xfe, 0x01, 0x00, 1, 2, 3, 4});

  SYNC_REQUIRE(decoder.feed(oversized, output) == ws::DecodeError::MessageTooLarge);
  SYNC_REQUIRE(output.empty());
}

SYNC_TEST(client_decoder_rejects_fragment_aggregate_limits_before_continuation_payload) {
  ws::ClientFrameDecoder decoder(4);
  std::vector<ws::Message> output;
  const auto first = bytes({0x02, 0x83, 1, 2, 3, 4, 0x60, 0x60, 0x60});
  const auto continuation_header = bytes({0x80, 0x82, 5, 6, 7, 8});

  SYNC_REQUIRE(decoder.feed(first, output) == ws::DecodeError::None);
  SYNC_REQUIRE(decoder.feed(continuation_header, output) == ws::DecodeError::MessageTooLarge);
  SYNC_REQUIRE(output.empty());
}

SYNC_TEST(client_decoder_rejects_noncanonical_or_invalid_64_bit_lengths) {
  require_decode_error({0x82, 0xfe, 0x00, 0x7d, 1, 2, 3, 4},
                       ws::DecodeError::InvalidLengthEncoding);
  require_decode_error({0x82, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff,
                        1, 2, 3, 4},
                       ws::DecodeError::InvalidLengthEncoding, 65536);
  require_decode_error({0x82, 0xff, 0x80, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
                        1, 2, 3, 4},
                       ws::DecodeError::InvalidLengthEncoding, 65536);
}

SYNC_TEST(client_decoder_enters_terminal_state_after_the_first_error) {
  ws::ClientFrameDecoder decoder(16);
  std::vector<ws::Message> output;
  const auto bad = bytes({0x81, 0x00});
  const auto good = bytes({0x81, 0x81, 1, 2, 3, 4, 0x79});

  SYNC_REQUIRE(decoder.feed(bad, output) == ws::DecodeError::UnmaskedClientFrame);
  SYNC_REQUIRE(decoder.feed(good, output) == ws::DecodeError::Terminal);
  SYNC_REQUIRE(output.empty());
}

SYNC_TEST(client_decoder_cleanses_fragmented_and_mid_frame_storage) {
  RecordingCleanseObserver observer;
  {
    ws::ClientFrameDecoder decoder(
        256, ws::PayloadSensitivity::Sensitive, &observer);
    std::vector<ws::Message> output;
    const auto first = masked_frame(false, ws::Opcode::Text, "secret-fragment");
    SYNC_REQUIRE(decoder.feed(first, output) == ws::DecodeError::None);
    const auto malformed = bytes({0x80, 0x00});
    SYNC_REQUIRE(decoder.feed(malformed, output) ==
                 ws::DecodeError::UnmaskedClientFrame);
  }
  SYNC_REQUIRE(observer.calls >= 1);
  SYNC_REQUIRE(observer.cleansed_bytes >= std::string_view("secret-fragment").size());
  SYNC_REQUIRE(observer.all_zero);

  const std::size_t before_destruction = observer.cleansed_bytes;
  {
    ws::ClientFrameDecoder decoder(
        256, ws::PayloadSensitivity::Sensitive, &observer);
    std::vector<ws::Message> output;
    auto partial = masked_frame(true, ws::Opcode::Text, "mid-frame-secret");
    partial.resize(partial.size() - 5);
    SYNC_REQUIRE(decoder.feed(partial, output) == ws::DecodeError::None);
    SYNC_REQUIRE(output.empty());
  }
  SYNC_REQUIRE(observer.cleansed_bytes > before_destruction);
  SYNC_REQUIRE(observer.all_zero);
}

SYNC_TEST(sensitive_fragment_storage_is_fixed_before_secret_bytes_arrive) {
  RecordingCleanseObserver observer;
  ws::ClientFrameDecoder decoder(
      256, ws::PayloadSensitivity::Sensitive, &observer);
  std::vector<ws::Message> output;
  const auto first = masked_frame(false, ws::Opcode::Text, "token-part-one");
  const auto second =
      masked_frame(false, ws::Opcode::Continuation, "-part-two");
  const auto third = masked_frame(true, ws::Opcode::Continuation, "-done");

  SYNC_REQUIRE(decoder.feed(first, output) == ws::DecodeError::None);
  SYNC_REQUIRE(decoder.feed(second, output) == ws::DecodeError::None);
  SYNC_REQUIRE(decoder.feed(third, output) == ws::DecodeError::None);
  SYNC_REQUIRE(output.size() == 1);
  SYNC_REQUIRE(observer.fragment_reserve_calls == 1);
  SYNC_REQUIRE(observer.fragment_current_size == 0);
  SYNC_REQUIRE(observer.fragment_requested_capacity == 256);
  ws::cleanse_message_payloads(output, &observer);
  SYNC_REQUIRE(observer.all_zero);

  RecordingCleanseObserver nonsensitive_observer;
  ws::ClientFrameDecoder nonsensitive(
      64U * 1024U * 1024U, ws::PayloadSensitivity::NonSensitive,
      &nonsensitive_observer);
  std::vector<ws::Message> nonsensitive_output;
  SYNC_REQUIRE(nonsensitive.feed(first, nonsensitive_output) ==
               ws::DecodeError::None);
  SYNC_REQUIRE(nonsensitive_observer.fragment_reserve_calls == 0);
}

SYNC_TEST(sensitive_message_scope_cleanses_completed_hello_during_unwind) {
  RecordingCleanseObserver observer;
  constexpr std::string_view hello_json =
      R"({"type":"hello","token":"secret","protocolVersions":[1]})";
  try {
    std::vector<ws::Message> output;
    ws::MessagePayloadGuard guard(
        output, ws::PayloadSensitivity::Sensitive, &observer);
    ws::ClientFrameDecoder decoder(
        512, ws::PayloadSensitivity::Sensitive, &observer);
    const auto hello = masked_frame(true, ws::Opcode::Text, hello_json);
    SYNC_REQUIRE(decoder.feed(hello, output) == ws::DecodeError::None);
    SYNC_REQUIRE(output.size() == 1);
    throw std::bad_alloc();
  } catch (const std::bad_alloc&) {
  }
  SYNC_REQUIRE(observer.cleansed_bytes >= hello_json.size());
  SYNC_REQUIRE(observer.all_zero);
}

SYNC_TEST(completed_sensitive_messages_can_be_cleansed_after_a_later_decode_error) {
  RecordingCleanseObserver observer;
  ws::ClientFrameDecoder decoder(
      512, ws::PayloadSensitivity::Sensitive, &observer);
  std::vector<ws::Message> output;
  auto input = masked_frame(
      true, ws::Opcode::Text,
      R"({"type":"hello","token":"secret","protocolVersions":[1]})");
  const auto malformed = bytes({0x81, 0x00});
  input.insert(input.end(), malformed.begin(), malformed.end());
  SYNC_REQUIRE(decoder.feed(input, output) ==
               ws::DecodeError::UnmaskedClientFrame);
  SYNC_REQUIRE(output.size() == 1);
  ws::cleanse_message_payloads(output, &observer);
  SYNC_REQUIRE(output[0].payload.empty());
  SYNC_REQUIRE(observer.cleansed_bytes > 0);
  SYNC_REQUIRE(observer.all_zero);
}

SYNC_TEST(server_encoder_emits_exact_literal_headers_for_all_length_classes) {
  const auto empty = ws::encode_server_frame(ws::Opcode::Text, {});
  SYNC_REQUIRE(empty == bytes({0x81, 0x00}));

  const std::vector<std::byte> p125(125, std::byte{0x11});
  const auto f125 = ws::encode_server_frame(ws::Opcode::Binary, p125);
  SYNC_REQUIRE(f125.size() == 127);
  SYNC_REQUIRE(value(f125[0]) == 0x82);
  SYNC_REQUIRE(value(f125[1]) == 0x7d);

  const std::vector<std::byte> p126(126, std::byte{0x22});
  const auto f126 = ws::encode_server_frame(ws::Opcode::Binary, p126);
  SYNC_REQUIRE(f126.size() == 130);
  SYNC_REQUIRE(value(f126[0]) == 0x82);
  SYNC_REQUIRE(value(f126[1]) == 0x7e);
  SYNC_REQUIRE(value(f126[2]) == 0x00);
  SYNC_REQUIRE(value(f126[3]) == 0x7e);

  const std::vector<std::byte> p65536(65536, std::byte{0x33});
  const auto f65536 = ws::encode_server_frame(ws::Opcode::Binary, p65536);
  const auto expected_header = bytes({0x82, 0x7f, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0x01, 0x00, 0x00});
  SYNC_REQUIRE(f65536.size() == 65546);
  for (std::size_t index = 0; index < expected_header.size(); ++index) {
    SYNC_REQUIRE(f65536[index] == expected_header[index]);
  }
}

SYNC_TEST(server_encoder_rejects_invalid_opcodes_and_oversized_control_payloads) {
  const std::vector<std::byte> oversized(126, std::byte{0});
  SYNC_REQUIRE(ws::encode_server_frame(ws::Opcode::Ping, oversized).empty());
  SYNC_REQUIRE(ws::encode_server_frame(static_cast<ws::Opcode>(0x3), {}).empty());
}

SYNC_TEST(server_encoder_rejects_invalid_close_codes_and_reasons) {
  require_invalid_server_close({0x03});
  require_invalid_server_close({0x03, 0xe7});
  require_invalid_server_close({0x03, 0xec});
  require_invalid_server_close({0x03, 0xee});
  require_invalid_server_close({0x03, 0xf7});
  require_invalid_server_close({0x03, 0xf8});
  require_invalid_server_close({0x0b, 0xb7});
  require_invalid_server_close({0x13, 0x88});
  require_invalid_server_close({0x03, 0xe8, 0x80});
  require_invalid_server_close({0x03, 0xe8, 0xc2});
  require_invalid_server_close({0x03, 0xe8, 0xc0, 0xaf});
  require_invalid_server_close({0x03, 0xe8, 0xed, 0xa0, 0x80});
  require_invalid_server_close({0x03, 0xe8, 0xf4, 0x90, 0x80, 0x80});
}

SYNC_TEST(server_encoder_accepts_empty_code_and_valid_utf8_close_reasons) {
  SYNC_REQUIRE(ws::encode_server_frame(ws::Opcode::Close, {}) == bytes({0x88, 0x00}));
  SYNC_REQUIRE(ws::encode_server_frame(ws::Opcode::Close, bytes({0x03, 0xe8})) ==
               bytes({0x88, 0x02, 0x03, 0xe8}));
  SYNC_REQUIRE(ws::encode_server_frame(
                   ws::Opcode::Close,
                   bytes({0x03, 0xf6, 0xf0, 0x9f, 0x92, 0xa5})) ==
               bytes({0x88, 0x06, 0x03, 0xf6, 0xf0, 0x9f, 0x92, 0xa5}));
  SYNC_REQUIRE(!ws::encode_server_frame(
                    ws::Opcode::Close,
                    bytes({0x0b, 0xb8, 0x01, 0xc2, 0x80}))
                    .empty());
}
