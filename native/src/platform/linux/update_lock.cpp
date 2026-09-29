#include <sync/platform/linux_update_lock.hpp>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace noisefactor::sync::update {
namespace {
bool absent(const char* path) noexcept {
  struct stat info{};
  return ::lstat(path, &info) != 0 && errno == ENOENT;
}
}
LinuxStartupLock::~LinuxStartupLock() {
  if (descriptor_ >= 0) ::close(descriptor_);
}
bool LinuxStartupLock::acquire(const char* lock, const char* enrollment,
                               const char* marker, std::uint32_t owner) noexcept {
  if (descriptor_ >= 0) return false;
  const int candidate = ::open(lock, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (candidate < 0) {
    return errno == ENOENT && absent(enrollment) && absent(marker);
  }
  struct stat info{};
  const bool valid = ::fstat(candidate, &info) == 0 && S_ISREG(info.st_mode) &&
      info.st_uid == owner && (info.st_mode & 0022) == 0 &&
      ::flock(candidate, LOCK_SH | LOCK_NB) == 0 && absent(marker);
  if (!valid) {
    ::close(candidate);
    return false;
  }
  descriptor_ = candidate;
  return true;
}
}
