#include "../../pairing_store_fs.hpp"

// This is the platform-specific half of PairingStore that used to live
// directly in native/src/pairing_store.cpp before the Windows port required
// splitting it out. Each function here is moved verbatim from that file:
// same syscalls, same flags, same order, same error mapping.
//
// One thing above this seam did change, and it is worth stating rather
// than leaving to be discovered: PairingStore::persist() used to open the
// store directory once and use that single fd for the temp-file create,
// write, fsync, rename, and cleanup. It now calls three separate seam
// functions, each of which resolves the directory itself. The individual
// operations are still fd-anchored and so still cannot be redirected by a
// symlink swap; what is no longer guaranteed is that all of them act on
// the same directory *object* if it is replaced between calls. Against the
// owner-only directory this store requires, an attacker able to do that
// can already read and write the store directly.

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/acl.h>
#endif

namespace noisefactor::sync::fs {

// kMaximumPathComponentBytes (shared with pairing_store.cpp's
// path_is_bounded_absolute) must track this platform's real NAME_MAX, or
// this file's fixed-size component buffers could either reject valid names
// the OS would accept, or -- far worse -- silently truncate a name the
// neutral validator already approved at a different length.
static_assert(kMaximumPathComponentBytes == NAME_MAX);

ScopedHandle::ScopedHandle() noexcept : value_(-1) {}
ScopedHandle::~ScopedHandle() noexcept { close(); }
ScopedHandle::ScopedHandle(int native) noexcept : value_(native) {}

ScopedHandle::ScopedHandle(ScopedHandle&& other) noexcept : value_(other.value_) {
  other.value_ = -1;
}

ScopedHandle& ScopedHandle::operator=(ScopedHandle&& other) noexcept {
  if (this != &other) {
    close();
    value_ = other.value_;
    other.value_ = -1;
  }
  return *this;
}

bool ScopedHandle::valid() const noexcept { return value_ >= 0; }

void ScopedHandle::close() noexcept {
  if (value_ >= 0) {
    ::close(value_);
    value_ = -1;
  }
}

int ScopedHandle::native() const noexcept { return value_; }

namespace {

#if defined(__APPLE__)
class ScopedAcl {
 public:
  explicit ScopedAcl(acl_t value) noexcept : value_(value) {}
  ~ScopedAcl() { if (value_ != nullptr) ::acl_free(value_); }
  [[nodiscard]] acl_t get() const noexcept { return value_; }

