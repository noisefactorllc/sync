// Feeds a solid colour into the Sync camera for a while, so a real consumer
// can be pointed at the camera and checked by eye or by script.
//
// The sibling of spout_receiver_probe and ndi_receiver_probe: not a test, a
// tool for the acceptance checks that need a live sender on one side and a
// genuine application on the other.
//
//   sync_camera_feed_probe [seconds] [b] [g] [r]
//
// Prints one line per second so a script driving it can tell it is alive.

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include <sync/platform/camera_identity.hpp>
#include <sync/platform/mf_camera_sink.hpp>

namespace {

using noisefactor::sync::camera::CameraSinkFrame;
using noisefactor::sync::camera::CameraSinkSubmit;
using noisefactor::sync::camera::describe_unavailability;
using noisefactor::sync::camera::kBytesPerPixel;
using noisefactor::sync::camera::kCanvas;
using noisefactor::sync::camera::kMaximumFramesPerSecond;
using noisefactor::sync::camera::MfCameraSink;

constexpr std::size_t kStride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;

[[nodiscard]] auto argument(int argc, char** argv, int index, int fallback) -> int {
  if (index >= argc) return fallback;
  return std::atoi(argv[index]);
}

}  // namespace

int main(int argc, char** argv) {
  const int seconds = argument(argc, argv, 1, 10);
  const auto blue = static_cast<std::uint8_t>(argument(argc, argv, 2, 128));
  const auto green = static_cast<std::uint8_t>(argument(argc, argv, 3, 128));
  const auto red = static_cast<std::uint8_t>(argument(argc, argv, 4, 128));

  MfCameraSink sink;
  if (!sink.available()) {
    std::printf("unavailable: %s\n",
                describe_unavailability(sink.unavailable_reason(), sink.unavailable_status())
                    .c_str());
    return 1;
  }

  std::vector<std::byte> frame(kStride * kCanvas.height);
  for (std::size_t i = 0; i < frame.size(); i += kBytesPerPixel) {
    frame[i + 0] = static_cast<std::byte>(blue);
    frame[i + 1] = static_cast<std::byte>(green);
    frame[i + 2] = static_cast<std::byte>(red);
    frame[i + 3] = static_cast<std::byte>(255);
  }

  std::printf("feeding b=%u g=%u r=%u for %ds\n", blue, green, red, seconds);
  std::fflush(stdout);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  std::uint64_t sent = 0;
  std::uint64_t dropped = 0;
  std::uint64_t presentation = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const CameraSinkFrame submission{
        .width = kCanvas.width,
        .height = kCanvas.height,
        .row_stride = kStride,
        .bgra = frame,
        .presentation_time_us = ++presentation,
    };
    switch (sink.submit(submission)) {
      case CameraSinkSubmit::Accepted:
        ++sent;
        break;
      // Nobody watching yet is the ordinary case here, not an error: the
      // section only exists while a consumer has the camera open.
      case CameraSinkSubmit::Backpressured:
      case CameraSinkSubmit::Failed:
        ++dropped;
        break;
    }
    if (std::chrono::steady_clock::now() >= next_report) {
      std::printf("sent=%llu dropped=%llu\n", static_cast<unsigned long long>(sent),
                  static_cast<unsigned long long>(dropped));
      std::fflush(stdout);
      next_report += std::chrono::seconds(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000 / kMaximumFramesPerSecond));
  }
  std::printf("done sent=%llu dropped=%llu\n", static_cast<unsigned long long>(sent),
              static_cast<unsigned long long>(dropped));
  return 0;
}
