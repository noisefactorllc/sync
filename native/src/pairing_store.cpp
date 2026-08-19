#include <sync/pairing_store.hpp>

#include "pairing_store_fs.hpp"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

// This file holds every platform-neutral piece of PairingStore: record
// parsing, versioning, capacity limits, token/digest handling, commit-gate
// interaction, and cleansing. The filesystem primitives that give it its
// actual guarantees (owner-only access, refusal to follow a
// symlink/reparse point, durable atomic commit) live behind the seam
// declared in pairing_store_fs.hpp -- see
// native/src/platform/posix/pairing_store_fs.cpp and
// native/src/platform/windows/pairing_store_fs.cpp. path_is_bounded_absolute
// and split_path below are the two narrow exceptions: they are pure path
// *syntax* (no filesystem calls at all), but that syntax genuinely differs
// between '/'-separated POSIX paths and drive-absolute '\'-separated
// Windows paths, so each carries its own small, self-contained branch
// rather than needing a seam function.

namespace noisefactor::sync {
namespace {

constexpr std::array<unsigned char, 8> kMagic = {'N', 'F', 'S', 'Y', 'N', 'C', 'P', 'R'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kDigestBytes = 32;
constexpr std::size_t kMaximumSerializedBytes =
    kHeaderBytes + kMaximumPairingOrigins * (4 + kMaximumOriginBytes + kDigestBytes);

void cleanse(void* data, std::size_t length) noexcept {
  if (length > 0) OPENSSL_cleanse(data, length);
}

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

#if defined(_WIN32)

// Device names are reserved on Windows regardless of case or a trailing
// extension -- CON, CON.txt, and con.anything all name the same reserved
// device -- so a path component matching one of these (by its stem, before
// any '.') must never be accepted as an ordinary directory/file name.
bool is_reserved_windows_device_name(std::string_view component) noexcept {
  std::string_view stem = component;
  const std::size_t dot = stem.find('.');
  if (dot != std::string_view::npos) stem = stem.substr(0, dot);
  auto equals_ignoring_case = [&](std::string_view name) noexcept {
    if (stem.size() != name.size()) return false;
    for (std::size_t index = 0; index < stem.size(); ++index) {
      char value = stem[index];
      if (value >= 'a' && value <= 'z') value = static_cast<char>(value - 'a' + 'A');
      if (value != name[index]) return false;
    }
    return true;
  };
  constexpr std::array<std::string_view, 22> kReserved = {
      "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
      "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
      "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
  };
  for (const auto& name : kReserved) {
    if (equals_ignoring_case(name)) return true;
  }
  return false;
}

// Windows drive-absolute form: "C:\...". Rejects relative paths, UNC paths
// ("\\server\share\..."), and the "\\?\" extended-length prefix -- all of
// those fail the drive-letter check at index 0 already, since they start
// with a second backslash there instead.
bool path_is_bounded_absolute(std::string_view path) noexcept {
  if (path.size() < 4 || path.size() >= kMaximumPairingStorePathBytes) return false;
  const char drive = path.front();
  if (!((drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z'))) return false;
  if (path[1] != ':' || path[2] != '\\') return false;
  if (path.back() == '\\') return false;
  for (const unsigned char byte : path) {
    if (byte < 0x20U || byte == 0x7fU || byte > 0x7eU) return false;
    // Reserved/ambiguous on Windows even though they sit inside the
    // otherwise-printable-ASCII range this store allows: none of these can
    // appear in a real filename component.
    if (byte == '<' || byte == '>' || byte == '"' || byte == '|' || byte == '?' || byte == '*') {
      return false;
    }
  }
  // A second ':' anywhere past the drive letter denotes an NTFS alternate
  // data stream selector, not a path character.
  if (path.substr(2).find(':') != std::string_view::npos) return false;
  // Forward slash is never accepted as a separator: CreateFileW happens to
  // treat it interchangeably with backslash, which would make "bounded
  // absolute" ambiguous about how many components a path actually has.
  if (path.find('/') != std::string_view::npos) return false;

  std::size_t start = 3;
  while (start < path.size()) {
    const std::size_t backslash = path.find('\\', start);
    const std::size_t end = backslash == std::string_view::npos ? path.size() : backslash;
    const auto component = path.substr(start, end - start);
    if (component.empty() || component.size() > fs::kMaximumPathComponentBytes ||
        component == "." || component == "..") {
      return false;
    }
    // Windows silently strips a trailing '.' or ' ' from a component when
    // resolving it, which would let two different validated strings name
    // the same on-disk object -- reject that ambiguity outright rather
    // than let it anywhere near a security check.
    if (component.back() == '.' || component.back() == ' ') return false;
    if (is_reserved_windows_device_name(component)) return false;
    if (backslash == std::string_view::npos) break;
    start = backslash + 1;
  }
  return true;
}

#else

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
    if (component.empty() || component.size() > fs::kMaximumPathComponentBytes ||
        component == "." || component == "..") {
      return false;
    }
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return true;
}

#endif

struct PathParts {
  std::string_view directory;
  std::string_view filename;
};

PathParts split_path(std::string_view path) noexcept {
#if defined(_WIN32)
  const std::size_t backslash = path.rfind('\\');
  // backslash == 2 means the only separator found is the drive root's own
  // ("C:\name"), so the directory is the 3-character root "C:\" itself.
  // A plain substr(0, backslash) would instead yield "C:" (2 characters),
  // losing the trailing separator that open_secure_directory requires
  // (directory[2] == '\\') -- the same special case the POSIX branch below
  // handles for a bare leading '/'.
  return {.directory = backslash == 2 ? path.substr(0, 3) : path.substr(0, backslash),
          .filename = path.substr(backslash + 1)};
#else
  const std::size_t slash = path.rfind('/');
  return {.directory = slash == 0 ? std::string_view("/") : path.substr(0, slash),
          .filename = path.substr(slash + 1)};
#endif
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
      parts.filename.size() + kTemporarySuffixBytes > fs::kMaximumPathComponentBytes) {
    clear();
    return PairingStoreError::InvalidPath;
  }
  fs::ScopedHandle directory;
  const PairingStoreError directory_error =
      fs::open_secure_directory(parts.directory, directory, true);
  if (directory_error != PairingStoreError::None) {
    clear();
    return directory_error;
  }
  opened_ = true;
  fs::ScopedHandle lock;
  const PairingStoreError lock_error = fs::acquire_store_lock(parts.directory, lock);
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

  std::array<unsigned char, kMaximumSerializedBytes> bytes{};
  ScopedCleanse bytes_cleanser(bytes.data(), bytes.size());
  const fs::LoadResult load = fs::load_secure_regular_file(parts.directory, parts.filename, bytes);
  if (load.error != PairingStoreError::None) return load.error;
  if (!load.exists) {
    cleanse(records_.data(), sizeof(records_));
    record_count_ = 0;
    return PairingStoreError::None;
  }
  const std::size_t size = load.size;
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
  const std::string_view temporary_name(temporary.data(), temp_length);

  const PairingStoreError write_error = fs::write_new_secure_regular_file(
      parts.directory, temporary_name, std::span<const unsigned char>(bytes.data(), length));
  if (write_error != PairingStoreError::None) return {.error = write_error};

  if (fail_point_ == PairingStoreFailPoint::BeforeRename) {
    fs::remove_file(parts.directory, temporary_name);
    return {.error = PairingStoreError::Io};
  }
  if (gate != nullptr) {
    if (commit_hook_ != nullptr)
      commit_hook_->before_commit();
    if (!gate->try_begin_commit()) {
      fs::remove_file(parts.directory, temporary_name);
      return {.error = PairingStoreError::Canceled};
    }
  }
  const bool simulate_directory_sync_failure =
      fail_point_ == PairingStoreFailPoint::AfterRenameBeforeDirectorySync;
  const fs::CommitResult commit = fs::rename_into_place(parts.directory, temporary_name,
                                                         parts.filename,
                                                         simulate_directory_sync_failure);
  return {.error = commit.error, .commit = commit.commit};
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
  const PathParts parts = split_path({path_.data(), path_length_});
  fs::ScopedHandle lock;
  result.error = fs::acquire_store_lock(parts.directory, lock);
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
  const PathParts parts = split_path({path_.data(), path_length_});
  fs::ScopedHandle lock;
  result.error = fs::acquire_store_lock(parts.directory, lock);
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
  const PathParts parts = split_path({path_.data(), path_length_});
  fs::ScopedHandle lock;
  result.error = fs::acquire_store_lock(parts.directory, lock);
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
  const PathParts parts = split_path({path_.data(), path_length_});
  fs::ScopedHandle lock;
  result.error = fs::acquire_store_lock(parts.directory, lock);
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
#if defined(_WIN32)
  // Plain (ANSI) getenv, not a Win32 wide-string API, is deliberate here:
  // path_is_bounded_absolute only ever accepts printable ASCII, so a
  // %LOCALAPPDATA% value containing non-ASCII bytes gets rejected below
  // regardless of how faithfully it was decoded -- the same limitation the
  // POSIX branch already has for a non-ASCII $HOME. Staying on getenv keeps
  // this whole file free of <windows.h>; SetEnvironmentVariable still
  // reaches it because the CRT's environment block is the process
  // environment block.
  const char* local_app_data = std::getenv("LOCALAPPDATA");
  if (local_app_data == nullptr) return PairingStoreError::InvalidPath;
  const std::string_view prefix(local_app_data);
  if (prefix.size() < 3 ||
      !((prefix[0] >= 'A' && prefix[0] <= 'Z') || (prefix[0] >= 'a' && prefix[0] <= 'z')) ||
      prefix[1] != ':' || prefix[2] != '\\') {
    return PairingStoreError::InvalidPath;
  }
  constexpr std::string_view suffix = "\\Noisefactor Sync\\pairings.v1";
#else
#if defined(__APPLE__)
  constexpr std::string_view suffix =
      "/Library/Application Support/Noisefactor Sync/pairings.v1";
#else
  constexpr std::string_view suffix = "/.config/noisefactor-sync/pairings.v1";
#endif
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] != '/') return PairingStoreError::InvalidPath;
  const std::string_view prefix(home);
#endif
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
