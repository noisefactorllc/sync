#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace noisefactor::sync::detail {

struct MetalDeviceAttributes {
  std::uint64_t registry_id = 0;
  bool removable = false;
  bool headless = false;
};

[[nodiscard]] constexpr auto select_metal_device_index(
    std::span<const MetalDeviceAttributes> devices) noexcept -> std::optional<std::size_t> {
  std::optional<std::size_t> preferred;
  std::optional<std::size_t> fallback;
  for (std::size_t index = 0; index < devices.size(); ++index) {
    const MetalDeviceAttributes& candidate = devices[index];
    if (!fallback.has_value() ||
        candidate.registry_id < devices[*fallback].registry_id) {
      fallback = index;
    }
    if (!candidate.removable && !candidate.headless &&
        (!preferred.has_value() ||
         candidate.registry_id < devices[*preferred].registry_id)) {
      preferred = index;
    }
  }
  return preferred.has_value() ? preferred : fallback;
}

}  // namespace noisefactor::sync::detail
