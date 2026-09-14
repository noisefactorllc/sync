#include <jack/jack.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

namespace {
volatile std::sig_atomic_t running = 1;
std::array<jack_port_t *, 32> ports{};

int process(jack_nframes_t count, void *) {
  for (unsigned channel = 0; channel < ports.size(); ++channel) {
    auto *samples = static_cast<float *>(jack_port_get_buffer(ports[channel], count));
    const float value = static_cast<float>(channel + 1) / 64.0f *
                        (channel % 2 ? -1.0f : 1.0f);
    for (jack_nframes_t frame = 0; frame < count; ++frame) samples[frame] = value;
  }
  return 0;
}
} // namespace

// Run only against an isolated JACK server. This creates a real native JACK
// source; the acceptance test uses Sync's production RtAudio input backend.
int main() {
  jack_status_t status{};
  auto *client = jack_client_open("SyncTest32",
      static_cast<jack_options_t>(JackNoStartServer | JackUseExactName), &status);
  if (!client) return 1;
  for (unsigned channel = 0; channel < ports.size(); ++channel) {
    char name[32];
    std::snprintf(name, sizeof(name), "channel_%02u", channel + 1);
    ports[channel] = jack_port_register(client, name, JACK_DEFAULT_AUDIO_TYPE,
                                        JackPortIsOutput, 0);
    if (!ports[channel]) { jack_client_close(client); return 2; }
  }
  if (jack_set_process_callback(client, process, nullptr) || jack_activate(client)) {
    jack_client_close(client);
    return 3;
  }
  std::signal(SIGTERM, [](int) { running = 0; });
  std::signal(SIGINT, [](int) { running = 0; });
  std::printf("{\"source\":\"SyncTest32\",\"channels\":32,\"sampleRate\":%u}\n",
              jack_get_sample_rate(client));
  std::fflush(stdout);
  while (running) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  jack_client_close(client);
}
