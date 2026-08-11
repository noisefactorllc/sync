#include <sync/pairing_store.hpp>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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

namespace noisefactor::sync {
namespace {

constexpr std::array<unsigned char, 8> kMagic = {'N', 'F', 'S', 'Y', 'N', 'C', 'P', 'R'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kDigestBytes = 32;
constexpr std::size_t kMaximumSerializedBytes =
    kHeaderBytes + kMaximumPairingOrigins * (4 + kMaximumOriginBytes + kDigestBytes);

class ScopedFd {
 public:
  explicit ScopedFd(int value = -1) noexcept : value_(value) {}
  ~ScopedFd() { close(); }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  ScopedFd(ScopedFd&& other) noexcept : value_(other.release()) {}
  ScopedFd& operator=(ScopedFd&& other) noexcept {
    if (this != &other) {
      close();
      value_ = other.release();
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return value_; }
  [[nodiscard]] int release() noexcept {
    const int value = value_;
    value_ = -1;
    return value;
  }
  void close() noexcept {
    if (value_ >= 0) {
      ::close(value_);
      value_ = -1;
    }
  }

 private:
  int value_;
};

void cleanse(void* data, std::size_t length) noexcept;

class ScopedCleanse {
 public:
  ScopedCleanse(void* data, std::size_t length) noexcept : data_(data), length_(length) {}
  ~ScopedCleanse() { cleanse(data_, length_); }
  ScopedCleanse(const ScopedCleanse&) = delete;
  ScopedCleanse& operator=(const ScopedCleanse&) = delete;

 private:
  void* data_;
  std::size_t length_;
};

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

void cleanse(void* data, std::size_t length) noexcept {
  if (length > 0) OPENSSL_cleanse(data, length);
}

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

std::uint32_t read_u32(const unsigned char* bytes) noexcept {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint16_t read_u16(const unsigned char* bytes) noexcept {
  return static_cast<std::uint16_t>(bytes[0]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

void write_u32(unsigned char* bytes, std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[index] = static_cast<unsigned char>((value >> (index * 8U)) & 0xffU);
  }
}

void write_u16(unsigned char* bytes, std::uint16_t value) noexcept {
  bytes[0] = static_cast<unsigned char>(value & 0xffU);
  bytes[1] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

bool sha256(std::span<const unsigned char> input,
            std::array<unsigned char, kDigestBytes>& output) noexcept {
  std::size_t length = output.size();
  ERR_clear_error();
  const int ok = EVP_Q_digest(nullptr, "SHA256", nullptr, input.data(), input.size(),
                              output.data(), &length);
  ERR_clear_error();
  return ok == 1 && length == output.size();
}

bool decode_token(std::string_view token,
                  std::array<unsigned char, kPairingTokenBytes>& bytes) noexcept {
  if (token.size() != kPairingTokenHexBytes) return false;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    auto nibble = [](char value) noexcept -> int {
      if (value >= '0' && value <= '9') return value - '0';
      if (value >= 'a' && value <= 'f') return value - 'a' + 10;
      return -1;
    };
    const int high = nibble(token[index * 2]);
    const int low = nibble(token[index * 2 + 1]);
    if (high < 0 || low < 0) {
      cleanse(bytes.data(), bytes.size());
      return false;
    }
    bytes[index] = static_cast<unsigned char>((high << 4U) | low);
  }
  return true;
}

bool path_is_bounded_absolute(std::string_view path) noexcept {
  if (path.empty() || path.size() >= kMaximumPairingStorePathBytes || path.front() != '/' ||
      path.back() == '/') {
    return false;
  }
  for (const unsigned char byte : path) {
    if (byte < 0x20U || byte == 0x7fU || byte > 0x7eU) return false;
  }
  std::size_t start = 1;
  while (start < path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
    const auto component = path.substr(start, end - start);
    if (component.empty() || component.size() > NAME_MAX || component == "." ||
        component == "..") {
      return false;
    }
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return true;
}

struct PathParts {
  std::string_view directory;
  std::string_view filename;
};

PathParts split_path(std::string_view path) noexcept {
  const std::size_t slash = path.rfind('/');
  return {.directory = slash == 0 ? std::string_view("/") : path.substr(0, slash),
          .filename = path.substr(slash + 1)};
}

PairingStoreError open_secure_directory(std::string_view directory, ScopedFd& output,
                                        bool create) noexcept {
  if (directory.empty() || directory.front() != '/' || directory == "/") {
    return PairingStoreError::InvalidPath;
  }
  ScopedFd current(::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY));
  if (current.get() < 0) return PairingStoreError::Io;
  std::size_t start = 1;
  while (start < directory.size()) {
    const std::size_t slash = directory.find('/', start);
    const std::size_t end = slash == std::string_view::npos ? directory.size() : slash;
    const auto component = directory.substr(start, end - start);
    if (component.empty() || component.size() > NAME_MAX) return PairingStoreError::InvalidPath;
    std::array<char, NAME_MAX + 1> name{};
    std::memcpy(name.data(), component.data(), component.size());
    const bool final = end == directory.size();
    bool created = false;
    int next_fd = ::openat(current.get(), name.data(),
                           O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (next_fd < 0 && errno == ENOENT && create) {
      const int mkdir_result = ::mkdirat(current.get(), name.data(), 0700);
      if (mkdir_result != 0 && errno != EEXIST) {
        return PairingStoreError::Io;
      }
      created = mkdir_result == 0;
      if (created && ::fchmodat(current.get(), name.data(), 0700, 0) != 0) {
        return PairingStoreError::Io;
      }
      next_fd = ::openat(current.get(), name.data(),
                         O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    }
    if (next_fd < 0) {
      return errno == ELOOP || errno == ENOTDIR ? PairingStoreError::DirectorySecurity
                                                : PairingStoreError::Io;
    }
    ScopedFd next(next_fd);
    struct stat status {};
    if (::fstat(next.get(), &status) != 0 || !S_ISDIR(status.st_mode)) {
      return PairingStoreError::DirectorySecurity;
    }
    if (created && (::fchmod(next.get(), 0700) != 0 || !strip_extended_acl(next.get()) ||
                    ::fsync(current.get()) != 0)) {
      return PairingStoreError::Io;
    }
    if (final && (status.st_uid != ::geteuid() ||
                  ((!created) && (status.st_mode & 0777) != 0700))) {
      return PairingStoreError::DirectorySecurity;
    }
    if (final && created) {
      if (::fstat(next.get(), &status) != 0 || status.st_uid != ::geteuid() ||
          (status.st_mode & 0777) != 0700) {
        return PairingStoreError::DirectorySecurity;
      }
    } else if (final && !extended_acl_is_empty(next.get())) {
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

PairingStoreError acquire_store_lock(std::string_view path, ScopedFd& lock) noexcept {
  const PathParts parts = split_path(path);
  ScopedFd directory;
  PairingStoreError error = open_secure_directory(parts.directory, directory, false);
  if (error != PairingStoreError::None) return error;
  constexpr char kLockName[] = ".pairings.lock";
  bool created = false;
  int fd = ::openat(directory.get(), kLockName,
                    O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd >= 0) {
    created = true;
  } else if (errno == EEXIST) {
    fd = ::openat(directory.get(), kLockName, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  }
  if (fd < 0) return PairingStoreError::FileSecurity;
  lock = ScopedFd(fd);
  struct stat status {};
  if ((created && (::fchmod(lock.get(), 0600) != 0 || !strip_extended_acl(lock.get()))) ||
      ::fstat(lock.get(), &status) != 0 ||
      !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
      (status.st_mode & 0777) != 0600 || status.st_nlink != 1 ||
      (!created && !extended_acl_is_empty(lock.get()))) {
    return PairingStoreError::FileSecurity;
  }
  constexpr std::size_t kAttempts = 50;
  const timespec pause{.tv_sec = 0, .tv_nsec = 2'000'000};
  for (std::size_t attempt = 0; attempt < kAttempts; ++attempt) {
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) == 0) return PairingStoreError::None;
    if (errno != EWOULDBLOCK && errno != EINTR) return PairingStoreError::Io;
    ::nanosleep(&pause, nullptr);
  }
  return PairingStoreError::Busy;
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

bool same_origin(const NormalizedOrigin& left, const NormalizedOrigin& right) noexcept {
  return left.view() == right.view();
}

}  // namespace

bool PairingCommitGate::cancel() noexcept {
  State expected = State::Open;
  return state_.compare_exchange_strong(expected, State::Canceled,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire);
}

bool PairingCommitGate::try_begin_commit() noexcept {
  State expected = State::Open;
  return state_.compare_exchange_strong(expected, State::CommitClaimed,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire);
}

bool PairingCommitGate::canceled() const noexcept {
  return state_.load(std::memory_order_acquire) == State::Canceled;
}

PairingToken::~PairingToken() noexcept { clear(); }

PairingToken::PairingToken(PairingToken&& other) noexcept
    : bytes_(other.bytes_), length_(other.length_) {
  other.clear();
}

PairingToken& PairingToken::operator=(PairingToken&& other) noexcept {
  if (this != &other) {
    clear();
    bytes_ = other.bytes_;
    length_ = other.length_;
    other.clear();
  }
  return *this;
}

void PairingToken::clear() noexcept {
  cleanse(bytes_.data(), bytes_.size());
  length_ = 0;
}

PairingStore::~PairingStore() noexcept { clear(); }

PairingStore::PairingStore(PairingStore&& other) noexcept
    : path_(other.path_), path_length_(other.path_length_), records_(other.records_),
      record_count_(other.record_count_), fail_point_(other.fail_point_),
      commit_hook_(other.commit_hook_), opened_(other.opened_) {
  other.clear();
}

PairingStore& PairingStore::operator=(PairingStore&& other) noexcept {
  if (this != &other) {
    clear();
    path_ = other.path_;
    path_length_ = other.path_length_;
    records_ = other.records_;
    record_count_ = other.record_count_;
    fail_point_ = other.fail_point_;
    commit_hook_ = other.commit_hook_;
    opened_ = other.opened_;
    other.clear();
  }
  return *this;
}

void PairingStore::clear() noexcept {
  cleanse(path_.data(), path_.size());
  cleanse(records_.data(), sizeof(records_));
  path_length_ = 0;
  record_count_ = 0;
  fail_point_ = PairingStoreFailPoint::None;
  commit_hook_ = nullptr;
  opened_ = false;
}

PairingStoreError PairingStore::open(PairingStoreOptions options) noexcept {
  clear();
  if (!path_is_bounded_absolute(options.path)) return PairingStoreError::InvalidPath;
  std::memcpy(path_.data(), options.path.data(), options.path.size());
  path_length_ = options.path.size();
  fail_point_ = options.fail_point;
  commit_hook_ = options.commit_hook;

  const PathParts parts = split_path({path_.data(), path_length_});
  constexpr std::size_t kTemporarySuffixBytes = 21;
  if (parts.filename == ".pairings.lock" ||
      parts.filename.size() + kTemporarySuffixBytes > NAME_MAX) {
    clear();
    return PairingStoreError::InvalidPath;
  }
  ScopedFd directory;
  const PairingStoreError directory_error = open_secure_directory(parts.directory, directory, true);
  if (directory_error != PairingStoreError::None) {
    clear();
    return directory_error;
  }
  opened_ = true;
  ScopedFd lock;
  const PairingStoreError lock_error =
      acquire_store_lock({path_.data(), path_length_}, lock);
  if (lock_error != PairingStoreError::None) {
    clear();
    return lock_error;
  }
  const PairingStoreError error = reload();
  if (error != PairingStoreError::None) clear();
  return error;
}

PairingStoreError PairingStore::reload() noexcept {
  if (!opened_) return PairingStoreError::InvalidPath;
  const PathParts parts = split_path({path_.data(), path_length_});
  ScopedFd directory;
  PairingStoreError error = open_secure_directory(parts.directory, directory, false);
  if (error != PairingStoreError::None) return error;

  std::array<char, kMaximumPairingStorePathBytes> filename{};
  std::memcpy(filename.data(), parts.filename.data(), parts.filename.size());
  struct stat path_status {};
  if (::fstatat(directory.get(), filename.data(), &path_status, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      cleanse(records_.data(), sizeof(records_));
      record_count_ = 0;
      return PairingStoreError::None;
    }
    return PairingStoreError::Io;
  }
  if (!S_ISREG(path_status.st_mode) || S_ISLNK(path_status.st_mode) ||
      path_status.st_uid != ::geteuid() || (path_status.st_mode & 0777) != 0600 ||
      path_status.st_nlink != 1) {
    return PairingStoreError::FileSecurity;
  }
  ScopedFd file(::openat(directory.get(), filename.data(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (file.get() < 0) return PairingStoreError::FileSecurity;
  struct stat file_status {};
  if (::fstat(file.get(), &file_status) != 0 || file_status.st_dev != path_status.st_dev ||
      file_status.st_ino != path_status.st_ino || !S_ISREG(file_status.st_mode) ||
      file_status.st_uid != ::geteuid() || (file_status.st_mode & 0777) != 0600 ||
      file_status.st_nlink != 1 || !extended_acl_is_empty(file.get())) {
    return PairingStoreError::FileSecurity;
  }
  if (file_status.st_size <= 0 ||
      static_cast<std::uint64_t>(file_status.st_size) > kMaximumSerializedBytes) {
    return PairingStoreError::Corrupt;
  }
  const std::size_t size = static_cast<std::size_t>(file_status.st_size);
  std::array<unsigned char, kMaximumSerializedBytes> bytes{};
  ScopedCleanse bytes_cleanser(bytes.data(), bytes.size());
  if (!full_read(file.get(), bytes.data(), size)) return PairingStoreError::Io;
  unsigned char extra = 0;
  ssize_t extra_size = 0;
  do {
    extra_size = ::read(file.get(), &extra, 1);
  } while (extra_size < 0 && errno == EINTR);
  if (extra_size != 0) return PairingStoreError::Corrupt;
  if (size < kHeaderBytes ||
      CRYPTO_memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) {
    return PairingStoreError::Corrupt;
  }
  const std::uint32_t version = read_u32(bytes.data() + 8);
  if (version != kVersion) return PairingStoreError::UnknownVersion;
  const std::uint32_t count = read_u32(bytes.data() + 12);
  if (count > kMaximumPairingOrigins) return PairingStoreError::Corrupt;

  std::array<Record, kMaximumPairingOrigins> parsed{};
  ScopedCleanse parsed_cleanser(parsed.data(), sizeof(parsed));
  std::size_t offset = kHeaderBytes;
  for (std::size_t index = 0; index < count; ++index) {
    if (offset > size || size - offset < 4) return PairingStoreError::Corrupt;
    const std::uint16_t origin_length = read_u16(bytes.data() + offset);
    const std::uint16_t reserved = read_u16(bytes.data() + offset + 2);
    offset += 4;
    if (reserved != 0 || origin_length == 0 || origin_length > kMaximumOriginBytes ||
        offset > size || size - offset < static_cast<std::size_t>(origin_length) + kDigestBytes) {
      return PairingStoreError::Corrupt;
    }
    const std::string_view serialized_origin(
        reinterpret_cast<const char*>(bytes.data() + offset), origin_length);
    const auto normalized = normalize_origin(serialized_origin);
    if (!normalized.ok() || normalized.origin.view() != serialized_origin) {
      return PairingStoreError::Corrupt;
    }
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
      if (same_origin(parsed[earlier].origin, normalized.origin)) {
        return PairingStoreError::Corrupt;
      }
    }
    parsed[index].origin = normalized.origin;
    offset += origin_length;
    std::memcpy(parsed[index].digest.data(), bytes.data() + offset, kDigestBytes);
    offset += kDigestBytes;
  }
  if (offset != size) return PairingStoreError::Corrupt;
  cleanse(records_.data(), sizeof(records_));
  records_ = parsed;
  record_count_ = count;
  return PairingStoreError::None;
}

PairingStore::PersistResult PairingStore::persist(
    const std::array<Record, kMaximumPairingOrigins>& records, std::size_t count,
    PairingCommitGate* gate) noexcept {
  if (!opened_ || count > records.size()) return {.error = PairingStoreError::InvalidPath};
  std::array<unsigned char, kMaximumSerializedBytes> bytes{};
  ScopedCleanse bytes_cleanser(bytes.data(), bytes.size());
  std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
  write_u32(bytes.data() + 8, kVersion);
  write_u32(bytes.data() + 12, static_cast<std::uint32_t>(count));
  std::size_t length = kHeaderBytes;
  for (std::size_t index = 0; index < count; ++index) {
    const auto origin = records[index].origin.view();
    write_u16(bytes.data() + length, static_cast<std::uint16_t>(origin.size()));
    write_u16(bytes.data() + length + 2, 0);
    length += 4;
    std::memcpy(bytes.data() + length, origin.data(), origin.size());
    length += origin.size();
    std::memcpy(bytes.data() + length, records[index].digest.data(), kDigestBytes);
    length += kDigestBytes;
  }

  const PathParts parts = split_path({path_.data(), path_length_});
  ScopedFd directory;
  PairingStoreError error = open_secure_directory(parts.directory, directory, false);
  if (error != PairingStoreError::None) return {.error = error};
  std::array<char, kMaximumPairingStorePathBytes> filename{};
  std::memcpy(filename.data(), parts.filename.data(), parts.filename.size());

  std::array<unsigned char, 8> random{};
  ScopedCleanse random_cleanser(random.data(), random.size());
  ERR_clear_error();
  if (RAND_priv_bytes(random.data(), static_cast<int>(random.size())) != 1) {
    ERR_clear_error();
    return {.error = PairingStoreError::RandomFailure};
  }
  ERR_clear_error();
  constexpr char hex[] = "0123456789abcdef";
  std::array<char, kMaximumPairingStorePathBytes> temporary{};
  const std::string_view suffix = ".tmp-";
  if (parts.filename.size() + suffix.size() + random.size() * 2 >= temporary.size()) {
    return {.error = PairingStoreError::InvalidPath};
  }
  std::size_t temp_length = 0;
  std::memcpy(temporary.data(), parts.filename.data(), parts.filename.size());
  temp_length += parts.filename.size();
  std::memcpy(temporary.data() + temp_length, suffix.data(), suffix.size());
  temp_length += suffix.size();
  for (const unsigned char byte : random) {
    temporary[temp_length++] = hex[byte >> 4U];
    temporary[temp_length++] = hex[byte & 0x0fU];
  }

  ScopedFd file(::openat(directory.get(), temporary.data(),
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (file.get() < 0) {
    return {.error = PairingStoreError::Io};
  }
  struct stat temporary_status {};
  bool ready = ::fchmod(file.get(), 0600) == 0 && strip_extended_acl(file.get()) &&
               ::fstat(file.get(), &temporary_status) == 0 &&
               S_ISREG(temporary_status.st_mode) && temporary_status.st_uid == ::geteuid() &&
               (temporary_status.st_mode & 0777) == 0600 && temporary_status.st_nlink == 1 &&
               full_write(file.get(), bytes.data(), length) && ::fsync(file.get()) == 0;
  if (!ready) {
    file.close();
    ::unlinkat(directory.get(), temporary.data(), 0);
    return {.error = PairingStoreError::Io};
  }
  file.close();
  if (fail_point_ == PairingStoreFailPoint::BeforeRename) {
    ::unlinkat(directory.get(), temporary.data(), 0);
    return {.error = PairingStoreError::Io};
  }
  if (gate != nullptr) {
    if (commit_hook_ != nullptr)
      commit_hook_->before_commit();
    if (!gate->try_begin_commit()) {
      ::unlinkat(directory.get(), temporary.data(), 0);
      return {.error = PairingStoreError::Canceled};
    }
  }
  if (::renameat(directory.get(), temporary.data(), directory.get(), filename.data()) != 0) {
    ::unlinkat(directory.get(), temporary.data(), 0);
    return {.error = PairingStoreError::Io};
  }
  if (fail_point_ == PairingStoreFailPoint::AfterRenameBeforeDirectorySync ||
      ::fsync(directory.get()) != 0) {
    return {.error = PairingStoreError::Io,
            .commit = PairingCommitState::CommittedDurabilityUncertain};
  }
  return {.error = PairingStoreError::None,
          .commit = PairingCommitState::CommittedDurable};
}

PairingIssueResult PairingStore::issue(const NormalizedOrigin& origin) noexcept {
  PairingCommitGate gate;
  return issue(origin, gate);
}

PairingIssueResult PairingStore::issue(const NormalizedOrigin& origin,
                                       PairingCommitGate& gate) noexcept {
  PairingIssueResult result{};
  if (origin.empty()) {
    result.error = PairingStoreError::Corrupt;
    return result;
  }
  ScopedFd lock;
  result.error = acquire_store_lock({path_.data(), path_length_}, lock);
  if (result.error != PairingStoreError::None) return result;
  result.error = reload();
  if (result.error != PairingStoreError::None) return result;
  std::size_t slot = record_count_;
  for (std::size_t index = 0; index < record_count_; ++index) {
    if (same_origin(records_[index].origin, origin)) {
      slot = index;
      result.replaced = true;
      break;
    }
  }
  if (slot == record_count_ && record_count_ == records_.size()) {
    result.error = PairingStoreError::Capacity;
    return result;
  }

  std::array<unsigned char, kPairingTokenBytes> raw{};
  ScopedCleanse raw_cleanser(raw.data(), raw.size());
  ERR_clear_error();
  if (RAND_priv_bytes(raw.data(), static_cast<int>(raw.size())) != 1) {
    ERR_clear_error();
    result.error = PairingStoreError::RandomFailure;
    return result;
  }
  ERR_clear_error();
  std::array<unsigned char, kDigestBytes> digest{};
  ScopedCleanse digest_cleanser(digest.data(), digest.size());
  if (!sha256(raw, digest)) {
    result.error = PairingStoreError::RandomFailure;
    return result;
  }
  constexpr char hex[] = "0123456789abcdef";
  for (std::size_t index = 0; index < raw.size(); ++index) {
    result.token.bytes_[index * 2] = hex[raw[index] >> 4U];
    result.token.bytes_[index * 2 + 1] = hex[raw[index] & 0x0fU];
  }
  result.token.length_ = result.token.bytes_.size();

  auto candidate = records_;
  ScopedCleanse candidate_cleanser(candidate.data(), sizeof(candidate));
  candidate[slot].origin = origin;
  candidate[slot].digest = digest;
  const std::size_t candidate_count = slot == record_count_ ? record_count_ + 1 : record_count_;
  const PersistResult persistence = persist(candidate, candidate_count, &gate);
  result.error = persistence.error;
  result.commit = persistence.commit;
  if (result.commit == PairingCommitState::NotCommitted) {
    result.token.clear();
    return result;
  }
  cleanse(records_.data(), sizeof(records_));
  records_ = candidate;
  record_count_ = candidate_count;
  return result;
}

PairingAuthenticationResult PairingStore::authenticate(
    const NormalizedOrigin& origin, std::string_view token) noexcept {
  PairingAuthenticationResult result{};
  std::array<unsigned char, kPairingTokenBytes> raw{};
  ScopedCleanse raw_cleanser(raw.data(), raw.size());
  if (!decode_token(token, raw)) {
    result.error = PairingStoreError::InvalidToken;
    return result;
  }
  ScopedFd lock;
  result.error = acquire_store_lock({path_.data(), path_length_}, lock);
  if (result.error != PairingStoreError::None) return result;
  result.error = reload();
  if (result.error != PairingStoreError::None) {
    return result;
  }
  std::array<unsigned char, kDigestBytes> presented{};
  ScopedCleanse presented_cleanser(presented.data(), presented.size());
  if (!sha256(raw, presented)) {
    result.error = PairingStoreError::RandomFailure;
    return result;
  }
  std::array<unsigned char, kDigestBytes> expected{};
  ScopedCleanse expected_cleanser(expected.data(), expected.size());
  bool found = false;
  for (std::size_t index = 0; index < record_count_; ++index) {
    if (same_origin(records_[index].origin, origin)) {
      expected = records_[index].digest;
      found = true;
      break;
    }
  }
  result.authenticated = CRYPTO_memcmp(presented.data(), expected.data(), expected.size()) == 0;
  if (!found) result.authenticated = false;
  return result;
}

PairingRevocationResult PairingStore::revoke(const NormalizedOrigin& origin) noexcept {
  PairingRevocationResult result{};
  ScopedFd lock;
  result.error = acquire_store_lock({path_.data(), path_length_}, lock);
  if (result.error != PairingStoreError::None) return result;
  result.error = reload();
  if (result.error != PairingStoreError::None) return result;
  std::size_t slot = record_count_;
  for (std::size_t index = 0; index < record_count_; ++index) {
    if (same_origin(records_[index].origin, origin)) {
      slot = index;
      break;
    }
  }
  if (slot == record_count_) {
    result.error = PairingStoreError::None;
    return result;
  }
  auto candidate = records_;
  ScopedCleanse candidate_cleanser(candidate.data(), sizeof(candidate));
  for (std::size_t index = slot + 1; index < record_count_; ++index) {
    candidate[index - 1] = candidate[index];
  }
  cleanse(&candidate[record_count_ - 1], sizeof(Record));
  const PersistResult persistence = persist(candidate, record_count_ - 1);
  result.error = persistence.error;
  result.commit = persistence.commit;
  if (result.commit == PairingCommitState::NotCommitted) {
    return result;
  }
  cleanse(records_.data(), sizeof(records_));
  records_ = candidate;
  --record_count_;
  result.revoked = true;
  return result;
}

PairingListResult PairingStore::list(std::span<NormalizedOrigin> output) noexcept {
  PairingListResult result{};
  ScopedFd lock;
  result.error = acquire_store_lock({path_.data(), path_length_}, lock);
  if (result.error != PairingStoreError::None) return result;
  result.error = reload();
  if (result.error != PairingStoreError::None) return result;
  if (output.size() < record_count_) {
    result.error = PairingStoreError::Capacity;
    return result;
  }
  for (std::size_t index = 0; index < record_count_; ++index) {
    output[index] = records_[index].origin;
  }
  result.count = record_count_;
  return result;
}

PairingStoreError default_pairing_store_path(std::span<char> output,
                                             std::size_t& length) noexcept {
  length = 0;
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] != '/') return PairingStoreError::InvalidPath;
#if defined(__APPLE__)
  constexpr std::string_view suffix =
      "/Library/Application Support/Noisefactor Sync/pairings.v1";
#else
  constexpr std::string_view suffix = "/.config/noisefactor-sync/pairings.v1";
#endif
  const std::string_view prefix(home);
  if (prefix.size() + suffix.size() >= output.size() ||
      prefix.size() + suffix.size() >= kMaximumPairingStorePathBytes) {
    return PairingStoreError::InvalidPath;
  }
  std::memcpy(output.data(), prefix.data(), prefix.size());
  std::memcpy(output.data() + prefix.size(), suffix.data(), suffix.size());
  length = prefix.size() + suffix.size();
  output[length] = '\0';
  if (!path_is_bounded_absolute({output.data(), length})) {
    cleanse(output.data(), length);
    length = 0;
    return PairingStoreError::InvalidPath;
  }
  return PairingStoreError::None;
}

}  // namespace noisefactor::sync
