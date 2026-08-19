#include "../test_harness.hpp"

#include "../../src/platform/windows/companion_process.hpp"

#include <sync/companion_model.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace {

using noisefactor::sync::companion::AvailableProviders;
using noisefactor::sync::companion::classify_revocation;
using noisefactor::sync::companion::HealthSnapshot;
using noisefactor::sync::companion::parse_health;
using noisefactor::sync::companion::parse_pairings_json;
using noisefactor::sync::companion::resolve_helper_path;

// Builds a /status-shaped body identical to what control::encode_status
// produces (see native/src/control.cpp), with the providers array and
// activeSenders value substituted in verbatim so each test can supply
// exactly the shape it wants to probe.
std::string status_body(std::string_view providers_json,
                        std::string_view active_senders_json = "2") {
  std::string body =
      "{\"product\":\"Sync\",\"status\":\"ok\",\"version\":\"0.2.7\","
      "\"protocolVersions\":[1],\"instanceId\":\"abc\",\"capabilities\":"
      "{\"send\":true,\"receive\":false,\"providers\":[";
  body += providers_json;
  body += "]}";
  if (!active_senders_json.empty()) {
    body += ",\"activeSenders\":";
    body += active_senders_json;
  }
  body += "}";
  return body;
}

std::string provider(std::string_view id, std::string_view direction,
                     std::string_view available_json,
                     std::string_view selected_json) {
  std::string entry = "{\"id\":\"";
  entry += id;
  entry += "\",\"direction\":\"";
  entry += direction;
  entry += "\"";
  if (!available_json.empty()) {
    entry += ",\"available\":";
    entry += available_json;
  }
  if (!selected_json.empty()) {
    entry += ",\"selected\":";
    entry += selected_json;
  }
  entry += "}";
  return entry;
}

std::string send_provider(std::string_view id) {
  return provider(id, "send", "true", "true");
}

SYNC_TEST(parse_health_accepts_a_valid_single_provider_status_body) {
  const auto health =
      parse_health(status_body(send_provider("spout")));
  SYNC_REQUIRE(health.has_value());
  SYNC_REQUIRE(health->reachable);
  SYNC_REQUIRE(health->compatible);
  SYNC_REQUIRE(health->product == "Sync");
  SYNC_REQUIRE(health->version == "0.2.7");
  SYNC_REQUIRE(health->providers.size() == 1);
  SYNC_REQUIRE(health->providers.contains("spout"));
  SYNC_REQUIRE(health->active_senders == std::optional<std::size_t>(2));
}

SYNC_TEST(parse_health_accepts_spout_and_ndi_together) {
  const std::string providers =
      send_provider("spout") + "," + send_provider("ndi");
  const auto health = parse_health(status_body(providers));
  SYNC_REQUIRE(health.has_value());
  SYNC_REQUIRE(health->providers.size() == 2);
  SYNC_REQUIRE(health->providers.contains("spout"));
  SYNC_REQUIRE(health->providers.contains("ndi"));
}

SYNC_TEST(parse_health_excludes_an_unavailable_provider) {
  const std::string providers =
      provider("spout", "send", "false", "true") + "," + send_provider("ndi");
  const auto health = parse_health(status_body(providers));
  SYNC_REQUIRE(health.has_value());
  SYNC_REQUIRE(health->providers.size() == 1);
  SYNC_REQUIRE(health->providers.contains("ndi"));
  SYNC_REQUIRE(!health->providers.contains("spout"));
}

SYNC_TEST(parse_health_excludes_a_receive_direction_provider) {
  const std::string providers =
      provider("syphon", "receive", "true", "true") + "," +
      send_provider("ndi");
  const auto health = parse_health(status_body(providers));
  SYNC_REQUIRE(health.has_value());
  SYNC_REQUIRE(health->providers.size() == 1);
  SYNC_REQUIRE(health->providers.contains("ndi"));
}

