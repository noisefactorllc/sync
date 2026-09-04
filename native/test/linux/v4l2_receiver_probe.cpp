#include <sync/platform/v4l2_receiver_probe.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef SYNC_V4L2_PROBE_NO_MAIN
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace noisefactor::sync::v4l2_probe {
namespace {

auto parse_count(std::string_view text, std::uint32_t& output) noexcept -> bool {
  if (text.empty() || text.size() > 3) return false;
  std::uint32_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value == 0 ||
      value > 600) {
    return false;
  }
  output = value;
  return true;
}

auto valid_output_path(std::string_view value) noexcept -> bool {
  return value.size() > 1 && value.size() < 4096 && value.front() == '/' &&
         value.find('\0') == std::string_view::npos;
}

}  // namespace

auto valid_camera_path(std::string_view path) noexcept -> bool {
  constexpr std::string_view prefix = "/dev/video";
  if (!path.starts_with(prefix) || path.size() == prefix.size() ||
      path.size() >= 64) {
    return false;
  }
  return std::all_of(path.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                     path.end(),
                     [](char value) { return value >= '0' && value <= '9'; });
}

auto parse(std::span<const std::string_view> arguments) -> ParseResult {
  ParseResult result;
  bool saw_device = false;
  bool saw_frames = false;
  bool saw_output = false;
  for (std::size_t index = 0; index < arguments.size();) {
    if (index + 1 >= arguments.size()) {
      result.error = "every option requires a value";
      return result;
    }
    const auto name = arguments[index++];
    const auto value = arguments[index++];
    if (name == "--device" && !saw_device && valid_camera_path(value)) {
      result.options.device = value;
      saw_device = true;
    } else if (name == "--frames" && !saw_frames &&
               parse_count(value, result.options.frame_count)) {
      saw_frames = true;
    } else if (name == "--output" && !saw_output && valid_output_path(value)) {
      result.options.output_directory = value;
      saw_output = true;
    } else {
      result.error = "invalid or duplicate option";
      return result;
    }
  }
  if (!saw_device || !saw_frames || !saw_output || arguments.size() != 6) {
    result.error = "--device, --frames, and --output are required exactly once";
  }
  return result;
}

auto fnv1a64(std::span<const std::byte> bytes) noexcept -> std::uint64_t {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const std::byte byte : bytes) {
    hash ^= static_cast<std::uint8_t>(byte);
    hash *= 1099511628211ULL;
  }
  return hash;
}

auto unique_checksum_count(std::span<const std::uint64_t> checksums) noexcept
    -> std::size_t {
  std::size_t unique = 0;
  for (std::size_t index = 0; index < checksums.size(); ++index) {
    bool seen = false;
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (checksums[previous] == checksums[index]) {
        seen = true;
        break;
      }
    }
    if (!seen) ++unique;
  }
  return unique;
}

}  // namespace noisefactor::sync::v4l2_probe

