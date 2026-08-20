#include "test_harness.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#include <winioctl.h>  // FSCTL_SET_REPARSE_POINT
#include <cwchar>  // _snwprintf_s
#else
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <membership.h>
#include <sys/acl.h>
#endif

#include <sync/origin.hpp>
#include <sync/pairing_store.hpp>

#include "../src/pairing_store_fs.hpp"

namespace {

using noisefactor::sync::NormalizedOrigin;
using noisefactor::sync::PairingCommitState;
using noisefactor::sync::PairingCommitGate;
using noisefactor::sync::PairingStore;
using noisefactor::sync::PairingStoreCommitHook;
using noisefactor::sync::PairingStoreError;
using noisefactor::sync::PairingStoreFailPoint;
using noisefactor::sync::PairingStoreOptions;
using noisefactor::sync::normalize_origin;

class TempDirectory {
 public:
  TempDirectory() {
#if defined(_WIN32)
    // Unlike macOS's /tmp -> /private/tmp, GetTempPathW's result on a
    // normal Windows install/CI image is a real directory, not a reparse
    // point, so it does not need the same "use the resolved path, not the
    // symlinked convenience path" workaround the POSIX branch below uses.
    wchar_t temp_path[MAX_PATH + 1]{};
    const DWORD temp_path_length = ::GetTempPathW(MAX_PATH, temp_path);
    SYNC_REQUIRE(temp_path_length > 0 && temp_path_length < MAX_PATH);
    std::mt19937_64 engine(std::random_device{}());
    bool created = false;
    for (int attempt = 0; attempt < 100 && !created; ++attempt) {
      wchar_t name[64]{};
      ::_snwprintf_s(name, _TRUNCATE, L"sync-pairing-test-%016llx",
                     static_cast<unsigned long long>(engine()));
      std::filesystem::path candidate = std::filesystem::path(temp_path) / name;
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = candidate;
        created = true;
      }
    }
    SYNC_REQUIRE(created);
#else
    std::array<char, 256> pattern{};
    const std::string seed = "/private/tmp/sync-pairing-test-XXXXXX";
    std::copy(seed.begin(), seed.end(), pattern.begin());
    char* made = ::mkdtemp(pattern.data());
    SYNC_REQUIRE(made != nullptr);
    path_ = made;
#endif
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

class HoldingCommitHook final : public PairingStoreCommitHook {
 public:
  void before_commit() noexcept override {
    std::unique_lock lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [&] { return released_; });
  }

  bool wait_until_entered() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(2),
                               [&] { return entered_; });
  }

  void release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    condition_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool entered_ = false;
  bool released_ = false;
};

NormalizedOrigin origin(std::string_view value) {
  const auto result = normalize_origin(value);
  SYNC_REQUIRE(result.ok());
  return result.origin;
}

std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

#if defined(_WIN32)

// Reimplements PairingStore's owner-only DACL check independently of
// native/src/platform/windows/pairing_store_fs.cpp's verify_owner_only_
// security -- deliberately not shared with it, so these tests exercise the
// production code's actual on-disk result rather than asserting via the
// same logic that produced it.
bool get_current_user_sid_for_test(std::vector<unsigned char>& buffer, PSID& sid) {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  DWORD needed = 0;
  ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
  bool ok = needed > 0;
  if (ok) {
    buffer.resize(needed);
    ok = ::GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed) != 0;
  }
  ::CloseHandle(token);
  if (!ok) return false;
  sid = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
  return true;
}

bool file_dacl_grants_only_current_user(const std::filesystem::path& path) {
  std::vector<unsigned char> sid_buffer;
  PSID owner_sid = nullptr;
  if (!get_current_user_sid_for_test(sid_buffer, owner_sid)) return false;

  PSID file_owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD status = ::GetNamedSecurityInfoW(
      const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &file_owner, nullptr, &dacl,
      nullptr, &descriptor);
  if (status != ERROR_SUCCESS || descriptor == nullptr) return false;
  struct Guard {
    PSECURITY_DESCRIPTOR value;
    ~Guard() { if (value != nullptr) ::LocalFree(value); }
  } guard{descriptor};
  if (file_owner == nullptr || !::EqualSid(file_owner, owner_sid)) return false;

  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  if (!::GetSecurityDescriptorControl(descriptor, &control, &revision)) return false;
  if ((control & SE_DACL_PROTECTED) == 0) return false;
  if (dacl == nullptr) return false;

  ACL_SIZE_INFORMATION size_info{};
  if (!::GetAclInformation(dacl, &size_info, sizeof(size_info), AclSizeInformation)) return false;
  if (size_info.AceCount != 1) return false;
  LPVOID ace = nullptr;
  if (!::GetAce(dacl, 0, &ace)) return false;
  const auto* header = static_cast<ACE_HEADER*>(ace);
  if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
  const auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
  if (allowed->Mask != FILE_ALL_ACCESS) return false;
  const PSID ace_sid = const_cast<PSID>(static_cast<const void*>(&allowed->SidStart));
  return ::EqualSid(ace_sid, owner_sid) != 0;
}

// Sets `path`'s DACL to exactly what PairingStore itself would create:
// this is the Windows analogue of `::chmod(path, 0600)` used below to make
// a raw-bytes test fixture pass PairingStore's security checks so the
// content-level (corrupt/version) checks under test are actually reached.
void secure_file_for_test(const std::filesystem::path& path) {
  std::vector<unsigned char> sid_buffer;
  PSID sid = nullptr;
  SYNC_REQUIRE(get_current_user_sid_for_test(sid_buffer, sid));
  std::array<unsigned char, 256> acl_buffer{};
  auto* acl = reinterpret_cast<PACL>(acl_buffer.data());
  SYNC_REQUIRE(::InitializeAcl(acl, static_cast<DWORD>(acl_buffer.size()), ACL_REVISION));
  SYNC_REQUIRE(::AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, sid));
  const DWORD result = ::SetNamedSecurityInfoW(
      const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      sid, nullptr, acl, nullptr);
  SYNC_REQUIRE(result == ERROR_SUCCESS);
}

// Deliberately widens `path`'s DACL to also grant Everyone, and leaves it
// unprotected against inheritance -- the Windows analogue of
// `::chmod(path, 0644)` / `::chmod(path, 0755)` below: both make an object
// more permissive than PairingStore will accept, so it must be refused.
void widen_permissions_for_test(const std::filesystem::path& path) {
  SID_IDENTIFIER_AUTHORITY world_authority = SECURITY_WORLD_SID_AUTHORITY;
  PSID everyone_sid = nullptr;
  SYNC_REQUIRE(::AllocateAndInitializeSid(&world_authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0,
                                          0, &everyone_sid) != 0);
  std::vector<unsigned char> sid_buffer;
  PSID owner_sid = nullptr;
  SYNC_REQUIRE(get_current_user_sid_for_test(sid_buffer, owner_sid));

  std::array<unsigned char, 256> acl_buffer{};
  auto* acl = reinterpret_cast<PACL>(acl_buffer.data());
  SYNC_REQUIRE(::InitializeAcl(acl, static_cast<DWORD>(acl_buffer.size()), ACL_REVISION));
  SYNC_REQUIRE(::AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, owner_sid));
  SYNC_REQUIRE(::AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_READ, everyone_sid));
  const DWORD result = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                               DACL_SECURITY_INFORMATION, nullptr, nullptr, acl,
                                               nullptr);
  ::FreeSid(everyone_sid);
  SYNC_REQUIRE(result == ERROR_SUCCESS);
}