 private:
  acl_t value_;
};
#endif

// macOS files/directories can carry an "extended" (NFSv4-style) ACL layered
// on top of the traditional mode bits; POSIX mode 0700/0600 alone is not
// sufficient there because a permissive ACL entry can grant access the mode
// bits appear to deny. These two helpers are the ACL half of "owner-only":
// strip_extended_acl removes any such ACL from an object this code just
// created, and extended_acl_is_empty refuses any pre-existing object that
// still carries one. Both are no-ops (an unconditional pass) on
// non-Darwin POSIX, which has no extended-ACL analogue to worry about.
bool extended_acl_is_empty(int fd) noexcept {
#if defined(__APPLE__)
  errno = 0;
  ScopedAcl acl(::acl_get_fd_np(fd, ACL_TYPE_EXTENDED));
  if (acl.get() == nullptr) return errno == ENOENT;
  acl_entry_t entry = nullptr;
  errno = 0;
  const int result = ::acl_get_entry(acl.get(), ACL_FIRST_ENTRY, &entry);
  return result != 0 && errno == EINVAL;
#else
  (void)fd;
  return true;
#endif
}

bool strip_extended_acl(int fd) noexcept {
#if defined(__APPLE__)
  ScopedAcl empty(::acl_init(0));
  return empty.get() != nullptr &&
         ::acl_set_fd_np(fd, empty.get(), ACL_TYPE_EXTENDED) == 0 &&
         extended_acl_is_empty(fd);
#else
  (void)fd;
  return true;
#endif
}

bool full_read(int fd, unsigned char* output, std::size_t length) noexcept {
  std::size_t offset = 0;
  while (offset < length) {
    const ssize_t amount = ::read(fd, output + offset, length - offset);
    if (amount < 0 && errno == EINTR) continue;
    if (amount <= 0) return false;
    offset += static_cast<std::size_t>(amount);
  }
  return true;
}

bool full_write(int fd, const unsigned char* input, std::size_t length) noexcept {
  std::size_t offset = 0;
  while (offset < length) {
    const ssize_t amount = ::write(fd, input + offset, length - offset);
    if (amount < 0 && errno == EINTR) continue;
    if (amount <= 0) return false;
    offset += static_cast<std::size_t>(amount);
  }
  return true;
}

// Copies a single already-length-checked path component into a
// NAME_MAX-bounded, NUL-padded buffer suitable for the *at() family below.
std::array<char, kMaximumPathComponentBytes + 1> component_buffer(std::string_view name) noexcept {
  std::array<char, kMaximumPathComponentBytes + 1> buffer{};
  std::memcpy(buffer.data(), name.data(), name.size());
  return buffer;
}

}  // namespace

PairingStoreError open_secure_directory(std::string_view directory, ScopedHandle& output,
                                        bool create) noexcept {
  if (directory.empty() || directory.front() != '/' || directory == "/") {
    return PairingStoreError::InvalidPath;
  }
  ScopedHandle current(::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY));
  if (!current.valid()) return PairingStoreError::Io;
  std::size_t start = 1;
  while (start < directory.size()) {
    const std::size_t slash = directory.find('/', start);
    const std::size_t end = slash == std::string_view::npos ? directory.size() : slash;
    const auto component = directory.substr(start, end - start);
    if (component.empty() || component.size() > NAME_MAX) return PairingStoreError::InvalidPath;
    const auto name = component_buffer(component);
    const bool final = end == directory.size();
    bool created = false;
    int next_fd = ::openat(current.native(), name.data(),
                           O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (next_fd < 0 && errno == ENOENT && create) {
      const int mkdir_result = ::mkdirat(current.native(), name.data(), 0700);
      if (mkdir_result != 0 && errno != EEXIST) {
        return PairingStoreError::Io;
      }
      created = mkdir_result == 0;
      if (created && ::fchmodat(current.native(), name.data(), 0700, 0) != 0) {
        return PairingStoreError::Io;
      }
      next_fd = ::openat(current.native(), name.data(),
                         O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    }
    if (next_fd < 0) {
      return errno == ELOOP || errno == ENOTDIR ? PairingStoreError::DirectorySecurity
                                                : PairingStoreError::Io;
    }
    ScopedHandle next(next_fd);
    struct stat status {};
    if (::fstat(next.native(), &status) != 0 || !S_ISDIR(status.st_mode)) {
      return PairingStoreError::DirectorySecurity;
    }
    if (created && (::fchmod(next.native(), 0700) != 0 || !strip_extended_acl(next.native()) ||
                    ::fsync(current.native()) != 0)) {
      return PairingStoreError::Io;
    }
    if (final && (status.st_uid != ::geteuid() ||
                  ((!created) && (status.st_mode & 0777) != 0700))) {
      return PairingStoreError::DirectorySecurity;
    }
    if (final && created) {
      if (::fstat(next.native(), &status) != 0 || status.st_uid != ::geteuid() ||
          (status.st_mode & 0777) != 0700) {
        return PairingStoreError::DirectorySecurity;
      }
    } else if (final && !extended_acl_is_empty(next.native())) {
      return PairingStoreError::DirectorySecurity;
    }
    current = std::move(next);
    if (final) {
      output = std::move(current);
      return PairingStoreError::None;
    }
    start = end + 1;
  }
  return PairingStoreError::InvalidPath;
}

PairingStoreError acquire_store_lock(std::string_view directory, ScopedHandle& lock) noexcept {
  ScopedHandle dir;
  PairingStoreError error = open_secure_directory(directory, dir, false);
  if (error != PairingStoreError::None) return error;
  constexpr char kLockName[] = ".pairings.lock";
  bool created = false;
  int fd = ::openat(dir.native(), kLockName,
                    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd >= 0) {
    created = true;
  } else if (errno == EEXIST) {
    fd = ::openat(dir.native(), kLockName, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  }
  if (fd < 0) return PairingStoreError::FileSecurity;
  lock = ScopedHandle(fd);
  struct stat status {};
  if ((created && (::fchmod(lock.native(), 0600) != 0 || !strip_extended_acl(lock.native()))) ||
      ::fstat(lock.native(), &status) != 0 ||
      !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
      (status.st_mode & 0777) != 0600 || status.st_nlink != 1 ||
      (!created && !extended_acl_is_empty(lock.native()))) {
    return PairingStoreError::FileSecurity;
  }
  constexpr std::size_t kAttempts = 50;
  const timespec pause{.tv_sec = 0, .tv_nsec = 2'000'000};
  for (std::size_t attempt = 0; attempt < kAttempts; ++attempt) {
    if (::flock(lock.native(), LOCK_EX | LOCK_NB) == 0) return PairingStoreError::None;
    if (errno != EWOULDBLOCK && errno != EINTR) return PairingStoreError::Io;
    ::nanosleep(&pause, nullptr);
  }
  return PairingStoreError::Busy;
}

LoadResult load_secure_regular_file(std::string_view directory, std::string_view filename,
                                    std::span<unsigned char> buffer) noexcept {
  LoadResult result{};
  ScopedHandle dir;
  const PairingStoreError directory_error = open_secure_directory(directory, dir, false);
  if (directory_error != PairingStoreError::None) {
    result.error = directory_error;
    return result;
  }
  const auto name = component_buffer(filename);
  struct stat path_status {};
  if (::fstatat(dir.native(), name.data(), &path_status, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      result.error = PairingStoreError::None;
      result.exists = false;
      return result;
    }
    result.error = PairingStoreError::Io;
    return result;
  }
  if (!S_ISREG(path_status.st_mode) || S_ISLNK(path_status.st_mode) ||
      path_status.st_uid != ::geteuid() || (path_status.st_mode & 0777) != 0600 ||
      path_status.st_nlink != 1) {
    result.error = PairingStoreError::FileSecurity;
    return result;
  }
  ScopedHandle file(::openat(dir.native(), name.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (!file.valid()) {
    result.error = PairingStoreError::FileSecurity;
    return result;
  }
  struct stat file_status {};
  if (::fstat(file.native(), &file_status) != 0 || file_status.st_dev != path_status.st_dev ||
      file_status.st_ino != path_status.st_ino || !S_ISREG(file_status.st_mode) ||
      file_status.st_uid != ::geteuid() || (file_status.st_mode & 0777) != 0600 ||
      file_status.st_nlink != 1 || !extended_acl_is_empty(file.native())) {
    result.error = PairingStoreError::FileSecurity;
    return result;
  }
  if (file_status.st_size <= 0 ||
      static_cast<std::uint64_t>(file_status.st_size) > buffer.size()) {
    result.error = PairingStoreError::Corrupt;
    return result;
  }
  const std::size_t size = static_cast<std::size_t>(file_status.st_size);
  if (!full_read(file.native(), buffer.data(), size)) {
    result.error = PairingStoreError::Io;
    return result;
  }
  unsigned char extra = 0;
  ssize_t extra_size = 0;
  do {
    extra_size = ::read(file.native(), &extra, 1);
  } while (extra_size < 0 && errno == EINTR);
  if (extra_size != 0) {
    result.error = PairingStoreError::Corrupt;
    return result;
  }
  result.error = PairingStoreError::None;
  result.exists = true;
  result.size = size;
  return result;
}

PairingStoreError write_new_secure_regular_file(std::string_view directory,
                                                 std::string_view filename,
                                                 std::span<const unsigned char> bytes) noexcept {
  ScopedHandle dir;
  const PairingStoreError directory_error = open_secure_directory(directory, dir, false);
  if (directory_error != PairingStoreError::None) return directory_error;
  const auto name = component_buffer(filename);

  ScopedHandle file(::openat(dir.native(), name.data(),
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (!file.valid()) return PairingStoreError::Io;
  struct stat temporary_status {};
  const bool ready =
      ::fchmod(file.native(), 0600) == 0 && strip_extended_acl(file.native()) &&
      ::fstat(file.native(), &temporary_status) == 0 && S_ISREG(temporary_status.st_mode) &&
      temporary_status.st_uid == ::geteuid() && (temporary_status.st_mode & 0777) == 0600 &&
      temporary_status.st_nlink == 1 && full_write(file.native(), bytes.data(), bytes.size()) &&
      ::fsync(file.native()) == 0;
  file.close();
  if (!ready) {
    ::unlinkat(dir.native(), name.data(), 0);
    return PairingStoreError::Io;
  }
  return PairingStoreError::None;
}

void remove_file(std::string_view directory, std::string_view filename) noexcept {
  ScopedHandle dir;
  if (open_secure_directory(directory, dir, false) != PairingStoreError::None) return;
  const auto name = component_buffer(filename);
  ::unlinkat(dir.native(), name.data(), 0);
}

CommitResult rename_into_place(std::string_view directory, std::string_view temporary_filename,
                               std::string_view final_filename,
                               bool simulate_directory_sync_failure) noexcept {
  CommitResult result{};
  ScopedHandle dir;
  const PairingStoreError directory_error = open_secure_directory(directory, dir, false);
  if (directory_error != PairingStoreError::None) {
    // We never got a directory handle at all, so there is nothing to
    // renameat through; best-effort clean up the temp file via a fresh
    // resolve and report the directory failure.
    remove_file(directory, temporary_filename);
    result.error = directory_error;
    return result;
  }
  const auto temporary_name = component_buffer(temporary_filename);
  const auto final_name = component_buffer(final_filename);
  if (::renameat(dir.native(), temporary_name.data(), dir.native(), final_name.data()) != 0) {
    ::unlinkat(dir.native(), temporary_name.data(), 0);
    result.error = PairingStoreError::Io;
    return result;
  }
  // fsync(directory) is what makes the rename's directory-entry update
  // durable across a crash; the fail-point short-circuits it exactly like
  // the AfterRenameBeforeDirectorySync path always did, to deterministically
  // exercise the CommittedDurabilityUncertain branch in tests.
  if (simulate_directory_sync_failure || ::fsync(dir.native()) != 0) {
    result.error = PairingStoreError::Io;
    result.commit = PairingCommitState::CommittedDurabilityUncertain;
    return result;
  }
  result.commit = PairingCommitState::CommittedDurable;
  return result;
}

}  // namespace noisefactor::sync::fs
