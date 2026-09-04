#include "../test_harness.hpp"

#include <sync/camera/nv12.hpp>
#include <sync/platform/linux_camera_device.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace camera = noisefactor::sync::camera;

class DeviceOps final : public camera::LinuxCameraDeviceOps {
 public:
  auto enumerate(std::span<std::array<char, 64>> output) noexcept
      -> std::size_t override {
    const std::size_t copied = std::min(output.size(), paths.size());
    for (std::size_t index = 0; index < copied; ++index) {
      std::copy(paths[index].begin(), paths[index].end(), output[index].begin());
    }
    return paths.size();
  }

  auto open_no_follow(std::string_view path) noexcept -> int override {
    opened.emplace_back(path);
    if (open_result < 0) errno = open_error;
    return open_result;
  }

  auto validate_character_device(int) noexcept -> bool override {
    return character_device;
  }

  auto query_capabilities(int, v4l2_capability& output) noexcept
      -> int override {
    output = capabilities;
    if (query_result < 0) errno = query_error;
    return query_result;
  }

  auto set_nv12_format(int, v4l2_format& format) noexcept -> int override {
    ++format_calls;
    requested_format = format;
    if (format_result < 0) {
      errno = format_error;
      return format_result;
    }
    format.fmt.pix.width = returned_width;
    format.fmt.pix.height = returned_height;
    format.fmt.pix.bytesperline = returned_stride;
    format.fmt.pix.sizeimage = returned_size;
    format.fmt.pix.pixelformat = returned_pixel_format;
    return 0;
  }

  auto set_frame_rate(int, v4l2_streamparm& rate) noexcept -> int override {
    requested_rate = rate;
    return rate_result;
  }

  auto write_frame(int, std::span<const std::byte>) noexcept
      -> std::pair<std::ptrdiff_t, std::int32_t> override {
    return {-1, ENOSYS};
  }

  void close_descriptor(int descriptor) noexcept override {
    closed.push_back(descriptor);
  }

  void compatible(std::string_view card = "Sync Camera") {
    capabilities = {};
    std::copy_n("v4l2 loopback", 13, capabilities.driver);
    std::copy(card.begin(), card.end(), capabilities.card);
    capabilities.capabilities = V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_READWRITE;
    returned_width = 1920;
    returned_height = 1080;
    returned_stride = 1920;
    returned_size = static_cast<std::uint32_t>(
        camera::nv12_size_bytes(1920, 1080, returned_stride));
    returned_pixel_format = V4L2_PIX_FMT_NV12;
  }

  std::vector<std::string> paths;
  std::vector<std::string> opened;
  std::vector<int> closed;
  int open_result = 7;
  int open_error = EACCES;
  bool character_device = true;
  v4l2_capability capabilities{};
  int query_result = 0;
  int query_error = EIO;
  int format_result = 0;
  unsigned format_calls = 0;
  int format_error = EINVAL;
  std::uint32_t returned_width = 1920;
  std::uint32_t returned_height = 1080;
  std::uint32_t returned_stride = 1920;
  std::uint32_t returned_size = 1920 * 1080 * 3 / 2;
  std::uint32_t returned_pixel_format = V4L2_PIX_FMT_NV12;
  int rate_result = 0;
  v4l2_format requested_format{};
  v4l2_streamparm requested_rate{};
};

}  // namespace

SYNC_TEST(linux_camera_device_requires_one_exact_automatic_match) {
  DeviceOps none;
  SYNC_REQUIRE(camera::open_linux_camera({}, none).error ==
               camera::LinuxCameraDeviceError::NotFound);

  DeviceOps two;
  two.compatible();
  two.paths = {"/dev/video2", "/dev/video12"};
  const auto ambiguous = camera::open_linux_camera({}, two);
  SYNC_REQUIRE(ambiguous.error == camera::LinuxCameraDeviceError::Ambiguous);
  SYNC_REQUIRE(two.opened.size() == 2);
  SYNC_REQUIRE(two.closed.size() == 2);

  DeviceOps physical;
  physical.compatible();
  std::copy_n("uvcvideo", 8, physical.capabilities.driver);
  physical.paths = {"/dev/video3"};
  SYNC_REQUIRE(camera::open_linux_camera({}, physical).error ==
               camera::LinuxCameraDeviceError::NotFound);
}