// Best-effort symlink creation: on a CI image without Developer Mode or
// admin rights, CreateSymbolicLinkW fails with ERROR_PRIVILEGE_NOT_HELD.
// Callers must treat a false return as "skip this assertion", not a test
// failure. For DIRECTORY reparse points use try_create_junction below
// instead: a mount point needs no privilege at all, so it actually runs
// everywhere and is the better test of the ancestor-refusal guarantee.
bool try_create_symlink(const std::filesystem::path& link, const std::filesystem::path& target,
                        bool directory) {
  DWORD flags = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
#if defined(SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)
  flags |= SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
#endif
  return ::CreateSymbolicLinkW(link.c_str(), target.c_str(), flags) != 0;
}

// Creates a directory junction (an unprivileged reparse point) at `link`
// pointing at `target`. Unlike CreateSymbolicLinkW this needs no Developer
// Mode and no admin rights, so the "refuse a reparse point in the path"
// guarantee gets tested on every machine rather than skipped on most.
//
// REPARSE_DATA_BUFFER is not declared in the user-mode SDK headers, so the
// mount-point layout is spelled out here: the tag, the byte counts, then
// the substitute name ("\??\C:\target", what the filesystem resolves)
// followed by the print name (what the shell displays), both inside one
// PathBuffer.
bool try_create_junction(const std::filesystem::path& link,
                         const std::filesystem::path& target) {
  std::error_code error;
  std::filesystem::create_directory(link, error);
  if (error) return false;

  const std::wstring substitute = L"\\??\\" + target.wstring();
  const std::wstring print = target.wstring();
  const std::size_t substitute_bytes = substitute.size() * sizeof(wchar_t);
  const std::size_t print_bytes = print.size() * sizeof(wchar_t);

  struct MountPointBuffer {
    DWORD ReparseTag;
    WORD ReparseDataLength;
    WORD Reserved;
    WORD SubstituteNameOffset;
    WORD SubstituteNameLength;
    WORD PrintNameOffset;
    WORD PrintNameLength;
    wchar_t PathBuffer[MAX_PATH * 4];
  };
  MountPointBuffer buffer{};
  // Two NUL terminators are included in the path buffer but not counted in
  // the name lengths, which is what the mount-point format expects.
  if (substitute_bytes + print_bytes + 2 * sizeof(wchar_t) >
      sizeof(buffer.PathBuffer)) {
    return false;
  }
  buffer.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
  buffer.SubstituteNameOffset = 0;
  buffer.SubstituteNameLength = static_cast<WORD>(substitute_bytes);
  buffer.PrintNameOffset = static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
  buffer.PrintNameLength = static_cast<WORD>(print_bytes);
  std::memcpy(buffer.PathBuffer, substitute.c_str(),
              substitute_bytes + sizeof(wchar_t));
  std::memcpy(reinterpret_cast<unsigned char*>(buffer.PathBuffer) +
                  buffer.PrintNameOffset,
              print.c_str(), print_bytes + sizeof(wchar_t));
  // Everything from SubstituteNameOffset onward, i.e. the header fields
  // after ReparseDataLength plus both names and their terminators.
  const std::size_t data_length = 4 * sizeof(WORD) + substitute_bytes +
                                  print_bytes + 2 * sizeof(wchar_t);
  buffer.ReparseDataLength = static_cast<WORD>(data_length);

  HANDLE handle = ::CreateFileW(
      link.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD returned = 0;
  const BOOL ok = ::DeviceIoControl(
      handle, FSCTL_SET_REPARSE_POINT, &buffer,
      static_cast<DWORD>(data_length + 2 * sizeof(DWORD)), nullptr, 0,
      &returned, nullptr);
  ::CloseHandle(handle);
  return ok != 0;
}

bool set_local_app_data_for_test(const std::string& value) {
  return ::_putenv_s("LOCALAPPDATA", value.c_str()) == 0;
}

std::string get_local_app_data_for_test(bool& had_previous) {
  char buffer[4096]{};
  const DWORD written = ::GetEnvironmentVariableA("LOCALAPPDATA", buffer, sizeof(buffer));
  had_previous = written > 0 && written < sizeof(buffer);
  return had_previous ? std::string(buffer) : std::string{};
}

void restore_local_app_data_for_test(bool had_previous, const std::string& saved) {
  // An empty value removes the variable in the Microsoft CRT's _putenv_s,
  // documented behaviour used here as the unsetenv() this platform lacks.
  SYNC_REQUIRE(::_putenv_s("LOCALAPPDATA", had_previous ? saved.c_str() : "") == 0);
}

#endif  // defined(_WIN32)

void write_bytes(const std::filesystem::path& path, std::span<const unsigned char> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  output.close();
#if defined(_WIN32)
  secure_file_for_test(path);
#else
  SYNC_REQUIRE(::chmod(path.c_str(), 0600) == 0);
#endif
}

bool all_lowercase_hex(std::string_view value) {
  if (value.size() != noisefactor::sync::kPairingTokenHexBytes) return false;
  for (const char c : value) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

#if defined(__APPLE__)
class ScopedAcl {
 public:
  explicit ScopedAcl(acl_t value) : value_(value) {}
  ~ScopedAcl() { if (value_ != nullptr) ::acl_free(value_); }
  acl_t get() const { return value_; }
 private:
  acl_t value_;
};

void add_extended_acl(int fd, bool inheritable = false) {
  ScopedAcl acl(::acl_init(1));
  SYNC_REQUIRE(acl.get() != nullptr);
  acl_entry_t entry = nullptr;
  acl_t mutable_acl = acl.get();
  SYNC_REQUIRE(::acl_create_entry(&mutable_acl, &entry) == 0);
  SYNC_REQUIRE(::acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0);
  uuid_t owner{};
  SYNC_REQUIRE(::mbr_uid_to_uuid(::geteuid(), owner) == 0);
  SYNC_REQUIRE(::acl_set_qualifier(entry, owner) == 0);
  acl_permset_t permissions = nullptr;
  SYNC_REQUIRE(::acl_get_permset(entry, &permissions) == 0);
  SYNC_REQUIRE(::acl_clear_perms(permissions) == 0);
  SYNC_REQUIRE(::acl_add_perm(permissions, ACL_READ_DATA) == 0);
  if (inheritable) {
    acl_flagset_t flags = nullptr;
    SYNC_REQUIRE(::acl_get_flagset_np(entry, &flags) == 0);
    SYNC_REQUIRE(::acl_add_flag_np(flags, ACL_ENTRY_FILE_INHERIT) == 0);
    SYNC_REQUIRE(::acl_add_flag_np(flags, ACL_ENTRY_DIRECTORY_INHERIT) == 0);
  }
  SYNC_REQUIRE(::acl_set_fd_np(fd, acl.get(), ACL_TYPE_EXTENDED) == 0);
}

bool has_extended_acl(int fd) {
  ScopedAcl acl(::acl_get_fd_np(fd, ACL_TYPE_EXTENDED));
  if (acl.get() == nullptr) return false;
  acl_entry_t entry = nullptr;
  return ::acl_get_entry(acl.get(), ACL_FIRST_ENTRY, &entry) == 0;
}
#endif

PairingStore open_store(const std::filesystem::path& path,
                        PairingStoreFailPoint fail = PairingStoreFailPoint::None,
                        PairingStoreCommitHook* commit_hook = nullptr) {
  PairingStore store;
  SYNC_REQUIRE(store.open({.path = path.string(),
                           .fail_point = fail,
                           .commit_hook = commit_hook}) ==
               PairingStoreError::None);
  return store;
}

}  // namespace

SYNC_TEST(pairing_commit_gate_has_one_atomic_cancel_or_commit_winner) {
  PairingCommitGate canceled;
  SYNC_REQUIRE(canceled.cancel());
  SYNC_REQUIRE(canceled.canceled());
  SYNC_REQUIRE(!canceled.try_begin_commit());
  SYNC_REQUIRE(!canceled.cancel());

  PairingCommitGate committed;
  SYNC_REQUIRE(committed.try_begin_commit());
  SYNC_REQUIRE(!committed.canceled());
  SYNC_REQUIRE(!committed.cancel());
  SYNC_REQUIRE(!committed.try_begin_commit());
}

SYNC_TEST(pairing_store_treats_missing_file_as_empty_and_rejects_empty_or_corrupt_files) {
  TempDirectory temporary;
  const auto path = temporary.path() / "state" / "pairings.bin";
  auto store = open_store(path);
  std::array<NormalizedOrigin, 64> listed{};
  const auto empty = store.list(listed);
  SYNC_REQUIRE(empty.error == PairingStoreError::None);
  SYNC_REQUIRE(empty.count == 0);

  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary).close();
#if defined(_WIN32)
  secure_file_for_test(path);
#else
  SYNC_REQUIRE(::chmod(path.c_str(), 0600) == 0);
#endif
  PairingStore corrupt;
  SYNC_REQUIRE(corrupt.open({.path = path.string()}) == PairingStoreError::Corrupt);
}

