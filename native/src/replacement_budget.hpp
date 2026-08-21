#pragma once

#include <cstddef>
#include <limits>
#include <optional>

namespace noisefactor::sync::allocation {

// Returns the steady-state allocation after replacing `current_bytes` with
// `requested_bytes`, but only when both the transient replacement peak and
// the resulting retained total fit within `budget_bytes`.
[[nodiscard]] inline std::optional<std::size_t>
replacement_total_if_peak_fits(
    std::size_t allocated_bytes, std::size_t current_bytes,
    std::size_t requested_bytes, std::size_t budget_bytes) noexcept {
  if (current_bytes > allocated_bytes ||
      allocated_bytes > std::numeric_limits<std::size_t>::max() -
                            requested_bytes) {
    return std::nullopt;
  }
  const std::size_t peak_bytes = allocated_bytes + requested_bytes;
  if (peak_bytes > budget_bytes) return std::nullopt;

  // The retained portion is no larger than allocated_bytes, so the peak
  // check above also proves this addition cannot overflow.
  return allocated_bytes - current_bytes + requested_bytes;
}

}  // namespace noisefactor::sync::allocation
