#include "test_harness.hpp"

#include <array>
#include <string>
#include <string_view>

#include <sync/origin.hpp>

namespace {

using noisefactor::sync::OriginError;
using noisefactor::sync::normalize_origin;

void require_normalized(std::string_view input, std::string_view expected) {
  const auto result = normalize_origin(input);
  SYNC_REQUIRE(result.ok());
  SYNC_REQUIRE(result.origin.view() == expected);
}

void require_rejected(std::string_view input, OriginError expected = OriginError::Malformed) {
  const auto result = normalize_origin(input);
  SYNC_REQUIRE(!result.ok());
  SYNC_REQUIRE(result.error == expected);
  SYNC_REQUIRE(result.origin.view().empty());
}

}  // namespace

SYNC_TEST(origin_normalizes_https_hosts_and_default_ports_deterministically) {
  require_normalized("HTTPS://Deck.Example:443", "https://deck.example");
  require_normalized("https://Deck.Example:8443", "https://deck.example:8443");
  require_normalized("https://127.0.0.1:443", "https://127.0.0.1");
  require_normalized("https://[::1]:443", "https://[::1]");
  require_normalized("https://[2001:0db8:0:0:0:0:0:1]", "https://[2001:db8::1]");
  require_normalized("https://[::ffff:192.168.0.1]", "https://[::ffff:c0a8:1]");
  require_normalized("https://[::ffff:c0a8:1]", "https://[::ffff:c0a8:1]");
  require_normalized("https://[::192.0.2.1]", "https://[::c000:201]");
  require_normalized("https://[::c000:201]", "https://[::c000:201]");
  require_normalized("https://[64:ff9b::192.0.2.1]", "https://[64:ff9b::c000:201]");
}

SYNC_TEST(origin_accepts_only_trustworthy_http_loopback_forms) {
  require_normalized("HTTP://LOCALHOST:80", "http://localhost");
  require_normalized("http://localhost:53979", "http://localhost:53979");
  require_normalized("http://127.0.0.1", "http://127.0.0.1");
  require_normalized("http://127.42.0.9", "http://127.42.0.9");
  require_normalized("http://studio.localhost", "http://studio.localhost");
  require_normalized("http://[::1]:53979", "http://[::1]:53979");

  require_rejected("http://example.com", OriginError::InsecureRemote);
  require_rejected("http://126.255.255.255", OriginError::InsecureRemote);
  require_rejected("http://localhost.example", OriginError::InsecureRemote);
}

SYNC_TEST(origin_accepts_only_exact_packaged_product_origins) {
  require_normalized("app://noisedeck", "app://noisedeck");
  require_normalized("app://polymorphic", "app://polymorphic");
  require_rejected("APP://noisedeck");
  require_rejected("app://Noisedeck");
  require_rejected("app://noisedeck:1");
  require_rejected("app://noisedeck/");
  require_rejected("APP://polymorphic");
  require_rejected("app://Polymorphic");
  require_rejected("app://polymorphic:1");
  require_rejected("app://polymorphic/");
  require_rejected("app://other");
}

SYNC_TEST(origin_rejects_opaque_credentials_and_non_origin_components) {
  const std::array rejected = {
      "null", "data:text/plain,x", "https:example.com", "https:///example.com",
      "https://user@example.com", "https://user:pass@example.com", "https://example.com/",
      "https://example.com/path", "https://example.com?query", "https://example.com#fragment",
      "https://example.com\\path", "https://example.com:443/", "https://example.com@evil.test",
  };
  for (const std::string_view value : rejected) require_rejected(value);
}

SYNC_TEST(origin_rejects_controls_whitespace_non_ascii_and_confusable_hosts) {
  const std::array rejected = {
      std::string("https://example.com\n"), std::string(" https://example.com"),
      std::string("https://example.com\t"), std::string("https://exa\0mple.com", 20),
      std::string("https://examp\xC2\xA0le.com"),
      std::string("https://\xD0\xB5xample.com"),
      std::string("https://exam_ple.com"),
      std::string("https://xn--bcher-kva.example"),
      std::string("https://XN--BCHER-KVA.example"),
      std::string("https://xn--invalid-.example"),
      std::string("https://example%2ecom"),
  };
  for (const auto& value : rejected) require_rejected(value);
}

SYNC_TEST(origin_rejects_exotic_or_ambiguous_host_and_port_forms) {
  const std::array rejected = {
      "https://example.com.", "https://.example.com", "https://a..example.com",
      "https://-example.com", "https://example-.com", "https://0177.0.0.1",
      "https://127.1", "https://2130706433", "https://127.0.0.1:0443",
      "https://127.0.0.1:0", "https://127.0.0.1:65536",
      "https://example.com:", "https://[::1]:",
      "https://0x7f000001", "https://0x7f.0.0.1", "https://0177.0.0.1",
      "https://127.1", "https://127.0.1", "https://1.2.3.0x7f", "https://example.1",
      "https://0x", "https://0X", "https://example.0x",
      "https://[::ffff:127.0.0.1%25lo0]", "https://::1", "https://[::1",
  };
  for (const std::string_view value : rejected) require_rejected(value);
}

SYNC_TEST(origin_enforces_fixed_input_and_normalized_bounds) {
  std::string overlong(noisefactor::sync::kMaximumOriginInputBytes + 1, 'a');
  require_rejected(overlong, OriginError::TooLong);

  std::string label(64, 'a');
  require_rejected("https://" + label + ".example");
}