SYNC_TEST(pairing_store_mints_lowercase_256_bit_tokens_and_persists_hashes_only) {
  TempDirectory temporary;
  const auto path = temporary.path() / "state" / "pairings.bin";
  auto store = open_store(path);
  const auto deck = origin("https://deck.example");
  auto issued = store.issue(deck);
  SYNC_REQUIRE(issued.error == PairingStoreError::None);
  SYNC_REQUIRE(issued.commit == PairingCommitState::CommittedDurable);
  SYNC_REQUIRE(!issued.replaced);
  SYNC_REQUIRE(all_lowercase_hex(issued.token.view()));

  const auto bytes = read_bytes(path);
  const std::string persisted(bytes.begin(), bytes.end());
  SYNC_REQUIRE(persisted.find(std::string(issued.token.view())) == std::string::npos);
  SYNC_REQUIRE(persisted.find("https://deck.example") != std::string::npos);

  const auto auth = store.authenticate(deck, issued.token.view());
  SYNC_REQUIRE(auth.error == PairingStoreError::None);
  SYNC_REQUIRE(auth.authenticated);
  SYNC_REQUIRE(!store.authenticate(deck, std::string(64, '0')).authenticated);
  SYNC_REQUIRE(store.authenticate(deck, "short").error == PairingStoreError::InvalidToken);
}

SYNC_TEST(pairing_store_replaces_duplicate_origin_and_rotation_invalidates_old_token) {
  TempDirectory temporary;
  auto store = open_store(temporary.path() / "state" / "pairings.bin");
  const auto deck = origin("https://deck.example");
  const auto first = store.issue(deck);
  const auto second = store.issue(deck);
  SYNC_REQUIRE(first.error == PairingStoreError::None);
  SYNC_REQUIRE(second.error == PairingStoreError::None);
  SYNC_REQUIRE(second.replaced);
  SYNC_REQUIRE(first.token.view() != second.token.view());
  SYNC_REQUIRE(!store.authenticate(deck, first.token.view()).authenticated);
  SYNC_REQUIRE(store.authenticate(deck, second.token.view()).authenticated);
  std::array<NormalizedOrigin, 64> listed{};
  SYNC_REQUIRE(store.list(listed).count == 1);
}

SYNC_TEST(pairing_store_enforces_64_origin_bound_without_replacing_existing_records) {
  TempDirectory temporary;
  auto store = open_store(temporary.path() / "state" / "pairings.bin");
  for (std::size_t index = 0; index < noisefactor::sync::kMaximumPairingOrigins; ++index) {
    const auto current = origin("https://host" + std::to_string(index) + ".example");
    SYNC_REQUIRE(store.issue(current).error == PairingStoreError::None);
  }
  const auto overflow = store.issue(origin("https://overflow.example"));
  SYNC_REQUIRE(overflow.error == PairingStoreError::Capacity);
  SYNC_REQUIRE(overflow.token.view().empty());

  const auto replacement = store.issue(origin("https://host0.example"));
  SYNC_REQUIRE(replacement.error == PairingStoreError::None);
  SYNC_REQUIRE(replacement.replaced);
}

SYNC_TEST(pairing_store_strictly_rejects_unknown_versions_truncation_trailing_and_duplicate_records) {
  TempDirectory temporary;
  const auto path = temporary.path() / "state" / "pairings.bin";
  auto store = open_store(path);
  SYNC_REQUIRE(store.issue(origin("https://deck.example")).error == PairingStoreError::None);
  const auto valid = read_bytes(path);

  auto unknown = valid;
  unknown[8] = 2;
  write_bytes(path, unknown);
  PairingStore check_unknown;
  SYNC_REQUIRE(check_unknown.open({.path = path.string()}) == PairingStoreError::UnknownVersion);

  auto truncated = valid;
  truncated.pop_back();
  write_bytes(path, truncated);
  PairingStore check_truncated;
  SYNC_REQUIRE(check_truncated.open({.path = path.string()}) == PairingStoreError::Corrupt);

  auto trailing = valid;
  trailing.push_back(0);
  write_bytes(path, trailing);
  PairingStore check_trailing;
  SYNC_REQUIRE(check_trailing.open({.path = path.string()}) == PairingStoreError::Corrupt);

  auto duplicate = valid;
  duplicate[12] = 2;
  duplicate.insert(duplicate.end(), valid.begin() + 16, valid.end());
  write_bytes(path, duplicate);
  PairingStore check_duplicate;
  SYNC_REQUIRE(check_duplicate.open({.path = path.string()}) == PairingStoreError::Corrupt);
}

