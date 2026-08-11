#pragma once

#include <cstddef>
#include <span>

namespace noisefactor::sync {

class CleanseObserver {
 public:
  virtual ~CleanseObserver() = default;
  virtual void after_cleanse(std::span<const std::byte> bytes) noexcept = 0;
  virtual void before_sensitive_fragment_reserve(
      std::size_t, std::size_t) noexcept {}
};

void secure_cleanse(std::span<std::byte> bytes,
                    CleanseObserver* observer = nullptr) noexcept;

}  // namespace noisefactor::sync
