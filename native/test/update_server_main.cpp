#include <sync/server.hpp>
#include <sync/update_coordinator.hpp>
#include <uv.h>
#include <string_view>

// Separate fixture: production CLI never exposes a way to reserve maintenance
// from unauthenticated HTTP or a test-only command line switch.
int main(int argc, char** argv) {
  namespace sync = noisefactor::sync;
  sync::update::Coordinator coordinator;
  const auto now = uv_hrtime() / 1000000;
  coordinator.set_external_state(true, now - sync::update::Coordinator::kIdleMs);
  if (argc == 2 && std::string_view(argv[1]) == "reserved") {
    const std::string digest(64, 'a');
    const auto token = coordinator.reserve(digest, now);
    if (!token || !coordinator.commit(*token, digest, now)) return 1;
  }
  sync::ServerOptions options;
  options.update_coordinator = &coordinator;
  options.allowed_origin = "https://client.example";
  options.test_token = "test-update-token";
  options.test_receiver = true;
  options.providers[0] = {.id = "test", .direction = sync::ProviderDirection::Send,
                          .available = true, .selected = true};
  options.provider_count = 1;
  return sync::run_server(options);
}