#if !defined(_WIN32)
SYNC_TEST(pairing_store_requires_owner_only_directory_and_regular_store_file) {
  TempDirectory temporary;
  const auto state = temporary.path() / "state";
  const auto path = state / "pairings.bin";
  auto store = open_store(path);
  SYNC_REQUIRE(store.issue(origin("https://deck.example")).error == PairingStoreError::None);

  struct stat directory_status {};
  struct stat file_status {};
  struct stat lock_status {};
  SYNC_REQUIRE(::stat(state.c_str(), &directory_status) == 0);
  SYNC_REQUIRE(::stat(path.c_str(), &file_status) == 0);
  SYNC_REQUIRE(::stat((state / ".pairings.lock").c_str(), &lock_status) == 0);
  SYNC_REQUIRE((directory_status.st_mode & 0777) == 0700);
  SYNC_REQUIRE((file_status.st_mode & 0777) == 0600);
  SYNC_REQUIRE((lock_status.st_mode & 0777) == 0600);
  SYNC_REQUIRE(directory_status.st_uid == ::geteuid());
  SYNC_REQUIRE(file_status.st_uid == ::geteuid());
  SYNC_REQUIRE(lock_status.st_uid == ::geteuid());

  SYNC_REQUIRE(::chmod(path.c_str(), 0644) == 0);
  PairingStore broad_file;
  SYNC_REQUIRE(broad_file.open({.path = path.string()}) == PairingStoreError::FileSecurity);
  SYNC_REQUIRE(::chmod(path.c_str(), 0600) == 0);
  SYNC_REQUIRE(::chmod(state.c_str(), 0755) == 0);
  PairingStore broad_directory;
  SYNC_REQUIRE(broad_directory.open({.path = path.string()}) == PairingStoreError::DirectorySecurity);
}
#else
SYNC_TEST(pairing_store_requires_owner_only_directory_and_regular_store_file) {
  TempDirectory temporary;
  const auto state = temporary.path() / "state";
  const auto path = state / "pairings.bin";
  auto store = open_store(path);
  SYNC_REQUIRE(store.issue(origin("https://deck.example")).error == PairingStoreError::None);

  SYNC_REQUIRE(file_dacl_grants_only_current_user(state));
  SYNC_REQUIRE(file_dacl_grants_only_current_user(path));
  SYNC_REQUIRE(file_dacl_grants_only_current_user(state / ".pairings.lock"));

  widen_permissions_for_test(path);
  PairingStore broad_file;
  SYNC_REQUIRE(broad_file.open({.path = path.string()}) == PairingStoreError::FileSecurity);
  secure_file_for_test(path);

  widen_permissions_for_test(state);
  PairingStore broad_directory;
  SYNC_REQUIRE(broad_directory.open({.path = path.string()}) ==
               PairingStoreError::DirectorySecurity);
}
#endif

SYNC_TEST(pairing_store_rejects_symlinked_directories_files_and_parent_traversal) {
  TempDirectory temporary;
  PairingStore traversal;
  SYNC_REQUIRE(traversal.open({.path = (temporary.path() / ".." / "escape.bin").string()}) ==
               PairingStoreError::InvalidPath);

#if !defined(_WIN32)
  const auto real = temporary.path() / "real";
  std::filesystem::create_directory(real);
  SYNC_REQUIRE(::chmod(real.c_str(), 0700) == 0);
  const auto linked = temporary.path() / "linked";
  SYNC_REQUIRE(::symlink(real.c_str(), linked.c_str()) == 0);
  PairingStore directory_link;
  SYNC_REQUIRE(directory_link.open({.path = (linked / "pairings.bin").string()}) ==
               PairingStoreError::DirectorySecurity);

  const auto target = real / "target";
  std::ofstream(target) << "target";
  SYNC_REQUIRE(::chmod(target.c_str(), 0600) == 0);
  const auto file_link = real / "pairings.bin";
  SYNC_REQUIRE(::symlink(target.c_str(), file_link.c_str()) == 0);
  PairingStore symlink;
  SYNC_REQUIRE(symlink.open({.path = file_link.string()}) == PairingStoreError::FileSecurity);
#else
  const auto real = temporary.path() / "real";
  std::filesystem::create_directory(real);
  const auto linked = temporary.path() / "linked";
  if (try_create_symlink(linked, real, true)) {
    PairingStore directory_link;
    SYNC_REQUIRE(directory_link.open({.path = (linked / "pairings.bin").string()}) ==
                 PairingStoreError::DirectorySecurity);
  }

  const auto target = real / "target";
  std::ofstream(target) << "target";
  secure_file_for_test(target);
  const auto file_link = real / "pairings.bin";
  if (try_create_symlink(file_link, target, false)) {
    PairingStore symlink_store;
    SYNC_REQUIRE(symlink_store.open({.path = file_link.string()}) ==
                 PairingStoreError::FileSecurity);
  }
#endif
}

#if defined(_WIN32)
// The symlink assertions above are skipped on any machine without Developer
// Mode, which is most CI images -- so on its own the reparse-point guarantee
// had no test that actually executed there. A junction needs no privilege,
// so this one always runs.
//
// The junction sits in the MIDDLE of the path, which is the case
// FILE_FLAG_OPEN_REPARSE_POINT alone does not cover: that flag only refuses
// to follow a reparse point in the final component, so refusing this one is
// entirely down to open_secure_directory walking and checking every ancestor.
SYNC_TEST(pairing_store_rejects_a_junctioned_ancestor_directory) {
  TempDirectory temporary;
  const auto decoy = temporary.path() / "decoy";
  std::filesystem::create_directory(decoy);
  const auto junction = temporary.path() / "middle";
  SYNC_REQUIRE(try_create_junction(junction, decoy));

  // Sanity: the junction really does resolve to the decoy, so a failure
  // below means the store refused it rather than that it was never a
  // reparse point at all.
  std::ofstream(decoy / "proof") << "proof";
  SYNC_REQUIRE(std::filesystem::exists(junction / "proof"));

  PairingStore through_junction;
  SYNC_REQUIRE(through_junction.open(
                   {.path = (junction / "state" / "pairings.bin").string()}) ==
               PairingStoreError::DirectorySecurity);

  // And nothing was written into the decoy the junction pointed at.
  SYNC_REQUIRE(!std::filesystem::exists(decoy / "state"));
}
#endif

SYNC_TEST(pairing_store_atomic_pre_rename_failure_preserves_previous_file_and_cleans_temp) {
  TempDirectory temporary;
  const auto state = temporary.path() / "state";
  const auto path = state / "pairings.bin";
  auto store = open_store(path);
  const auto deck = origin("https://deck.example");
  const auto first = store.issue(deck);
  const auto before = read_bytes(path);

  auto failing = open_store(path, PairingStoreFailPoint::BeforeRename);
  const auto failed = failing.issue(deck);
  SYNC_REQUIRE(failed.error == PairingStoreError::Io);
  SYNC_REQUIRE(failed.commit == PairingCommitState::NotCommitted);
  SYNC_REQUIRE(failed.token.view().empty());
  SYNC_REQUIRE(read_bytes(path) == before);
  for (const auto& entry : std::filesystem::directory_iterator(state)) {
    SYNC_REQUIRE(entry.path().filename() == "pairings.bin" ||
                 entry.path().filename() == ".pairings.lock");
  }
  SYNC_REQUIRE(store.authenticate(deck, first.token.view()).authenticated);
}

