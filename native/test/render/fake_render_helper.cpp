// A stand-in for sync-render that speaks the same contract with no GPU: it
// creates the ring it is told to, reports ready, writes solid frames, and exits
// when its stdin closes. The supervisor tests drive it through real process
// spawning so the spawn, attach, restart and shutdown paths run for real.
//
//   --ring <name>             ring to create (the supervisor supplies this)
//   --exit-on-stdin-eof       exit when stdin closes (ditto)
//   --fake-exit-after <n>     exit after writing n frames
//   --fake-exit-code <code>   the code to exit with then (default 0)
//   --fake-stall-after <n>    stop writing and heartbeating after n frames

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sync/render/render_ring.hpp>

namespace {

std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) { g_stop.store(true); }

}  // namespace

int main(int argc, char** argv) {
  using namespace noisefactor::sync::render;
  std::string ring;
  bool watch_stdin = false;
  long exit_after = -1;
  long stall_after = -1;
  int exit_code = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (arg == "--ring") ring = next();
    else if (arg == "--exit-on-stdin-eof") watch_stdin = true;
    else if (arg == "--fake-exit-after") exit_after = std::strtol(next(), nullptr, 10);
    else if (arg == "--fake-exit-code") exit_code = static_cast<int>(std::strtol(next(), nullptr, 10));
    else if (arg == "--fake-stall-after") stall_after = std::strtol(next(), nullptr, 10);
  }
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  if (watch_stdin) {
    std::thread([] {
      while (std::fgetc(stdin) != EOF) {
      }
      g_stop.store(true);
    }).detach();
  }

  constexpr RenderRingGeometry geometry{.width = 32, .height = 16};
  std::string error;
  auto section = RenderRingSection::create(ring, *render_ring_bytes(geometry), error);
  if (!section.has_value()) {
    std::fprintf(stderr, "fake helper: %s\n", error.c_str());
    return 3;
  }
  RenderRingWriter writer(section->bytes(), geometry, 1);
  std::printf("{\"event\":\"ready\",\"ring\":\"%s\"}\n", ring.c_str());
  std::printf("{\"event\":\"stats\",\"rendered\":0}\n");
  std::fflush(stdout);

  std::vector<std::byte> frame(std::size_t{geometry.width} * 4 * geometry.height);
  long written = 0;
  while (!g_stop.load()) {
    if (stall_after < 0 || written < stall_after) {
      std::memset(frame.data(), static_cast<int>(written & 0xFF), frame.size());
      if (writer.write(frame, geometry.width * 4, render_clock_us())) ++written;
      if (exit_after >= 0 && written >= exit_after) return exit_code;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  writer.close();
  return 0;
}