SYNC_TEST(linux_camera_device_validates_descriptor_and_exact_format) {
  DeviceOps operations;
  operations.compatible();
  operations.paths = {"/dev/video8"};
  const auto opened = camera::open_linux_camera({}, operations);
  SYNC_REQUIRE(opened.error == camera::LinuxCameraDeviceError::None);
  SYNC_REQUIRE(opened.descriptor == 7);
  SYNC_REQUIRE(std::string_view(opened.path.data()) == "/dev/video8");
  SYNC_REQUIRE(opened.format.width == 1920);
  SYNC_REQUIRE(opened.format.height == 1080);
  SYNC_REQUIRE(opened.format.y_stride == 1920);
  SYNC_REQUIRE(opened.format.size_image == 1920 * 1080 * 3 / 2);
  SYNC_REQUIRE(operations.requested_format.type == V4L2_BUF_TYPE_VIDEO_OUTPUT);
  SYNC_REQUIRE(operations.requested_format.fmt.pix.width == 1920);
  SYNC_REQUIRE(operations.requested_format.fmt.pix.height == 1080);
  SYNC_REQUIRE(operations.requested_format.fmt.pix.pixelformat ==
               V4L2_PIX_FMT_NV12);
  SYNC_REQUIRE(operations.requested_format.fmt.pix.field == V4L2_FIELD_NONE);
  SYNC_REQUIRE(operations.requested_rate.parm.output.timeperframe.numerator == 1);
  SYNC_REQUIRE(operations.requested_rate.parm.output.timeperframe.denominator ==
               60);
}

SYNC_TEST(linux_camera_probe_accepts_an_exclusive_camera_while_the_daemon_owns_output) {
  DeviceOps operations;
  operations.compatible();
  operations.capabilities.capabilities =
      V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE;
  operations.paths = {"/dev/video8"};

  SYNC_REQUIRE(camera::open_linux_camera({}, operations).error ==
               camera::LinuxCameraDeviceError::NotFound);
  const auto probed = camera::probe_linux_camera({}, operations);
  SYNC_REQUIRE(probed.error == camera::LinuxCameraDeviceError::None);
  SYNC_REQUIRE(std::string_view(probed.path.data()) == "/dev/video8");
  SYNC_REQUIRE(operations.format_calls == 0);
  operations.close_descriptor(probed.descriptor);
}

SYNC_TEST(linux_camera_explicit_path_bypasses_only_card_name) {
  DeviceOps operations;
  operations.compatible("Operator Camera");
  SYNC_REQUIRE(camera::open_linux_camera("/dev/video42", operations).error ==
               camera::LinuxCameraDeviceError::None);

  operations.compatible();
  std::copy_n("uvcvideo", 8, operations.capabilities.driver);
  SYNC_REQUIRE(camera::open_linux_camera("/dev/video42", operations).error ==
               camera::LinuxCameraDeviceError::WrongDriver);
  operations.compatible();
  operations.character_device = false;
  SYNC_REQUIRE(camera::open_linux_camera("/dev/video42", operations).error ==
               camera::LinuxCameraDeviceError::NotCharacterDevice);
  operations.character_device = true;
  operations.capabilities.capabilities = V4L2_CAP_VIDEO_CAPTURE;
  SYNC_REQUIRE(camera::open_linux_camera("/dev/video42", operations).error ==
               camera::LinuxCameraDeviceError::MissingOutputCapability);
  SYNC_REQUIRE(camera::open_linux_camera("/dev/video1/../video2", operations)
                   .error == camera::LinuxCameraDeviceError::InvalidPath);
}

SYNC_TEST(linux_camera_device_rejects_negotiation_drift_and_unsafe_sizes) {
  auto result_for = [](auto configure) {
    DeviceOps operations;
    operations.compatible();
    configure(operations);
    return camera::open_linux_camera("/dev/video4", operations).error;
  };
  SYNC_REQUIRE(result_for([](DeviceOps& value) { value.returned_width = 1280; }) ==
               camera::LinuxCameraDeviceError::InvalidFormatBounds);
  SYNC_REQUIRE(result_for([](DeviceOps& value) { value.returned_height = 720; }) ==
               camera::LinuxCameraDeviceError::InvalidFormatBounds);
  SYNC_REQUIRE(result_for([](DeviceOps& value) { value.returned_stride = 1919; }) ==
               camera::LinuxCameraDeviceError::InvalidFormatBounds);
  SYNC_REQUIRE(result_for([](DeviceOps& value) { value.returned_size = 1; }) ==
               camera::LinuxCameraDeviceError::InvalidFormatBounds);
  SYNC_REQUIRE(result_for([](DeviceOps& value) {
                 value.returned_size = 64U * 1024U * 1024U + 1U;
               }) == camera::LinuxCameraDeviceError::InvalidFormatBounds);
  SYNC_REQUIRE(result_for([](DeviceOps& value) {
                 value.returned_pixel_format = V4L2_PIX_FMT_YUYV;
               }) == camera::LinuxCameraDeviceError::FormatRejected);
}
