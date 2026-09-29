#pragma once
#include <cstdint>

namespace noisefactor::sync::update {
// Every managed daemon keeps a shared lock for its entire lifetime. Maintenance
// takes the exclusive lock only after all reserved daemons have stopped.
class LinuxStartupLock final {
 public:
  LinuxStartupLock() = default;
  ~LinuxStartupLock();
  LinuxStartupLock(const LinuxStartupLock&) = delete;
  LinuxStartupLock& operator=(const LinuxStartupLock&) = delete;
  [[nodiscard]] bool acquire(
      const char* lock = "/run/noisedeck-sync-update.lock",
      const char* enrollment = "/etc/noisedeck-sync/update.json",
      const char* marker = "/var/lib/noisedeck-sync-update/transaction.json",
      std::uint32_t owner = 0) noexcept;
 private:
  int descriptor_ = -1;
};
}