// The commit reached the filesystem but its durability could not be
// confirmed. The record must still be readable by a process that opens the
// store fresh -- syncd reports this as exit code 3, "committed, durability
// uncertain", and that claim is only honest if the data is genuinely there.
SYNC_TEST(pairing_store_reports_after_rename_issue_as_committed_but_durability_uncertain) {
  TempDirectory temporary;
  const auto path = temporary.path() / "state" / "pairings.bin";
  auto store = open_store(path, PairingStoreFailPoint::AfterRenameBeforeDirectorySync);
  const auto deck = origin("https://deck.example");
  const auto issued = store.issue(deck);
  SYNC_REQUIRE(issued.error == PairingStoreError::Io);
  SYNC_REQUIRE(issued.commit == PairingCommitState::CommittedDurabilityUncertain);
  SYNC_REQUIRE(all_lowercase_hex(issued.token.view()));
  SYNC_REQUIRE(store.authenticate(deck, issued.token.view()).authenticated);

  auto fresh = open_store(path);
  SYNC_REQUIRE(fresh.authenticate(deck, issued.token.view()).authenticated);
}

// The same for a revocation: an uncertain-durability revoke must still have
// actually revoked, or a token the operator believes is dead stays live.
SYNC_TEST(pairing_store_reports_after_rename_revoke_as_visible_but_durability_uncertain) {
  TempDirectory temporary;
  const auto path = temporary.path() / "state" / "pairings.bin";
  auto initial = open_store(path);
  const auto deck = origin("https://deck.example");
  const auto token = initial.issue(deck).token;

  auto store = open_store(path, PairingStoreFailPoint::AfterRenameBeforeDirectorySync);
  const auto revoked = store.revoke(deck);
  SYNC_REQUIRE(revoked.error == PairingStoreError::Io);
  SYNC_REQUIRE(revoked.commit == PairingCommitState::CommittedDurabilityUncertain);
  SYNC_REQUIRE(revoked.revoked);
  SYNC_REQUIRE(!store.authenticate(deck, token.view()).authenticated);

  auto fresh = open_store(path);
  SYNC_REQUIRE(!fresh.authenticate(deck, token.view()).authenticated);
}

SYNC_TEST(pairing_store_canceled_precommit_issue_leaves_empty_store_unchanged) {
  TempDirectory temporary;
  const auto state = temporary.path() / "state";
  const auto path = state / "pairings.bin";
  HoldingCommitHook hook;
  auto store = open_store(path, PairingStoreFailPoint::None, &hook);
  PairingCommitGate gate;
  noisefactor::sync::PairingIssueResult issued;
  std::thread issue([&] {
    issued = store.issue(origin("https://deck.example"), gate);
  });
  SYNC_REQUIRE(hook.wait_until_entered());
  SYNC_REQUIRE(gate.cancel());
  hook.release();
  issue.join();

  SYNC_REQUIRE(issued.error == PairingStoreError::Canceled);
  SYNC_REQUIRE(issued.commit == PairingCommitState::NotCommitted);
  SYNC_REQUIRE(issued.token.view().empty());
  SYNC_REQUIRE(!std::filesystem::exists(path));
  std::array<NormalizedOrigin, 64> listed{};
  SYNC_REQUIRE(store.list(listed).count == 0);
  for (const auto& entry : std::filesystem::directory_iterator(state)) {
    SYNC_REQUIRE(entry.path().filename() == ".pairings.lock");
  }
}

SYNC_TEST(pairing_store_canceled_rotation_preserves_bytes_and_old_token) {
  TempDirectory temporary;
  const auto path = temporary.path() / "state" / "pairings.bin";
  const auto deck = origin("https://deck.example");
  auto initial = open_store(path);
  auto first = initial.issue(deck);
  const std::string old_token(first.token.view());
  const auto before = read_bytes(path);

  HoldingCommitHook hook;
  auto rotating = open_store(path, PairingStoreFailPoint::None, &hook);
  PairingCommitGate gate;
  noisefactor::sync::PairingIssueResult issued;
  std::thread issue([&] { issued = rotating.issue(deck, gate); });
  SYNC_REQUIRE(hook.wait_until_entered());
  SYNC_REQUIRE(gate.cancel());
  hook.release();
  issue.join();

  SYNC_REQUIRE(issued.error == PairingStoreError::Canceled);
  SYNC_REQUIRE(issued.commit == PairingCommitState::NotCommitted);
  SYNC_REQUIRE(issued.token.view().empty());
  SYNC_REQUIRE(read_bytes(path) == before);
  SYNC_REQUIRE(rotating.authenticate(deck, old_token).authenticated);
}

SYNC_TEST(pairing_store_reloads_external_changes_before_auth_list_issue_and_revoke) {
  TempDirectory temporary;
  const auto path = temporary.path() / "state" / "pairings.bin";
  auto first = open_store(path);
  auto second = open_store(path);
  const auto a = origin("https://a.example");
  const auto b = origin("https://b.example");
  const auto a_token = first.issue(a).token;
  SYNC_REQUIRE(second.authenticate(a, a_token.view()).authenticated);
  const auto b_token = second.issue(b).token;

  std::array<NormalizedOrigin, 64> listed{};
  const auto current = first.list(listed);
  SYNC_REQUIRE(current.error == PairingStoreError::None);
  SYNC_REQUIRE(current.count == 2);
  SYNC_REQUIRE(first.authenticate(b, b_token.view()).authenticated);

  const auto revoked = second.revoke(a);
  SYNC_REQUIRE(revoked.error == PairingStoreError::None);
  SYNC_REQUIRE(revoked.revoked);
  SYNC_REQUIRE(!first.authenticate(a, a_token.view()).authenticated);
  SYNC_REQUIRE(!second.revoke(a).revoked);
}

SYNC_TEST(pairing_store_default_path_is_bounded_and_never_exposes_digest_material) {
  std::array<char, noisefactor::sync::kMaximumPairingStorePathBytes> path{};
  std::size_t length = 0;
  SYNC_REQUIRE(noisefactor::sync::default_pairing_store_path(path, length) ==
               PairingStoreError::None);
  SYNC_REQUIRE(length > 0);
  SYNC_REQUIRE(length < path.size());
  SYNC_REQUIRE(std::string_view(path.data(), length).find("pair") != std::string_view::npos);
}

SYNC_TEST(pairing_store_rejects_nul_and_control_bytes_in_explicit_paths) {
  TempDirectory temporary;
  const std::string valid = (temporary.path() / "state" / "pairings.bin").string();
  std::string nul = valid;
  nul.insert(nul.size() - 4, 1, '\0');
  PairingStore nul_store;
  SYNC_REQUIRE(nul_store.open({.path = nul}) == PairingStoreError::InvalidPath);

  std::string control = valid;
  control.insert(control.size() - 4, 1, '\n');
  PairingStore control_store;
  SYNC_REQUIRE(control_store.open({.path = control}) == PairingStoreError::InvalidPath);
}

