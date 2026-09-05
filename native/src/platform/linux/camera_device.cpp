#include <sync/platform/linux_camera_device.hpp>

#include <sync/camera/nv12.hpp>
#include <sync/platform/camera_identity.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace noisefactor::sync::camera {
namespace {

constexpr std::string_view kDevicePrefix = "/dev/video";
constexpr std::string_view kExpectedDriver = "v4l2 loopback";
constexpr std::size_t kMaximumEnumeratedDevices = 64;
constexpr std::size_t kMaximumFrameBytes = 64U * 1024U * 1024U;

bool valid_path(std::string_view path) noexcept {
  if (path.size() <= kDevicePrefix.size() || path.size() >= 64 ||
      !path.starts_with(kDevicePrefix)) {
    return false;
  }
  for (const unsigned char byte : path.substr(kDevicePrefix.size())) {
    if (byte < '0' || byte > '9') return false;
  }
  return true;
}

template <typename Byte, std::size_t Size>
std::string_view fixed_text(const Byte (&value)[Size]) noexcept {
  std::size_t length = 0;
  while (length < Size && value[length] != 0) ++length;
  return {reinterpret_cast<const char*>(value), length};
}

std::uint32_t effective_capabilities(
    const v4l2_capability& capabilities) noexcept {
  return (capabilities.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
             ? capabilities.device_caps
             : capabilities.capabilities;
}

LinuxCameraDeviceError identity_error(const v4l2_capability& capabilities,
                                      bool require_card,
                                      bool require_output) noexcept {
  if (fixed_text(capabilities.driver) != kExpectedDriver) {
    return LinuxCameraDeviceError::WrongDriver;
  }
  if (require_card && fixed_text(capabilities.card) != kDeviceName) {
    return LinuxCameraDeviceError::WrongCard;
  }
  const std::uint32_t flags = effective_capabilities(capabilities);
  const bool video_capability =
      require_output ? (flags & V4L2_CAP_VIDEO_OUTPUT) != 0
                     : (flags & (V4L2_CAP_VIDEO_OUTPUT |
                                 V4L2_CAP_VIDEO_CAPTURE)) != 0;
  if (!video_capability ||
      (flags & V4L2_CAP_READWRITE) == 0) {
    return LinuxCameraDeviceError::MissingOutputCapability;
  }
  return LinuxCameraDeviceError::None;
}

LinuxCameraOpenResult negotiate(int descriptor, std::string_view path,
                                LinuxCameraDeviceOps& operations) noexcept {
  v4l2_format format{};
  format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  format.fmt.pix.width = kCanvas.width;
  format.fmt.pix.height = kCanvas.height;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
  format.fmt.pix.field = V4L2_FIELD_NONE;
  // bgra_to_nv12 encodes BT.709 limited-range YCbCr. Leaving this at DEFAULT
  // lets v4l2loopback choose sRGB, whose default YCbCr matrix is BT.601.
  format.fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
  if (operations.set_nv12_format(descriptor, format) != 0) {
    const int saved_error = errno;
    operations.close_descriptor(descriptor);
    return {.error = LinuxCameraDeviceError::FormatRejected,
            .native_error = saved_error};
  }
  if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12) {
    operations.close_descriptor(descriptor);
    return {.error = LinuxCameraDeviceError::FormatRejected};
  }
  const std::size_t stride = format.fmt.pix.bytesperline;
  const std::size_t size = format.fmt.pix.sizeimage;
  const bool multiplication_safe =
      stride <= std::numeric_limits<std::size_t>::max() / kCanvas.height;
  const std::size_t minimum =
      multiplication_safe
          ? nv12_size_bytes(kCanvas.width, kCanvas.height, stride)
          : std::numeric_limits<std::size_t>::max();
  if (format.fmt.pix.width != kCanvas.width ||
      format.fmt.pix.height != kCanvas.height || stride < kCanvas.width ||
      !multiplication_safe || minimum < stride || size < minimum ||
      size > kMaximumFrameBytes) {
    operations.close_descriptor(descriptor);
    return {.error = LinuxCameraDeviceError::InvalidFormatBounds};
  }

  v4l2_streamparm rate{};
  rate.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  rate.parm.output.timeperframe.numerator = 1;
  rate.parm.output.timeperframe.denominator = kMaximumFramesPerSecond;
  (void)operations.set_frame_rate(descriptor, rate);

  LinuxCameraOpenResult result{
      .error = LinuxCameraDeviceError::None,
      .descriptor = descriptor,
      .format = {.width = format.fmt.pix.width,
                 .height = format.fmt.pix.height,
                 .y_stride = stride,
                 .size_image = size},
  };
  std::copy(path.begin(), path.end(), result.path.begin());
  return result;
}

LinuxCameraOpenResult open_explicit(std::string_view path,
                                    LinuxCameraDeviceOps& operations,
                                    bool require_card,
                                    bool negotiate_format) noexcept {
  if (!valid_path(path)) return {.error = LinuxCameraDeviceError::InvalidPath};
  const int descriptor = operations.open_no_follow(path);
  if (descriptor < 0) {
    const int saved_error = errno;
    return {
        .error = saved_error == ELOOP
                     ? LinuxCameraDeviceError::InvalidPath
                     : ((saved_error == EACCES || saved_error == EPERM)
                            ? LinuxCameraDeviceError::OpenDenied
                            : (saved_error == ENOENT
                                   ? LinuxCameraDeviceError::NotFound
                                   : LinuxCameraDeviceError::Io)),
        .native_error = saved_error,
    };
  }
  if (!operations.validate_character_device(descriptor)) {
    operations.close_descriptor(descriptor);
    return {.error = LinuxCameraDeviceError::NotCharacterDevice};
  }
  v4l2_capability capabilities{};
  if (operations.query_capabilities(descriptor, capabilities) != 0) {
    const int saved_error = errno;
    operations.close_descriptor(descriptor);
    return {.error = LinuxCameraDeviceError::Io,
            .native_error = saved_error};
  }
  const LinuxCameraDeviceError identity =
      identity_error(capabilities, require_card, negotiate_format);
  if (identity != LinuxCameraDeviceError::None) {
    operations.close_descriptor(descriptor);
    return {.error = identity};
  }
  if (negotiate_format) return negotiate(descriptor, path, operations);
  LinuxCameraOpenResult result{.error = LinuxCameraDeviceError::None,
                               .descriptor = descriptor};
  std::copy(path.begin(), path.end(), result.path.begin());
  return result;
}

class SystemLinuxCameraDeviceOps final : public LinuxCameraDeviceOps {
 public:
  auto enumerate(std::span<std::array<char, 64>> output) noexcept
      -> std::size_t override {
    DIR* directory = ::opendir("/sys/class/video4linux");
    if (directory == nullptr) return 0;
    std::size_t count = 0;
    while (dirent* entry = ::readdir(directory)) {
      const std::string_view name(entry->d_name);
      constexpr std::string_view prefix = "video";
      if (name.size() <= prefix.size() || !name.starts_with(prefix)) continue;
      bool numeric = true;
      for (const unsigned char byte : name.substr(prefix.size())) {
        if (byte < '0' || byte > '9') {
          numeric = false;
          break;
        }
      }
      if (!numeric) continue;
      const std::string path = "/dev/" + std::string(name);
      if (path.size() >= 64) continue;
      if (count < output.size()) {
        output[count].fill('\0');
        std::copy(path.begin(), path.end(), output[count].begin());
      }
      ++count;
    }
    ::closedir(directory);
    return count;
  }

