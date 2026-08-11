#include <sync/secure_memory.hpp>

#include <openssl/crypto.h>

namespace noisefactor::sync {

void secure_cleanse(std::span<std::byte> bytes,
                    CleanseObserver* observer) noexcept {
  if (bytes.empty()) return;
  OPENSSL_cleanse(bytes.data(), bytes.size());
  if (observer != nullptr) observer->after_cleanse(bytes);
}

}  // namespace noisefactor::sync
