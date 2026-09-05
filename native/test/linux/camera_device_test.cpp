#include "../test_harness.hpp"

#include <sync/camera/nv12.hpp>
#include <sync/platform/linux_camera_device.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
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
    if (replace_color_metadata) {
      format.fmt.pix.colorspace = returned_colors.colorspace;
      format.fmt.pix.ycbcr_enc = returned_colors.ycbcr_enc;
      format.fmt.pix.quantization = returned_colors.quantization;
      format.fmt.pix.xfer_func = returned_colors.xfer_func;
    }
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
    capabilities.capabilities = V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_READWRITE |
                                V4L2_CAP_EXT_PIX_FORMAT;
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
  bool replace_color_metadata = false;
  v4l2_pix_format returned_colors{};
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

SYNC_TEST(linux_camera_color_metadata_decodes_the_nv12_encoder_without_color_shift) {
  DeviceOps operations;
  operations.compatible();
  SYNC_REQUIRE(camera::open_linux_camera("/dev/video8", operations).error ==
               camera::LinuxCameraDeviceError::None);

  // Decode real encoder bytes as a color-managed receiver would: negotiated
  // range and matrix, inverse transfer, then sRGB display encoding. Missing
  // priv magic makes extended fields undefined, so only their defaults apply.
  const auto& format = operations.requested_format.fmt.pix;
  const auto colorspace = format.colorspace == V4L2_COLORSPACE_DEFAULT
                              ? static_cast<std::uint32_t>(V4L2_COLORSPACE_SRGB)
                              : format.colorspace;
  const bool extended = format.priv == V4L2_PIX_FMT_PRIV_MAGIC;
  const auto encoding = !extended || format.ycbcr_enc == V4L2_YCBCR_ENC_DEFAULT
                            ? static_cast<std::uint32_t>(
                                  V4L2_MAP_YCBCR_ENC_DEFAULT(colorspace))
                            : format.ycbcr_enc;
  const auto transfer = !extended || format.xfer_func == V4L2_XFER_FUNC_DEFAULT
                            ? static_cast<std::uint32_t>(
                                  V4L2_MAP_XFER_FUNC_DEFAULT(colorspace))
                            : format.xfer_func;
  const bool full_range = extended && format.quantization == V4L2_QUANTIZATION_FULL_RANGE;
  const double kr = encoding == V4L2_YCBCR_ENC_709 ? 0.2126 : 0.299;
  const double kb = encoding == V4L2_YCBCR_ENC_709 ? 0.0722 : 0.114;
  const auto display_byte = [transfer](double value) {
    const double encoded = std::clamp(value / 255.0, 0.0, 1.0);
    const double linear = transfer == V4L2_XFER_FUNC_SRGB
                              ? (encoded <= 0.04045 ? encoded / 12.92
                                   : std::pow((encoded + 0.055) / 1.055, 2.4))
                              : (encoded < 0.081 ? encoded / 4.5
                                   : std::pow((encoded + 0.099) / 1.099, 1.0 / 0.45));
    return 255.0 * (linear <= 0.0031308 ? 12.92 * linear
                     : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055);
  };
  for (const std::array<int, 3> rgb : {std::array{96, 192, 64},
       std::array{128, 128, 128}, std::array{16, 16, 16},
       std::array{0, 0, 0}, std::array{255, 255, 255}}) {
    std::array<std::byte, 16> bgra{};
    for (std::size_t pixel = 0; pixel < 4; ++pixel) {
      bgra[pixel * 4] = static_cast<std::byte>(rgb[2]);
      bgra[pixel * 4 + 1] = static_cast<std::byte>(rgb[1]);
      bgra[pixel * 4 + 2] = static_cast<std::byte>(rgb[0]);
      bgra[pixel * 4 + 3] = std::byte{255};
    }
    std::array<std::byte, 6> nv12{};
    SYNC_REQUIRE(camera::bgra_to_nv12(bgra, 8, 2, 2, nv12, 2));
    const double y = (std::to_integer<int>(nv12[0]) - (full_range ? 0 : 16)) *
                     255.0 / (full_range ? 255.0 : 219.0);
    const double u = (std::to_integer<int>(nv12[4]) - 128) *
                     255.0 / (full_range ? 255.0 : 224.0);
    const double v = (std::to_integer<int>(nv12[5]) - 128) *
                     255.0 / (full_range ? 255.0 : 224.0);
    const double r = y + 2.0 * (1.0 - kr) * v;
    const double b = y + 2.0 * (1.0 - kb) * u;
    const double g = (y - kr * r - kb * b) / (1.0 - kr - kb);
    SYNC_REQUIRE(std::abs(display_byte(r) - rgb[0]) <= 2.0);
    SYNC_REQUIRE(std::abs(display_byte(g) - rgb[1]) <= 2.0);
    SYNC_REQUIRE(std::abs(display_byte(b) - rgb[2]) <= 2.0);
  }
}

SYNC_TEST(linux_camera_rejects_devices_without_extended_color_metadata) {
  DeviceOps operations;
  operations.compatible();
  operations.capabilities.capabilities &= ~V4L2_CAP_EXT_PIX_FORMAT;
  SYNC_REQUIRE(camera::open_linux_camera("/dev/video8", operations).error ==
               camera::LinuxCameraDeviceError::FormatRejected);
  SYNC_REQUIRE(operations.format_calls == 0);
  SYNC_REQUIRE(operations.closed == std::vector<int>{7});
}

SYNC_TEST(linux_camera_rejects_negotiated_color_metadata_that_changes_pixels) {
  for (const auto mismatch : {0, 1, 2}) {
    DeviceOps operations;
    operations.compatible();
    operations.replace_color_metadata = true;
    operations.returned_colors.colorspace = V4L2_COLORSPACE_REC709;
    operations.returned_colors.ycbcr_enc = V4L2_YCBCR_ENC_709;
    operations.returned_colors.xfer_func = V4L2_XFER_FUNC_SRGB;
    operations.returned_colors.quantization = V4L2_QUANTIZATION_LIM_RANGE;
    if (mismatch == 0) operations.returned_colors.xfer_func = V4L2_XFER_FUNC_709;
    if (mismatch == 1) operations.returned_colors.ycbcr_enc = V4L2_YCBCR_ENC_601;
    if (mismatch == 2) operations.returned_colors.quantization = V4L2_QUANTIZATION_FULL_RANGE;
    SYNC_REQUIRE(camera::open_linux_camera("/dev/video8", operations).error ==
                 camera::LinuxCameraDeviceError::FormatRejected);
    SYNC_REQUIRE(operations.closed == std::vector<int>{7});
  }
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