  auto open_no_follow(std::string_view path) noexcept -> int override {
    const std::string owned(path);
    return ::open(owned.c_str(),
                  O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  }

  auto validate_character_device(int descriptor) noexcept -> bool override {
    struct stat status {};
    return ::fstat(descriptor, &status) == 0 && S_ISCHR(status.st_mode);
  }

  auto query_capabilities(int descriptor,
                          v4l2_capability& output) noexcept -> int override {
    return ioctl_retry(descriptor, VIDIOC_QUERYCAP, &output);
  }

  auto set_nv12_format(int descriptor, v4l2_format& format) noexcept
      -> int override {
    return ioctl_retry(descriptor, VIDIOC_S_FMT, &format);
  }

  auto set_frame_rate(int descriptor, v4l2_streamparm& rate) noexcept
      -> int override {
    return ioctl_retry(descriptor, VIDIOC_S_PARM, &rate);
  }

  auto write_frame(int descriptor, std::span<const std::byte> frame) noexcept
      -> std::pair<std::ptrdiff_t, std::int32_t> override {
    const auto result = ::write(descriptor, frame.data(), frame.size());
    return {result, result < 0 ? errno : 0};
  }

  void close_descriptor(int descriptor) noexcept override {
    if (descriptor >= 0) ::close(descriptor);
  }

 private:
  static int ioctl_retry(int descriptor, unsigned long request,
                         void* value) noexcept {
    int result = -1;
    do {
      result = ::ioctl(descriptor, request, value);
    } while (result < 0 && errno == EINTR);
    return result;
  }
};

}  // namespace

auto default_linux_camera_device_ops() noexcept -> LinuxCameraDeviceOps& {
  static SystemLinuxCameraDeviceOps operations;
  return operations;
}

auto open_linux_camera(std::string_view explicit_path,
                       LinuxCameraDeviceOps& operations) noexcept
    -> LinuxCameraOpenResult {
  if (!explicit_path.empty()) {
    return open_explicit(explicit_path, operations, false, true);
  }
  std::array<std::array<char, 64>, kMaximumEnumeratedDevices> paths{};
  const std::size_t count = operations.enumerate(paths);
  if (count == 0) return {.error = LinuxCameraDeviceError::NotFound};

  int matched_descriptor = -1;
  std::string_view matched_path;
  std::size_t matches = 0;
  for (std::size_t index = 0; index < std::min(count, paths.size()); ++index) {
    const auto end = std::find(paths[index].begin(), paths[index].end(), '\0');
    const std::string_view path(paths[index].data(),
                                static_cast<std::size_t>(end - paths[index].begin()));
    if (!valid_path(path)) continue;
    const int descriptor = operations.open_no_follow(path);
    if (descriptor < 0) continue;
    if (!operations.validate_character_device(descriptor)) {
      operations.close_descriptor(descriptor);
      continue;
    }
    v4l2_capability capabilities{};
    if (operations.query_capabilities(descriptor, capabilities) != 0 ||
        identity_error(capabilities, true, true) !=
            LinuxCameraDeviceError::None) {
      operations.close_descriptor(descriptor);
      continue;
    }
    ++matches;
    if (matches == 1) {
      matched_descriptor = descriptor;
      matched_path = path;
    } else {
      operations.close_descriptor(descriptor);
    }
  }
  if (matches == 0) return {.error = LinuxCameraDeviceError::NotFound};
  if (matches > 1) {
    operations.close_descriptor(matched_descriptor);
    return {.error = LinuxCameraDeviceError::Ambiguous};
  }
  return negotiate(matched_descriptor, matched_path, operations);
}

auto probe_linux_camera(std::string_view explicit_path,
                        LinuxCameraDeviceOps& operations) noexcept
    -> LinuxCameraOpenResult {
  if (!explicit_path.empty()) {
    return open_explicit(explicit_path, operations, false, false);
  }
  std::array<std::array<char, 64>, kMaximumEnumeratedDevices> paths{};
  const std::size_t count = operations.enumerate(paths);
  if (count == 0) return {.error = LinuxCameraDeviceError::NotFound};

  LinuxCameraOpenResult match{.error = LinuxCameraDeviceError::NotFound};
  std::size_t matches = 0;
  for (std::size_t index = 0; index < std::min(count, paths.size()); ++index) {
    const auto end = std::find(paths[index].begin(), paths[index].end(), '\0');
    const std::string_view path(paths[index].data(),
                                static_cast<std::size_t>(end - paths[index].begin()));
    if (!valid_path(path)) continue;
    auto candidate = open_explicit(path, operations, true, false);
    if (candidate.error != LinuxCameraDeviceError::None) continue;
    ++matches;
    if (matches == 1) {
      match = candidate;
    } else {
      operations.close_descriptor(candidate.descriptor);
    }
  }
  if (matches == 0) return {.error = LinuxCameraDeviceError::NotFound};
  if (matches > 1) {
    operations.close_descriptor(match.descriptor);
    return {.error = LinuxCameraDeviceError::Ambiguous};
  }
  return match;
}

}  // namespace noisefactor::sync::camera