#if !defined(_WIN32)
SYNC_TEST(pairing_store_creates_absent_default_path_ancestors_securely) {
  TempDirectory temporary;
  const char* previous_home = std::getenv("HOME");
  const std::string saved_home = previous_home == nullptr ? std::string{} : previous_home;
  SYNC_REQUIRE(::setenv("HOME", temporary.path().c_str(), 1) == 0);

  std::array<char, noisefactor::sync::kMaximumPairingStorePathBytes> path{};
  std::size_t length = 0;
  SYNC_REQUIRE(noisefactor::sync::default_pairing_store_path(path, length) ==
               PairingStoreError::None);
  PairingStore store;
  const auto opened = store.open({.path = std::string_view(path.data(), length)});

  if (previous_home == nullptr) {
    ::unsetenv("HOME");
  } else {
    ::setenv("HOME", saved_home.c_str(), 1);
  }
  SYNC_REQUIRE(opened == PairingStoreError::None);
  struct stat final_directory {};
  SYNC_REQUIRE(::stat(std::filesystem::path(path.data()).parent_path().c_str(), &final_directory) == 0);
  SYNC_REQUIRE((final_directory.st_mode & 0777) == 0700);
}
#else
SYNC_TEST(pairing_store_creates_absent_default_path_ancestors_securely) {
  TempDirectory temporary;
  bool had_previous = false;
  const std::string saved = get_local_app_data_for_test(had_previous);
  SYNC_REQUIRE(set_local_app_data_for_test(temporary.path().string()));

  std::array<char, noisefactor::sync::kMaximumPairingStorePathBytes> path{};
  std::size_t length = 0;
  SYNC_REQUIRE(noisefactor::sync::default_pairing_store_path(path, length) ==
               PairingStoreError::None);
  PairingStore store;
  const auto opened = store.open({.path = std::string_view(path.data(), length)});

  restore_local_app_data_for_test(had_previous, saved);
  SYNC_REQUIRE(opened == PairingStoreError::None);
  SYNC_REQUIRE(
      file_dacl_grants_only_current_user(std::filesystem::path(path.data()).parent_path()));
}

// Additional Windows-only coverage for default_pairing_store_path's
// %LOCALAPPDATA% branch, alongside the existing POSIX $HOME tests above:
// correct composition, rejection when the variable is missing or relative,
// and bounds enforcement.
SYNC_TEST(pairing_store_default_path_composes_local_app_data_and_suffix) {
  bool had_previous = false;
  const std::string saved = get_local_app_data_for_test(had_previous);
  SYNC_REQUIRE(set_local_app_data_for_test("C:\\Users\\tester\\AppData\\Local"));

  std::array<char, noisefactor::sync::kMaximumPairingStorePathBytes> path{};
  std::size_t length = 0;
  const auto error = noisefactor::sync::default_pairing_store_path(path, length);
  restore_local_app_data_for_test(had_previous, saved);

  SYNC_REQUIRE(error == PairingStoreError::None);
  const std::string_view produced(path.data(), length);
  SYNC_REQUIRE(produced ==
               "C:\\Users\\tester\\AppData\\Local\\Noisefactor Sync\\pairings.v1");
}

SYNC_TEST(pairing_store_default_path_rejects_missing_local_app_data) {
  bool had_previous = false;
  const std::string saved = get_local_app_data_for_test(had_previous);
  SYNC_REQUIRE(::_putenv_s("LOCALAPPDATA", "") == 0);

  std::array<char, noisefactor::sync::kMaximumPairingStorePathBytes> path{};
  std::size_t length = 123;
  const auto error = noisefactor::sync::default_pairing_store_path(path, length);
  restore_local_app_data_for_test(had_previous, saved);

  SYNC_REQUIRE(error == PairingStoreError::InvalidPath);
  SYNC_REQUIRE(length == 0);
}

SYNC_TEST(pairing_store_default_path_rejects_relative_local_app_data) {
  bool had_previous = false;
  const std::string saved = get_local_app_data_for_test(had_previous);
  SYNC_REQUIRE(set_local_app_data_for_test("Users\\tester\\AppData\\Local"));

  std::array<char, noisefactor::sync::kMaximumPairingStorePathBytes> path{};
  std::size_t length = 123;
  const auto error = noisefactor::sync::default_pairing_store_path(path, length);
  restore_local_app_data_for_test(had_previous, saved);

  SYNC_REQUIRE(error == PairingStoreError::InvalidPath);
  SYNC_REQUIRE(length == 0);
}

SYNC_TEST(pairing_store_default_path_enforces_bounds) {
  bool had_previous = false;
  const std::string saved = get_local_app_data_for_test(had_previous);
  const std::string huge = "C:\\" + std::string(noisefactor::sync::kMaximumPairingStorePathBytes, 'a');
  SYNC_REQUIRE(set_local_app_data_for_test(huge));

  std::array<char, noisefactor::sync::kMaximumPairingStorePathBytes> path{};
  std::size_t length = 123;
  const auto error = noisefactor::sync::default_pairing_store_path(path, length);
  restore_local_app_data_for_test(had_previous, saved);

  SYNC_REQUIRE(error == PairingStoreError::InvalidPath);
  SYNC_REQUIRE(length == 0);
}
#endif

#if !defined(_WIN32)
SYNC_TEST(pairing_store_rejects_a_symlinked_lock_file) {
  TempDirectory temporary;
  const auto state = temporary.path() / "state";
  std::filesystem::create_directory(state);
  SYNC_REQUIRE(::chmod(state.c_str(), 0700) == 0);
  const auto target = temporary.path() / "target";
  std::ofstream(target) << "target";
  SYNC_REQUIRE(::chmod(target.c_str(), 0600) == 0);
  SYNC_REQUIRE(::symlink(target.c_str(), (state / ".pairings.lock").c_str()) == 0);
  PairingStore store;
  SYNC_REQUIRE(store.open({.path = (state / "pairings.bin").string()}) ==
               PairingStoreError::FileSecurity);
}
#else
SYNC_TEST(pairing_store_rejects_a_symlinked_lock_file) {
  TempDirectory temporary;
  const auto state = temporary.path() / "state";
  std::filesystem::create_directory(state);
  const auto target = temporary.path() / "target";
  std::ofstream(target) << "target";
  secure_file_for_test(target);
  if (try_create_symlink(state / ".pairings.lock", target, false)) {
    PairingStore store;
    SYNC_REQUIRE(store.open({.path = (state / "pairings.bin").string()}) ==
                 PairingStoreError::FileSecurity);
  }
}
#endif