SYNC_TEST(parse_health_excludes_an_available_but_not_selected_provider) {
  // A provider only counts once it is both available AND actually selected
  // (publishing) -- reporting an available-but-deselected provider would
  // send the operator looking for a source that is not there.
  const std::string providers =
      provider("spout", "send", "true", "false") + "," + send_provider("ndi");
  const auto health = parse_health(status_body(providers));
  SYNC_REQUIRE(health.has_value());
  SYNC_REQUIRE(health->providers.size() == 1);
  SYNC_REQUIRE(health->providers.contains("ndi"));
  SYNC_REQUIRE(!health->providers.contains("spout"));
}

SYNC_TEST(parse_health_excludes_a_provider_with_a_missing_selected_field) {
  const std::string providers = provider("spout", "send", "true", "");
  const auto health = parse_health(status_body(providers));
  SYNC_REQUIRE(health.has_value());
  SYNC_REQUIRE(health->providers.empty());
}

SYNC_TEST(parse_health_excludes_a_provider_with_a_non_boolean_selected_field) {
  std::string entry =
      "{\"id\":\"spout\",\"direction\":\"send\",\"available\":true,"
      "\"selected\":1}";
  const auto health = parse_health(status_body(entry));
  SYNC_REQUIRE(health.has_value());
  SYNC_REQUIRE(health->providers.empty());
}

SYNC_TEST(parse_health_rejects_more_providers_than_the_build_bounds_for) {
  // kMaximumHealthProviders is 4; five distinct available+selected send
  // providers must be rejected outright rather than silently truncated.
  const std::string providers =
      send_provider("a") + "," + send_provider("b") + "," +
      send_provider("c") + "," + send_provider("d") + "," + send_provider("e");
  const auto health = parse_health(status_body(providers));
  SYNC_REQUIRE(!health.has_value());
}

SYNC_TEST(parse_health_rejects_wrong_product_status_or_version_type) {
  const std::string providers = send_provider("spout");
  std::string wrong_product = status_body(providers);
  wrong_product.replace(wrong_product.find("\"Sync\""), 6, "\"Nope\"");
  SYNC_REQUIRE(!parse_health(wrong_product).has_value());

  std::string wrong_status = status_body(providers);
  wrong_status.replace(wrong_status.find("\"ok\""), 4, "\"bad\"");
  SYNC_REQUIRE(!parse_health(wrong_status).has_value());

  const std::string missing_version =
      "{\"product\":\"Sync\",\"status\":\"ok\","
      "\"protocolVersions\":[1],\"capabilities\":{\"providers\":[]},"
      "\"activeSenders\":0}";
  SYNC_REQUIRE(!parse_health(missing_version).has_value());

  const std::string non_string_version =
      "{\"product\":\"Sync\",\"status\":\"ok\",\"version\":7,"
      "\"protocolVersions\":[1],\"capabilities\":{\"providers\":[]},"
      "\"activeSenders\":0}";
  SYNC_REQUIRE(!parse_health(non_string_version).has_value());
}

SYNC_TEST(parse_health_rejects_a_body_missing_protocol_version_1) {
  const std::string body =
      "{\"product\":\"Sync\",\"status\":\"ok\",\"version\":\"0.2.7\","
      "\"protocolVersions\":[2,3],\"capabilities\":{\"providers\":[]},"
      "\"activeSenders\":0}";
  SYNC_REQUIRE(!parse_health(body).has_value());
}

SYNC_TEST(parse_health_rejects_malformed_active_senders_values) {
  const std::string providers = send_provider("spout");
  SYNC_REQUIRE(!parse_health(status_body(providers, "-1")).has_value());
  SYNC_REQUIRE(!parse_health(status_body(providers, "65")).has_value());
  SYNC_REQUIRE(!parse_health(status_body(providers, "2.5")).has_value());
  SYNC_REQUIRE(!parse_health(status_body(providers, "\"2\"")).has_value());
  SYNC_REQUIRE(!parse_health(status_body(providers, "")).has_value());
  // Bounds are inclusive: 0 and 64 are both valid.
  SYNC_REQUIRE(parse_health(status_body(providers, "0")).has_value());
  SYNC_REQUIRE(parse_health(status_body(providers, "64")).has_value());
}

