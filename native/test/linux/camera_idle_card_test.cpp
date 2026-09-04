#include "../test_harness.hpp"

#include <sync/camera/nv12.hpp>
#include <sync/platform/camera_idle_card.hpp>
#include <sync/platform/camera_identity.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

namespace camera = noisefactor::sync::camera;
constexpr std::size_t kStride =
    static_cast<std::size_t>(camera::kCanvas.width) * camera::kBytesPerPixel;

bool background_at(std::span<const std::byte> bytes, std::uint32_t x,
                   std::uint32_t y) {
  const std::size_t offset = static_cast<std::size_t>(y) * kStride + x * 4U;
  return bytes[offset] == std::byte{0x14} &&
         bytes[offset + 1] == std::byte{0x14} &&
         bytes[offset + 2] == std::byte{0x14} &&
         bytes[offset + 3] == std::byte{0xff};
}

}  // namespace

SYNC_TEST(linux_idle_card_is_opaque_centered_and_deterministic) {
  std::vector<std::byte> first(kStride * camera::kCanvas.height);
  std::vector<std::byte> second(first.size());
  SYNC_REQUIRE(camera::draw_camera_idle_card(first, kStride, camera::kCanvas));
  SYNC_REQUIRE(camera::draw_camera_idle_card(second, kStride,
                                             camera::kCanvas));
  SYNC_REQUIRE(first == second);
  SYNC_REQUIRE(background_at(first, 0, 0));
  std::size_t center_text_pixels = 0;
  std::size_t outside_text_pixels = 0;
  for (std::uint32_t y = 0; y < camera::kCanvas.height; ++y) {
    for (std::uint32_t x = 0; x < camera::kCanvas.width; ++x) {
      const std::size_t alpha = static_cast<std::size_t>(y) * kStride +
                                static_cast<std::size_t>(x) * 4U + 3U;
      SYNC_REQUIRE(first[alpha] == std::byte{0xff});
      if (!background_at(first, x, y)) {
        if (y >= 450 && y < 630) ++center_text_pixels;
        else ++outside_text_pixels;
      }
    }
  }
  SYNC_REQUIRE(center_text_pixels > 1000);
  SYNC_REQUIRE(outside_text_pixels == 0);
}

SYNC_TEST(linux_idle_card_rejects_short_storage_without_touching_sentinels) {
  std::vector<std::byte> short_buffer(kStride * camera::kCanvas.height - 1,
                                      std::byte{0x5a});
  SYNC_REQUIRE(!camera::draw_camera_idle_card(short_buffer, kStride,
                                              camera::kCanvas));
  SYNC_REQUIRE(std::ranges::all_of(short_buffer,
                                   [](std::byte value) {
                                     return value == std::byte{0x5a};
                                   }));
  std::vector<std::byte> narrow(kStride * camera::kCanvas.height,
                                std::byte{0x6b});
  SYNC_REQUIRE(!camera::draw_camera_idle_card(narrow, kStride - 1,
                                              camera::kCanvas));
  SYNC_REQUIRE(narrow.front() == std::byte{0x6b});
  SYNC_REQUIRE(narrow.back() == std::byte{0x6b});
}

SYNC_TEST(linux_idle_card_converts_to_nonuniform_nv12_luma) {
  std::vector<std::byte> bgra(kStride * camera::kCanvas.height);
  SYNC_REQUIRE(camera::draw_camera_idle_card(bgra, kStride, camera::kCanvas));
  const std::size_t nv12_size = camera::nv12_size_bytes(
      camera::kCanvas.width, camera::kCanvas.height, camera::kCanvas.width);
  std::vector<std::byte> nv12(nv12_size);
  SYNC_REQUIRE(camera::bgra_to_nv12(
      bgra, kStride, camera::kCanvas.width, camera::kCanvas.height, nv12,
      camera::kCanvas.width));
  const auto [minimum, maximum] = std::minmax_element(
      nv12.begin(), nv12.begin() +
                        static_cast<std::ptrdiff_t>(camera::kCanvas.width) *
                            camera::kCanvas.height);
  SYNC_REQUIRE(*minimum != *maximum);
}