#ifndef SYNC_V4L2_PROBE_NO_MAIN
namespace {

using noisefactor::sync::v4l2_probe::fnv1a64;

template <typename Value>
auto ioctl_retry(int descriptor, unsigned long request, Value* value) noexcept
    -> int {
  int status = -1;
  do {
    status = ::ioctl(descriptor, request, value);
  } while (status < 0 && errno == EINTR);
  return status;
}

auto monotonic_ms() noexcept -> std::uint64_t {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

auto exact_text(const __u8* value, std::size_t capacity,
                std::string_view expected) noexcept -> bool {
  const auto* text = reinterpret_cast<const char*>(value);
  const std::size_t length = ::strnlen(text, capacity);
  return length == expected.size() &&
         std::memcmp(text, expected.data(), expected.size()) == 0;
}

auto write_raw(const std::filesystem::path& path,
               std::span<const std::byte> bytes) -> bool {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return stream.good();
}

auto write_pgm(const std::filesystem::path& path,
               std::span<const std::byte> bytes, std::uint32_t width,
               std::uint32_t height, std::size_t stride) -> bool {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << "P5\n" << width << ' ' << height << "\n255\n";
  for (std::uint32_t row = 0; row < height; ++row) {
    const std::byte* data = bytes.data() + static_cast<std::size_t>(row) * stride;
    stream.write(reinterpret_cast<const char*>(data), width);
  }
  return stream.good();
}

auto resembles_idle_card(std::span<const std::byte> bytes, std::uint32_t width,
                         std::uint32_t height, std::size_t stride) noexcept
    -> bool {
  std::size_t background = 0;
  std::size_t light = 0;
  const std::size_t pixels = static_cast<std::size_t>(width) * height;
  for (std::uint32_t row = 0; row < height; ++row) {
    const std::byte* line = bytes.data() + static_cast<std::size_t>(row) * stride;
    for (std::uint32_t column = 0; column < width; ++column) {
      const auto value = static_cast<std::uint8_t>(line[column]);
      if (value >= 31 && value <= 35) ++background;
      if (value >= 175 && value <= 195) ++light;
    }
  }
  return background * 100 >= pixels * 97 && light > 500;
}

void print_json(const noisefactor::sync::v4l2_probe::Options& options,
                std::uint32_t width, std::uint32_t height,
                std::size_t stride, std::span<const std::uint64_t> timestamps,
                std::span<const std::uint64_t> checksums,
                std::size_t idle_frames) {
  std::cout << "{\"version\":1,\"device\":\"" << options.device
            << "\",\"width\":" << width << ",\"height\":" << height
            << ",\"format\":\"NV12\",\"stride\":" << stride
            << ",\"frameCount\":" << checksums.size()
            << ",\"uniqueChecksumCount\":"
            << noisefactor::sync::v4l2_probe::unique_checksum_count(checksums)
            << ",\"idleCardFrames\":" << idle_frames << ",\"timestampsMs\":[";
  for (std::size_t index = 0; index < timestamps.size(); ++index) {
    if (index != 0) std::cout << ',';
    std::cout << timestamps[index];
  }
  std::cout << "],\"checksums\":[";
  for (std::size_t index = 0; index < checksums.size(); ++index) {
    if (index != 0) std::cout << ',';
    std::cout << '"' << std::hex << checksums[index] << std::dec << '"';
  }
  std::cout << "]}\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> arguments;
  arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
  for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
  const auto parsed = noisefactor::sync::v4l2_probe::parse(arguments);
  if (!parsed.ok()) {
    std::cerr << "usage: sync_v4l2_receiver_probe --device /dev/videoN "
                 "--frames 90 --output /absolute/directory\n"
              << parsed.error << '\n';
    return 2;
  }

  std::error_code filesystem_error;
  std::filesystem::create_directories(parsed.options.output_directory,
                                      filesystem_error);
  if (filesystem_error) {
    std::cerr << "could not create output directory\n";
    return 1;
  }

  struct stat before {};
  if (::lstat(parsed.options.device.c_str(), &before) != 0 ||
      !S_ISCHR(before.st_mode)) {
    std::cerr << "camera path is not a character device\n";
    return 1;
  }
  const int descriptor = ::open(parsed.options.device.c_str(),
                                O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    std::cerr << "could not open camera: " << std::strerror(errno) << '\n';
    return 1;
  }
  const auto close_descriptor = [&] { ::close(descriptor); };

  struct stat after {};
  if (::fstat(descriptor, &after) != 0 || before.st_dev != after.st_dev ||
      before.st_ino != after.st_ino) {
    std::cerr << "camera path changed during open\n";
    close_descriptor();
    return 1;
  }

  v4l2_capability capabilities{};
  if (ioctl_retry(descriptor, VIDIOC_QUERYCAP, &capabilities) != 0 ||
      !exact_text(capabilities.driver, sizeof(capabilities.driver),
                  "v4l2 loopback") ||
      !exact_text(capabilities.card, sizeof(capabilities.card), "Sync Camera")) {
    std::cerr << "camera identity did not match Sync Camera\n";
    close_descriptor();
    return 1;
  }
  const std::uint32_t caps =
      (capabilities.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
          ? capabilities.device_caps
          : capabilities.capabilities;
  if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0 ||
      (caps & V4L2_CAP_READWRITE) == 0) {
    std::cerr << "camera lacks capture/read support\n";
    close_descriptor();
    return 1;
  }

  v4l2_format format{};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = 1920;
  format.fmt.pix.height = 1080;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
  format.fmt.pix.field = V4L2_FIELD_NONE;
  if (ioctl_retry(descriptor, VIDIOC_S_FMT, &format) != 0 ||
      format.fmt.pix.width != 1920 || format.fmt.pix.height != 1080 ||
      format.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12 ||
      format.fmt.pix.bytesperline < 1920 ||
      format.fmt.pix.sizeimage < format.fmt.pix.bytesperline * 1620ULL ||
      format.fmt.pix.sizeimage > 64ULL * 1024ULL * 1024ULL) {
    std::cerr << "camera rejected exact 1920x1080 NV12 capture\n";
    close_descriptor();
    return 1;
  }

  std::vector<std::byte> frame(format.fmt.pix.sizeimage);
  std::vector<std::uint64_t> checksums;
  std::vector<std::uint64_t> timestamps;
  checksums.reserve(parsed.options.frame_count);
  timestamps.reserve(parsed.options.frame_count);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::size_t idle_frames = 0;
  const std::array<std::uint32_t, 3> retained{
      0, parsed.options.frame_count / 2, parsed.options.frame_count - 1};

  for (std::uint32_t index = 0; index < parsed.options.frame_count; ++index) {
    std::size_t offset = 0;
    while (offset < frame.size()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        std::cerr << "timed out before receiving all frames\n";
        close_descriptor();
        return 1;
      }
      const auto count = ::read(descriptor, frame.data() + offset,
                                frame.size() - offset);
      if (count > 0) {
        offset += static_cast<std::size_t>(count);
        continue;
      }
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        std::cerr << "camera read failed: " << std::strerror(errno) << '\n';
        close_descriptor();
        return 1;
      }
      pollfd wait{.fd = descriptor, .events = POLLIN, .revents = 0};
      const int ready = ::poll(&wait, 1, 100);
      if (ready < 0 && errno != EINTR) {
        std::cerr << "camera poll failed\n";
        close_descriptor();
        return 1;
      }
    }
    checksums.push_back(fnv1a64(frame));
    timestamps.push_back(monotonic_ms());
    if (resembles_idle_card(frame, 1920, 1080, format.fmt.pix.bytesperline)) {
      ++idle_frames;
    }
    for (std::size_t retained_index = 0; retained_index < retained.size();
         ++retained_index) {
      if (retained[retained_index] != index) continue;
      const char* names[] = {"first", "middle", "last"};
      const auto base = std::filesystem::path(parsed.options.output_directory) /
                        names[retained_index];
      if (!write_raw(base.string() + ".nv12", frame) ||
          !write_pgm(base.string() + ".pgm", frame, 1920, 1080,
                     format.fmt.pix.bytesperline)) {
        std::cerr << "could not write retained frame\n";
        close_descriptor();
        return 1;
      }
    }
  }
  close_descriptor();
  print_json(parsed.options, 1920, 1080, format.fmt.pix.bytesperline,
             timestamps, checksums, idle_frames);
  return 0;
}
#endif