SYNC_TEST(parse_health_rejects_malformed_json) {
  SYNC_REQUIRE(!parse_health("").has_value());
  SYNC_REQUIRE(!parse_health("not json").has_value());
  SYNC_REQUIRE(!parse_health("{").has_value());
  SYNC_REQUIRE(!parse_health("[1,2,3]").has_value());
  SYNC_REQUIRE(!parse_health("{\"a\":1,}").has_value());
}

SYNC_TEST(resolve_helper_path_replaces_the_executable_name_only) {
  SYNC_REQUIRE(resolve_helper_path("C:\\Program Files\\Sync\\Sync.exe") ==
              "C:\\Program Files\\Sync\\syncd.exe");
  SYNC_REQUIRE(resolve_helper_path("C:/Program Files/Sync/Sync.exe") ==
              "C:/Program Files/Sync/syncd.exe");
  SYNC_REQUIRE(resolve_helper_path("Sync.exe") == "syncd.exe");
  SYNC_REQUIRE(resolve_helper_path("") == "");
  // A path with both separator styles uses whichever is rightmost.
  SYNC_REQUIRE(resolve_helper_path("C:/Users/Jess\\dev/Sync.exe") ==
              "C:/Users/Jess\\dev/syncd.exe");
}

SYNC_TEST(companion_process_pairing_parser_requires_normalized_origins) {
  const auto valid = parse_pairings_json(
      R"({"type":"pairings","origins":["https://one.example","http://localhost:8080"]})");
  SYNC_REQUIRE(valid.ok());
  SYNC_REQUIRE(valid.origins.size() == 2);

  const auto noncanonical = parse_pairings_json(
      R"({"type":"pairings","origins":["HTTPS://ONE.EXAMPLE:443"]})");
  SYNC_REQUIRE(!noncanonical.ok());
  const auto malformed = parse_pairings_json(R"({"origins":[]})");
  SYNC_REQUIRE(!malformed.ok());
  const auto extra_key = parse_pairings_json(
      R"({"type":"pairings","origins":[],"extra":1})");
  SYNC_REQUIRE(!extra_key.ok());
}

SYNC_TEST(companion_process_revocation_requires_a_durable_committed_record) {
  const std::string_view revoked =
      R"({"type":"revocation","origin":"https://one.example","status":"revoked"})";

  const auto durable = classify_revocation(0, revoked);
  SYNC_REQUIRE(durable.ok());
  SYNC_REQUIRE(durable.revoked);

  const auto absent = classify_revocation(
      0,
      R"({"type":"revocation","origin":"https://one.example","status":"not_found"})");
  SYNC_REQUIRE(absent.ok());
  SYNC_REQUIRE(absent.revoked);

  const auto uncertain = classify_revocation(
      3,
      R"({"type":"revocation","origin":"https://one.example","status":"revoked_durability_uncertain"})");
  SYNC_REQUIRE(!uncertain.revoked);
  SYNC_REQUIRE(!uncertain.ok());

  SYNC_REQUIRE(!classify_revocation(3, revoked).revoked);
  SYNC_REQUIRE(!classify_revocation(1, revoked).revoked);
  SYNC_REQUIRE(!classify_revocation(0, "not json").revoked);
  SYNC_REQUIRE(!classify_revocation(0, R"({"type":"revocation"})").revoked);
  SYNC_REQUIRE(
      !classify_revocation(
           0,
           R"({"type":"revocation","origin":"https://one.example","status":"revoked","extra":1})")
           .revoked);
}

}  // namespace