#if !defined(_WIN32)
SYNC_TEST(pairing_store_serializes_cross_process_reload_through_commit_without_lost_updates) {
  TempDirectory temporary;
  const auto path = temporary.path() / "state" / "pairings.bin";
  auto initial = open_store(path);

  int ready[2] = {-1, -1};
  int start[2] = {-1, -1};
  SYNC_REQUIRE(::pipe(ready) == 0);
  SYNC_REQUIRE(::pipe(start) == 0);
  std::array<pid_t, 2> children{};
  for (std::size_t index = 0; index < children.size(); ++index) {
    children[index] = ::fork();
    SYNC_REQUIRE(children[index] >= 0);
    if (children[index] == 0) {
      ::close(ready[0]);
      ::close(start[1]);
      PairingStore child;
      const auto opened = child.open({.path = path.string()});
      const char signal = 'r';
      const bool signaled = ::write(ready[1], &signal, 1) == 1;
      char release = 0;
      const bool released = ::read(start[0], &release, 1) == 1;
      const auto child_origin = origin(index == 0 ? "https://one.example"
                                                   : "https://two.example");
      const auto issued = child.issue(child_origin);
      ::_exit(opened == PairingStoreError::None && signaled && released &&
                      issued.error == PairingStoreError::None
                  ? 0
                  : 1);
    }
  }
  ::close(ready[1]);
  ::close(start[0]);
  std::array<char, 2> ready_bytes{};
  for (char& ready_byte : ready_bytes) {
    SYNC_REQUIRE(::read(ready[0], &ready_byte, 1) == 1);
  }
  SYNC_REQUIRE(::write(start[1], "go", 2) == 2);
  ::close(ready[0]);
  ::close(start[1]);
  for (const pid_t child : children) {
    int status = 0;
    SYNC_REQUIRE(::waitpid(child, &status, 0) == child);
    SYNC_REQUIRE(WIFEXITED(status));
    SYNC_REQUIRE(WEXITSTATUS(status) == 0);
  }
  std::array<NormalizedOrigin, 64> listed{};
  const auto result = initial.list(listed);
  SYNC_REQUIRE(result.error == PairingStoreError::None);
  SYNC_REQUIRE(result.count == 2);
}
#else
// Windows has no fork(); spawning real child processes to mirror this test
// exactly would need a second executable (or a re-exec-self-with-arguments
// scheme) plus named, cross-process synchronization objects, which was
// judged out of scope for this port. This substitutes two threads racing
// to issue through two INDEPENDENT PairingStore handles instead: each
// PairingStore::issue() call still opens its own fresh handle and takes a
// genuine LockFileEx lock via fs::acquire_store_lock (see
// native/src/platform/windows/pairing_store_fs.cpp), so this still
// exercises the real OS-level lock contention path, just not truly
// cross-process. No coverage is deleted -- the cross-process POSIX version
// above still runs on POSIX.
SYNC_TEST(pairing_store_serializes_concurrent_issue_through_lockfileex_without_lost_updates) {
  TempDirectory temporary;
  const auto path = temporary.path() / "state" / "pairings.bin";
  auto initial = open_store(path);

  std::array<noisefactor::sync::PairingIssueResult, 2> results{};
  std::array<std::thread, 2> workers;
  for (std::size_t index = 0; index < workers.size(); ++index) {
    workers[index] = std::thread([&, index] {
      PairingStore worker;
      SYNC_REQUIRE(worker.open({.path = path.string()}) == PairingStoreError::None);
      const auto worker_origin =
          origin(index == 0 ? "https://one.example" : "https://two.example");
      results[index] = worker.issue(worker_origin);
    });
  }
  for (auto& worker : workers) worker.join();
  SYNC_REQUIRE(results[0].error == PairingStoreError::None);
  SYNC_REQUIRE(results[1].error == PairingStoreError::None);

  std::array<NormalizedOrigin, 64> listed{};
  const auto result = initial.list(listed);
  SYNC_REQUIRE(result.error == PairingStoreError::None);
  SYNC_REQUIRE(result.count == 2);
}
#endif

#if !defined(_WIN32)
// Windows has no umask-equivalent ambient default permission for this
// store to fight against: build_owner_only_security
// (native/src/platform/windows/pairing_store_fs.cpp) always constructs and
// applies an explicit DACL at creation time regardless of any process- or
// account-level default, so there is nothing for a Windows mirror of this
// test to force. The security property this exercises on Windows is
// instead covered unconditionally by every other test in this file that
// checks file_dacl_grants_only_current_user.
SYNC_TEST(pairing_store_forces_owner_only_modes_under_a_restrictive_umask) {
  TempDirectory temporary;
  const auto state = temporary.path() / "state";
  const auto path = state / "pairings.bin";
  const mode_t previous = ::umask(0777);
  PairingStore store;
  const auto opened = store.open({.path = path.string()});
  const auto issued = opened == PairingStoreError::None
                          ? store.issue(origin("https://deck.example")).error
                          : PairingStoreError::Io;
  ::umask(previous);
  SYNC_REQUIRE(opened == PairingStoreError::None);
  SYNC_REQUIRE(issued == PairingStoreError::None);

  struct stat directory_status {};
  struct stat file_status {};
  struct stat lock_status {};
  SYNC_REQUIRE(::stat(state.c_str(), &directory_status) == 0);
  SYNC_REQUIRE(::stat(path.c_str(), &file_status) == 0);
  SYNC_REQUIRE(::stat((state / ".pairings.lock").c_str(), &lock_status) == 0);
  SYNC_REQUIRE((directory_status.st_mode & 0777) == 0700);
  SYNC_REQUIRE((file_status.st_mode & 0777) == 0600);
  SYNC_REQUIRE((lock_status.st_mode & 0777) == 0600);
}
#endif

#if !defined(_WIN32)
SYNC_TEST(pairing_store_default_path_rejects_unsafe_home_bytes) {
  const char* previous_home = std::getenv("HOME");
  const std::string saved_home = previous_home == nullptr ? std::string{} : previous_home;
  SYNC_REQUIRE(::setenv("HOME", "/private/tmp/unsafe\nhome", 1) == 0);
  std::array<char, noisefactor::sync::kMaximumPairingStorePathBytes> path{};
  std::size_t length = 123;
  const auto error = noisefactor::sync::default_pairing_store_path(path, length);
  if (previous_home == nullptr) {
    ::unsetenv("HOME");
  } else {
    ::setenv("HOME", saved_home.c_str(), 1);
  }
  SYNC_REQUIRE(error == PairingStoreError::InvalidPath);
  SYNC_REQUIRE(length == 0);
}
#else
SYNC_TEST(pairing_store_default_path_rejects_unsafe_local_app_data_bytes) {
  bool had_previous = false;
  const std::string saved = get_local_app_data_for_test(had_previous);
  SYNC_REQUIRE(set_local_app_data_for_test("C:\\unsafe\nplace"));
  std::array<char, noisefactor::sync::kMaximumPairingStorePathBytes> path{};
  std::size_t length = 123;
  const auto error = noisefactor::sync::default_pairing_store_path(path, length);
  restore_local_app_data_for_test(had_previous, saved);
  SYNC_REQUIRE(error == PairingStoreError::InvalidPath);
  SYNC_REQUIRE(length == 0);
}
#endif

SYNC_TEST(pairing_store_validates_filename_plus_temporary_suffix_against_name_max) {
  TempDirectory temporary;
  // The store file goes one level below the temp directory, in a "state"
  // component the store creates itself. TempDirectory's own directory is made
  // by std::filesystem and so carries whatever the platform gives it -- umask
  // on POSIX, ACEs inherited from %TEMP% on Windows -- neither of which is the
  // owner-only permission the store requires of the directory it lives in.
  // Every other test here uses the same shape for the same reason.
  const auto directory = temporary.path() / "state";
  constexpr std::size_t suffix_bytes = 21;
  const std::string accepted_name(noisefactor::sync::fs::kMaximumPathComponentBytes - suffix_bytes,
                                  'a');
  PairingStore accepted;
  SYNC_REQUIRE(accepted.open({.path = (directory / accepted_name).string()}) ==
               PairingStoreError::None);

  const std::string rejected_name(
      noisefactor::sync::fs::kMaximumPathComponentBytes - suffix_bytes + 1, 'b');
  PairingStore rejected;
  SYNC_REQUIRE(rejected.open({.path = (temporary.path() / rejected_name).string()}) ==
               PairingStoreError::InvalidPath);

  PairingStore reserved;
  SYNC_REQUIRE(reserved.open({.path = (temporary.path() / ".pairings.lock").string()}) ==
               PairingStoreError::InvalidPath);
}

#if defined(__APPLE__)
SYNC_TEST(pairing_store_rejects_extended_acls_on_existing_sensitive_objects) {
  TempDirectory temporary;
  const auto state = temporary.path() / "state";
  const auto path = state / "pairings.bin";
  auto store = open_store(path);
  SYNC_REQUIRE(store.issue(origin("https://deck.example")).error == PairingStoreError::None);

  int fd = ::open(state.c_str(), O_RDONLY | O_DIRECTORY);
  SYNC_REQUIRE(fd >= 0);
  add_extended_acl(fd);
  ::close(fd);
  PairingStore directory_acl;
  SYNC_REQUIRE(directory_acl.open({.path = path.string()}) ==
               PairingStoreError::DirectorySecurity);

  fd = ::open(state.c_str(), O_RDONLY | O_DIRECTORY);
  SYNC_REQUIRE(fd >= 0);
  ScopedAcl empty(::acl_init(0));
  SYNC_REQUIRE(::acl_set_fd_np(fd, empty.get(), ACL_TYPE_EXTENDED) == 0);
  ::close(fd);
  fd = ::open(path.c_str(), O_RDWR);
  SYNC_REQUIRE(fd >= 0);
  add_extended_acl(fd);
  ::close(fd);
  PairingStore store_acl;
  SYNC_REQUIRE(store_acl.open({.path = path.string()}) == PairingStoreError::FileSecurity);

  fd = ::open(path.c_str(), O_RDWR);
  SYNC_REQUIRE(fd >= 0);
  SYNC_REQUIRE(::acl_set_fd_np(fd, empty.get(), ACL_TYPE_EXTENDED) == 0);
  ::close(fd);
  fd = ::open((state / ".pairings.lock").c_str(), O_RDWR);
  SYNC_REQUIRE(fd >= 0);
  add_extended_acl(fd);
  ::close(fd);
  PairingStore lock_acl;
  SYNC_REQUIRE(lock_acl.open({.path = path.string()}) == PairingStoreError::FileSecurity);
}

SYNC_TEST(pairing_store_strips_inherited_acl_from_new_final_state_directory) {
  TempDirectory temporary;
  int parent = ::open(temporary.path().c_str(), O_RDONLY | O_DIRECTORY);
  SYNC_REQUIRE(parent >= 0);
  add_extended_acl(parent, true);
  ::close(parent);
  const auto state = temporary.path() / "state";
  PairingStore store;
  SYNC_REQUIRE(store.open({.path = (state / "pairings.bin").string()}) ==
               PairingStoreError::None);
  int state_fd = ::open(state.c_str(), O_RDONLY | O_DIRECTORY);
  SYNC_REQUIRE(state_fd >= 0);
  SYNC_REQUIRE(!has_extended_acl(state_fd));
  ::close(state_fd);
  const auto issued = store.issue(origin("https://deck.example"));
  SYNC_REQUIRE(issued.error == PairingStoreError::None);
  int store_fd = ::open((state / "pairings.bin").c_str(), O_RDONLY);
  int lock_fd = ::open((state / ".pairings.lock").c_str(), O_RDONLY);
  SYNC_REQUIRE(store_fd >= 0);
  SYNC_REQUIRE(lock_fd >= 0);
  SYNC_REQUIRE(!has_extended_acl(store_fd));
  SYNC_REQUIRE(!has_extended_acl(lock_fd));
  ::close(store_fd);
  ::close(lock_fd);
}

SYNC_TEST(pairing_store_rejects_an_inherited_acl_even_with_exact_mode_bits) {
  TempDirectory temporary;
  int parent = ::open(temporary.path().c_str(), O_RDONLY | O_DIRECTORY);
  SYNC_REQUIRE(parent >= 0);
  add_extended_acl(parent, true);
  ::close(parent);
  const auto state = temporary.path() / "state";
  SYNC_REQUIRE(::mkdir(state.c_str(), 0700) == 0);
  SYNC_REQUIRE(::chmod(state.c_str(), 0700) == 0);
  int state_fd = ::open(state.c_str(), O_RDONLY | O_DIRECTORY);
  SYNC_REQUIRE(state_fd >= 0);
  SYNC_REQUIRE(has_extended_acl(state_fd));
  ::close(state_fd);
  PairingStore store;
  SYNC_REQUIRE(store.open({.path = (state / "pairings.bin").string()}) ==
               PairingStoreError::DirectorySecurity);
}
#endif

#if defined(_WIN32)
// Windows-only mirror of the two macOS extended-ACL tests above: a
// pre-existing final directory/file/lock carrying a widened DACL (rather
// than an inherited macOS ACL) must be refused the same way.
SYNC_TEST(pairing_store_rejects_widened_dacls_on_existing_sensitive_objects) {
  TempDirectory temporary;
  const auto state = temporary.path() / "state";
  const auto path = state / "pairings.bin";
  auto store = open_store(path);
  SYNC_REQUIRE(store.issue(origin("https://deck.example")).error == PairingStoreError::None);

  widen_permissions_for_test(state);
  PairingStore directory_dacl;
  SYNC_REQUIRE(directory_dacl.open({.path = path.string()}) ==
               PairingStoreError::DirectorySecurity);
  secure_file_for_test(state);

  widen_permissions_for_test(path);
  PairingStore store_dacl;
  SYNC_REQUIRE(store_dacl.open({.path = path.string()}) == PairingStoreError::FileSecurity);
  secure_file_for_test(path);

  widen_permissions_for_test(state / ".pairings.lock");
  PairingStore lock_dacl;
  SYNC_REQUIRE(lock_dacl.open({.path = path.string()}) == PairingStoreError::FileSecurity);
}
#endif
