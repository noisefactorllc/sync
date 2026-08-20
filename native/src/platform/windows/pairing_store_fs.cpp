#include "../../pairing_store_fs.hpp"

// Windows filesystem primitives behind PairingStore. See
// native/src/pairing_store_fs.hpp for the contract every function here must
// meet, and native/src/platform/posix/pairing_store_fs.cpp for the POSIX
// implementation these are ports of -- comments below call out each
// POSIX -> Win32 mapping decision at the point it is made.
//
// IMPORTANT Windows-specific gotcha that shapes this whole file:
// FILE_FLAG_OPEN_REPARSE_POINT only stops CreateFileW from following a
// reparse point (symlink/junction/mount point) that is the LAST component
// of the path passed to it. Any reparse point earlier in the path is
// followed transparently, same as an ordinary CreateFileW call would. This
// is why every entry point below calls open_secure_directory -- which walks
// and checks every ancestor component individually -- before ever touching
// a child file by its full path; a single CreateFileW on a multi-component
// path is not sufficient to refuse a symlinked/junctioned ancestor.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace noisefactor::sync::fs {

ScopedHandle::ScopedHandle() noexcept : value_(INVALID_HANDLE_VALUE) {}
ScopedHandle::~ScopedHandle() noexcept { close(); }
ScopedHandle::ScopedHandle(void* native) noexcept : value_(native) {}

ScopedHandle::ScopedHandle(ScopedHandle&& other) noexcept : value_(other.value_) {
  other.value_ = INVALID_HANDLE_VALUE;
}

ScopedHandle& ScopedHandle::operator=(ScopedHandle&& other) noexcept {
  if (this != &other) {
    close();
    value_ = other.value_;
    other.value_ = INVALID_HANDLE_VALUE;
  }
  return *this;
}

// INVALID_HANDLE_VALUE, not nullptr, is the "empty" sentinel: every handle
// this file creates comes from CreateFileW, whose documented failure value
// is INVALID_HANDLE_VALUE. (nullptr is technically a value some other Win32
// APIs use for "no handle", but never this one, so treating it as "valid"
// here would be wrong.)
bool ScopedHandle::valid() const noexcept { return value_ != INVALID_HANDLE_VALUE; }

void ScopedHandle::close() noexcept {
  if (value_ != INVALID_HANDLE_VALUE) {
    ::CloseHandle(static_cast<HANDLE>(value_));
    value_ = INVALID_HANDLE_VALUE;
  }
}

void* ScopedHandle::native() const noexcept { return value_; }

namespace {

// A SID buffer obtained from GetTokenInformation(TokenUser); `sid` points
// inside `buffer` and stays valid as long as this object is alive (moving a
// std::vector never reallocates its heap block, so the pointer survives a
// move of the owning ScopedSid too).
class ScopedSid {
 public:
  ScopedSid() noexcept = default;
  ScopedSid(std::vector<unsigned char> buffer, PSID sid) noexcept
      : buffer_(std::move(buffer)), sid_(sid) {}
  [[nodiscard]] PSID sid() const noexcept { return sid_; }

 private:
  std::vector<unsigned char> buffer_;
  PSID sid_ = nullptr;
};

bool get_current_user_sid(ScopedSid& out) noexcept {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  struct TokenGuard {
    HANDLE value;
    ~TokenGuard() { ::CloseHandle(value); }
  } token_guard{token};

  DWORD needed = 0;
  ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
  if (needed == 0) return false;
  std::vector<unsigned char> buffer(needed);
  if (!::GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) return false;
  const auto* info = reinterpret_cast<const TOKEN_USER*>(buffer.data());
  if (info->User.Sid == nullptr || !::IsValidSid(info->User.Sid)) return false;
  PSID sid = info->User.Sid;
  out = ScopedSid(std::move(buffer), sid);
  return true;
}

// A well-known max SID (S-1-5-21-<3x32bit>-32bit RID) is well under this;
// defined locally rather than relying on an SDK-version-dependent macro.
constexpr DWORD kMaximumSidBytes = 68;

// Bundles the security descriptor + single-ACE DACL applied to everything
// this store creates, plus the buffers Win32's SECURITY_ATTRIBUTES /
// SECURITY_DESCRIPTOR / ACL point into -- those are non-owning pointers, so
// this struct's lifetime must dominate whatever CreateFileW/CreateDirectoryW
// call it is passed to. The owner SID is not copied in either, so the
// ScopedSid it came from must outlive this too -- every call site below
// declares the ScopedSid first, which makes it outlive this by destruction
// order.
struct OwnerOnlySecurity {
  SECURITY_DESCRIPTOR descriptor{};
  std::array<unsigned char, sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + kMaximumSidBytes>
      acl_buffer{};
  SECURITY_ATTRIBUTES attributes{};
};

bool build_owner_only_security(PSID user_sid, OwnerOnlySecurity& out) noexcept {
  if (!::InitializeSecurityDescriptor(&out.descriptor, SECURITY_DESCRIPTOR_REVISION)) return false;
  // The owner must be set explicitly rather than left to the token default.
  // A security descriptor with no owner takes the creating token's *default
  // owner* (TokenOwner), which is not always the token user (TokenUser):
  // for a member of the Administrators group the default owner can be the
  // BUILTIN\Administrators group instead, depending on the "System objects:
  // Default owner for objects created by members of the Administrators
  // group" security policy. Since verify_owner_only_security() below
  // requires the owner to be exactly the token user, leaving it implicit
  // made every object this store creates fail its own verification on such
  // an account -- syncd on an administrator machine could not open its
  // pairing store at all. Naming the owner here removes the dependency on
  // that policy entirely: setting the owner to one's own TokenUser SID is
  // always permitted and needs no privilege.
  if (!::SetSecurityDescriptorOwner(&out.descriptor, user_sid, FALSE)) return false;
  auto* acl = reinterpret_cast<PACL>(out.acl_buffer.data());
  if (!::InitializeAcl(acl, static_cast<DWORD>(out.acl_buffer.size()), ACL_REVISION)) return false;
  if (!::AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, user_sid)) return false;
  if (!::SetSecurityDescriptorDacl(&out.descriptor, TRUE, acl, FALSE)) return false;
  // SE_DACL_PROTECTED stops this object from inheriting any ACE from its
  // parent directory. This is the creation-time analogue of the POSIX
  // strip_extended_acl() call: that strips an ACL a parent could already
  // have propagated onto an object; this refuses to let a parent propagate
  // one in the first place. Without it, a parent directory carrying an
  // inheritable ACE (see the "inherited ACL" POSIX test on macOS) could
  // silently widen access to whatever this call creates.
  if (!::SetSecurityDescriptorControl(&out.descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
    return false;
  }
  out.attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
  out.attributes.lpSecurityDescriptor = &out.descriptor;
  out.attributes.bInheritHandle = FALSE;
  return true;
}

// Verifies an existing (possibly pre-existing, possibly just-created)
// object's security matches exactly what build_owner_only_security grants:
// owned by `expected_owner`, a DACL protected from inheritance, holding
// exactly one ACE that grants exactly FILE_ALL_ACCESS to exactly
// `expected_owner` and nothing else. This single check plays the role both
// of the POSIX mode-bits check (0700/0600, i.e. "owner only") and the
// separate extended-ACL-is-empty check (macOS only) combined, because on
// Windows the DACL *is* the permission model -- there is no analogue of
// mode bits sitting below it to check separately.
bool verify_owner_only_security(HANDLE handle, PSID expected_owner) noexcept {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD status =
      ::GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                       &owner, nullptr, &dacl, nullptr, &descriptor);
  if (status != ERROR_SUCCESS || descriptor == nullptr) return false;
  // GetSecurityInfo hands back memory allocated with LocalAlloc; it must be
  // freed with LocalFree exactly once, on every return path.
  struct DescriptorGuard {
    PSECURITY_DESCRIPTOR value;
    ~DescriptorGuard() {
      if (value != nullptr) ::LocalFree(value);
    }
  } guard{descriptor};

  if (owner == nullptr || !::EqualSid(owner, expected_owner)) return false;

  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  if (!::GetSecurityDescriptorControl(descriptor, &control, &revision)) return false;
  if ((control & SE_DACL_PROTECTED) == 0) return false;

  if (dacl == nullptr) return false;  // a null DACL means "Everyone", never acceptable here.
  ACL_SIZE_INFORMATION size_info{};
  if (!::GetAclInformation(dacl, &size_info, sizeof(size_info), AclSizeInformation)) return false;
  if (size_info.AceCount != 1) return false;

  LPVOID ace = nullptr;
  if (!::GetAce(dacl, 0, &ace)) return false;
  const auto* header = static_cast<ACE_HEADER*>(ace);
  if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || header->AceFlags != 0) return false;
  const auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
  if (allowed->Mask != FILE_ALL_ACCESS) return false;
  const PSID ace_sid = const_cast<PSID>(static_cast<const void*>(&allowed->SidStart));
  return ::EqualSid(ace_sid, expected_owner) != 0;
}

struct HandleInfo {
  bool is_directory = false;
  bool is_reparse_point = false;
  std::uint32_t links = 0;
  std::uint64_t size = 0;
};

bool query_handle_info(HANDLE handle, HandleInfo& out) noexcept {
  BY_HANDLE_FILE_INFORMATION info{};
  if (!::GetFileInformationByHandle(handle, &info)) return false;
  out.is_directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  // A reparse point covers symlinks, junctions, AND mount points -- all
  // three must be refused the same way O_NOFOLLOW refuses a POSIX symlink,
  // since all three let a name resolve to storage outside what was
  // validated as owner-only.
  out.is_reparse_point = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  out.links = info.nNumberOfLinks;
  out.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  return true;
}

bool is_drive_letter(char value) noexcept {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

// A bounded, stack-resident wide-character path buffer -- deliberately not
// std::wstring, to keep this file's buffers fixed-size like the rest of the
// codebase. Sized well beyond kMaximumPairingStorePathBytes: a directory
// string already bounded by kMaximumPairingStorePathBytes can still be
// joined with a temporary filename (the real filename plus up to
// kMaximumPathComponentBytes' worth of ".tmp-<hex>" suffix), so the
// combined "\\?\" + directory + '\' + filename could slightly exceed
// kMaximumPairingStorePathBytes even though each half was independently in
// bounds. build_extended_path below still bounds-checks every write into
// this buffer regardless -- this sizing just keeps that rejection from
// firing on paths that are otherwise entirely legitimate.
struct WidePath {
  std::array<wchar_t, kMaximumPairingStorePathBytes + kMaximumPathComponentBytes + 16> buffer{};
  std::size_t length = 0;
  [[nodiscard]] const wchar_t* c_str() const noexcept { return buffer.data(); }
};

// Widens `utf8` strictly: MB_ERR_INVALID_CHARS makes this fail rather than
// silently substitute U+FFFD for a byte sequence that is not valid UTF-8,
// per the port's requirement to reject bad input outright.
bool to_wide_strict(std::string_view utf8, WidePath& out) noexcept {
  out.length = 0;
  if (utf8.empty()) return true;
  if (utf8.size() >= kMaximumPairingStorePathBytes) return false;
  const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0 || static_cast<std::size_t>(needed) >= out.buffer.size()) return false;
  const int written = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                            static_cast<int>(utf8.size()), out.buffer.data(), needed);
  if (written != needed) return false;
  out.length = static_cast<std::size_t>(written);
  out.buffer[out.length] = L'\0';
  return true;
}

// Builds an extended-length ("\\?\"-prefixed) absolute wide path for
// `directory` alone (filename empty) or for `directory\filename`.
//
// The \\?\ prefix is used for every path this file touches: it lifts the
// ~260-character MAX_PATH limit (kMaximumPairingStorePathBytes allows paths
// well beyond that), and it disables Win32's own "." / ".." collapsing and
// '/'-as-separator translation. That canonicalization is safe to disable
// here specifically because path_is_bounded_absolute has already rejected
// "." / ".." components and forward slashes in every path this seam ever
// receives, and every component is independently bounded by
// kMaximumPathComponentBytes -- there is nothing left for Win32's
// canonicalization to have done that this code has not already checked
// itself.
bool build_extended_path(std::string_view directory, std::string_view filename,
                         WidePath& out) noexcept {
  out.length = 0;
  constexpr std::wstring_view kPrefix = L"\\\\?\\";
  if (kPrefix.size() >= out.buffer.size()) return false;
  std::copy(kPrefix.begin(), kPrefix.end(), out.buffer.begin());
  out.length = kPrefix.size();

  WidePath wide_directory{};
  if (!to_wide_strict(directory, wide_directory)) return false;
  if (out.length + wide_directory.length >= out.buffer.size()) return false;
  std::copy(wide_directory.buffer.begin(), wide_directory.buffer.begin() + wide_directory.length,
            out.buffer.begin() + out.length);
  out.length += wide_directory.length;

  if (!filename.empty()) {
    if (out.length + 1 >= out.buffer.size()) return false;
    out.buffer[out.length++] = L'\\';
    WidePath wide_name{};
    if (!to_wide_strict(filename, wide_name)) return false;
    if (out.length + wide_name.length >= out.buffer.size()) return false;
    std::copy(wide_name.buffer.begin(), wide_name.buffer.begin() + wide_name.length,
              out.buffer.begin() + out.length);
    out.length += wide_name.length;
  }
  out.buffer[out.length] = L'\0';
  return true;
}

bool full_read(HANDLE handle, unsigned char* output, std::size_t length) noexcept {
  std::size_t offset = 0;
  while (offset < length) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<std::size_t>(length - offset, 0x4000'0000U));
    DWORD amount = 0;
    if (!::ReadFile(handle, output + offset, chunk, &amount, nullptr) || amount == 0) return false;
    offset += amount;
  }
  return true;
}

bool full_write(HANDLE handle, const unsigned char* input, std::size_t length) noexcept {
  std::size_t offset = 0;
  while (offset < length) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<std::size_t>(length - offset, 0x4000'0000U));
    DWORD amount = 0;
    if (!::WriteFile(handle, input + offset, chunk, &amount, nullptr) || amount == 0) return false;
    offset += amount;
  }
  return true;
}

}  // namespace

PairingStoreError open_secure_directory(std::string_view directory, ScopedHandle& output,
                                        bool create) noexcept {
  // Mirrors path_is_bounded_absolute's Windows branch (drive-absolute,
  // e.g. "C:\Users\..."); re-checking the shape here rather than trusting
  // callers keeps this function safe to call directly, exactly like the
  // POSIX version re-checks for a leading '/'.
  if (directory.size() < 3 || !is_drive_letter(directory[0]) || directory[1] != ':' ||
      directory[2] != '\\') {
    return PairingStoreError::InvalidPath;
  }
  ScopedSid user;
  if (!get_current_user_sid(user)) return PairingStoreError::Io;
  OwnerOnlySecurity security{};
  if (!build_owner_only_security(user.sid(), security)) return PairingStoreError::Io;

  std::string_view parent_prefix = directory.substr(0, 3);  // the drive root, e.g. "C:\"
  std::size_t start = 3;
  while (start < directory.size()) {
    const std::size_t backslash = directory.find('\\', start);
    const std::size_t end = backslash == std::string_view::npos ? directory.size() : backslash;
    const auto component = directory.substr(start, end - start);
    if (component.empty() || component.size() > kMaximumPathComponentBytes) {
      return PairingStoreError::InvalidPath;
    }
    const bool final = end == directory.size();
    const auto prefix = directory.substr(0, end);

    WidePath wide_path{};
    if (!build_extended_path(prefix, {}, wide_path)) return PairingStoreError::InvalidPath;

    // FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL --
    // deliberately no write access -- is all this walk asks for: it must be
    // able to read through ancestor directories the store does not own and
    // may not have write access to (the %LOCALAPPDATA% root, say) purely to
    // validate them. FILE_READ_ATTRIBUTES is what GetFileInformationByHandle
    // needs; READ_CONTROL is what verify_owner_only_security's
    // GetSecurityInfo call below needs (it fails outright without it, even
    // though it is a read of the security descriptor, not a write).
    constexpr DWORD kDirectoryReadAccess =
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL;
    bool created = false;
    HANDLE opened = ::CreateFileW(
        wide_path.c_str(), kDirectoryReadAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (opened == INVALID_HANDLE_VALUE) {
      const DWORD open_error = ::GetLastError();
      if (!create || (open_error != ERROR_FILE_NOT_FOUND && open_error != ERROR_PATH_NOT_FOUND)) {
        return PairingStoreError::Io;
      }
      if (::CreateDirectoryW(wide_path.c_str(), &security.attributes)) {
        created = true;
      } else if (::GetLastError() != ERROR_ALREADY_EXISTS) {
        return PairingStoreError::Io;
      }
      opened = ::CreateFileW(
          wide_path.c_str(), kDirectoryReadAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (opened == INVALID_HANDLE_VALUE) return PairingStoreError::Io;
    }
    ScopedHandle handle(opened);

    HandleInfo info{};
    if (!query_handle_info(opened, info) || info.is_reparse_point || !info.is_directory) {
      return PairingStoreError::DirectorySecurity;
    }

    if (created) {
      // Durably commit the freshly-created directory entry into its
      // parent, mirroring fsync(current) in the POSIX walk. A dedicated
      // handle is opened for this (rather than reusing `opened`) because
      // FlushFileBuffers needs GENERIC_WRITE access, which this walk
      // otherwise never requests -- see the FILE_LIST_DIRECTORY comment
      // above for why that matters for ancestors the store does not own.
      WidePath parent_wide{};
      HANDLE parent_handle = INVALID_HANDLE_VALUE;
      if (build_extended_path(parent_prefix, {}, parent_wide)) {
        parent_handle =
            ::CreateFileW(parent_wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS, nullptr);
      }
      const bool flushed =
          parent_handle != INVALID_HANDLE_VALUE && ::FlushFileBuffers(parent_handle) != 0;
      if (parent_handle != INVALID_HANDLE_VALUE) ::CloseHandle(parent_handle);
      if (!flushed) return PairingStoreError::Io;
    }

    if (final) {
      // Whether just-created or pre-existing, the final directory must
      // carry exactly the owner-only, non-inherited DACL -- the Windows
      // analogue of the POSIX "mode 0700 + empty extended ACL" check.
      // Unlike POSIX, Windows needs no separate "just created" branch here:
      // the DACL check below is already a single, complete verification of
      // both "owner only" and "nothing extra", where POSIX has to check
      // mode bits and the macOS extended ACL as two separate mechanisms.
      if (!verify_owner_only_security(opened, user.sid())) {
        return PairingStoreError::DirectorySecurity;
      }
      output = std::move(handle);
      return PairingStoreError::None;
    }
    parent_prefix = prefix;
    start = end + 1;
  }
  return PairingStoreError::InvalidPath;
}

PairingStoreError acquire_store_lock(std::string_view directory, ScopedHandle& lock) noexcept {
  ScopedHandle dir;
  const PairingStoreError directory_error = open_secure_directory(directory, dir, false);
  if (directory_error != PairingStoreError::None) return directory_error;
  // The validated handle is deliberately HELD for the rest of this function
  // rather than closed here. It is opened without FILE_SHARE_DELETE, so while
  // it is alive this directory cannot be renamed or deleted -- and swapping a
  // directory for a junction requires exactly that. Closing it before doing
  // the real work, as this used to, reopened the window between validating a
  // path and using it. See the TOCTOU note in pairing_store_fs.hpp for what
  // this does and does not cover.

  ScopedSid user;
  if (!get_current_user_sid(user)) return PairingStoreError::Io;
  OwnerOnlySecurity security{};
  if (!build_owner_only_security(user.sid(), security)) return PairingStoreError::Io;

  constexpr std::string_view kLockName = ".pairings.lock";
  WidePath wide_path{};
  if (!build_extended_path(directory, kLockName, wide_path)) return PairingStoreError::InvalidPath;

  // FILE_SHARE_READ | FILE_SHARE_WRITE, and the exclusion comes entirely from
  // LockFileEx below. Denying write-sharing here looks stricter but is
  // actively wrong: a second opener contending for the lock would fail this
  // CreateFileW with ERROR_SHARING_VIOLATION and never reach the retry loop,
  // so ordinary, expected contention would surface as a hard FileSecurity
  // error instead of resolving on retry. POSIX has the same shape for the
  // same reason -- openat() always succeeds and flock() arbitrates.
  //
  // FILE_SHARE_DELETE stays off so the lock file cannot be unlinked out from
  // under a holder. FILE_FLAG_OPEN_REPARSE_POINT refuses to follow a
  // symlinked lock file, same reasoning as everywhere else in this file.
  HANDLE handle = ::CreateFileW(wide_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, &security.attributes,
                                OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return PairingStoreError::FileSecurity;
  lock = ScopedHandle(handle);

  HandleInfo info{};
  if (!query_handle_info(handle, info) || info.is_directory || info.is_reparse_point ||
      info.links != 1) {
    return PairingStoreError::FileSecurity;
  }
  // Security attributes only apply to a newly-created file, so a
  // pre-existing lock file's DACL must still be verified explicitly here --
  // exactly like the POSIX path, which only fchmod()s/strips the ACL when
  // it created the file but always re-fstats and checks mode/ACL either
  // way.
  if (!verify_owner_only_security(handle, user.sid())) return PairingStoreError::FileSecurity;

  OVERLAPPED overlapped{};
  constexpr std::size_t kAttempts = 50;
  for (std::size_t attempt = 0; attempt < kAttempts; ++attempt) {
    if (::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD,
                     MAXDWORD, &overlapped)) {
      return PairingStoreError::None;
    }
    if (::GetLastError() != ERROR_LOCK_VIOLATION) return PairingStoreError::Io;
    ::Sleep(2);
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
  // Handle held, not closed -- see the first such site above.

  ScopedSid user;
  if (!get_current_user_sid(user)) {
    result.error = PairingStoreError::Io;
    return result;
  }

  WidePath wide_path{};
  if (!build_extended_path(directory, filename, wide_path)) {
    result.error = PairingStoreError::InvalidPath;
    return result;
  }
  // A single open -- rather than POSIX's separate stat-then-open -- both
  // refuses to follow a symlink/reparse point (FILE_FLAG_OPEN_REPARSE_POINT
  // opens the reparse point object itself instead of transparently
  // traversing it) and sidesteps the TOCTOU window POSIX defeats with its
  // dev/ino re-check across two syscalls: there is only ever one handle
  // here, so there is nothing for a second query to disagree with.
  HANDLE handle = ::CreateFileW(wide_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD open_error = ::GetLastError();
    if (open_error == ERROR_FILE_NOT_FOUND || open_error == ERROR_PATH_NOT_FOUND) {
      result.error = PairingStoreError::None;
      result.exists = false;
      return result;
    }
    result.error = PairingStoreError::Io;
    return result;
  }
  ScopedHandle file(handle);
  HandleInfo info{};
  if (!query_handle_info(handle, info)) {
    result.error = PairingStoreError::Io;
    return result;
  }
  if (info.is_reparse_point || info.is_directory || info.links != 1) {
    result.error = PairingStoreError::FileSecurity;
    return result;
  }
  if (!verify_owner_only_security(handle, user.sid())) {
    result.error = PairingStoreError::FileSecurity;
    return result;
  }
  if (info.size == 0 || info.size > buffer.size()) {
    result.error = PairingStoreError::Corrupt;
    return result;
  }
  const std::size_t size = static_cast<std::size_t>(info.size);
  if (!full_read(handle, buffer.data(), size)) {
    result.error = PairingStoreError::Io;
    return result;
  }
  unsigned char extra = 0;
  DWORD extra_read = 0;
  if (!::ReadFile(handle, &extra, 1, &extra_read, nullptr)) {
    result.error = PairingStoreError::Io;
    return result;
  }
  if (extra_read != 0) {
    result.error = PairingStoreError::Corrupt;
    return result;
  }
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
  // Handle held, not closed -- see the first such site above.

  ScopedSid user;
  if (!get_current_user_sid(user)) return PairingStoreError::Io;
  OwnerOnlySecurity security{};
  if (!build_owner_only_security(user.sid(), security)) return PairingStoreError::Io;

  WidePath wide_path{};
  if (!build_extended_path(directory, filename, wide_path)) return PairingStoreError::InvalidPath;

  // CREATE_NEW is the Win32 analogue of O_CREAT | O_EXCL: it fails with
  // ERROR_FILE_EXISTS if anything -- including a dangling symlink/reparse
  // point placeholder -- already occupies that name.
  // FILE_FLAG_OPEN_REPARSE_POINT is kept anyway as defense in depth on top
  // of that.
  HANDLE handle =
      ::CreateFileW(wide_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                   &security.attributes, CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return PairingStoreError::Io;
  ScopedHandle file(handle);

  HandleInfo info{};
  const bool ready = query_handle_info(handle, info) && !info.is_reparse_point &&
                     !info.is_directory && info.links == 1 &&
                     verify_owner_only_security(handle, user.sid()) &&
                     full_write(handle, bytes.data(), bytes.size()) &&
                     ::FlushFileBuffers(handle) != 0;
  file.close();
  if (!ready) {
    ::DeleteFileW(wide_path.c_str());
    return PairingStoreError::Io;
  }
  return PairingStoreError::None;
}

void remove_file(std::string_view directory, std::string_view filename) noexcept {
  ScopedHandle dir;
  if (open_secure_directory(directory, dir, false) != PairingStoreError::None) return;
  // Handle held, not closed -- see the first such site above.
  WidePath wide_path{};
  if (!build_extended_path(directory, filename, wide_path)) return;
  ::DeleteFileW(wide_path.c_str());
}

CommitResult rename_into_place(std::string_view directory, std::string_view temporary_filename,
                               std::string_view final_filename,
                               bool simulate_directory_sync_failure) noexcept {
  CommitResult result{};
  ScopedHandle dir;
  const PairingStoreError directory_error = open_secure_directory(directory, dir, false);
  if (directory_error != PairingStoreError::None) {
    remove_file(directory, temporary_filename);
    result.error = directory_error;
    return result;
  }
  // Handle held, not closed -- see the first such site above.

  WidePath temporary_path{};
  WidePath final_path{};
  if (!build_extended_path(directory, temporary_filename, temporary_path) ||
      !build_extended_path(directory, final_filename, final_path)) {
    remove_file(directory, temporary_filename);
    result.error = PairingStoreError::InvalidPath;
    return result;
  }

  // Phase 1: the visible rename. Deliberately NOT MOVEFILE_WRITE_THROUGH --
  // that flag folds "renamed" and "durably committed" into a single
  // all-or-nothing call, but PairingCommitState needs those as two
  // distinguishable outcomes (CommittedDurable vs
  // CommittedDurabilityUncertain), exactly like the POSIX
  // renameat()-then-fsync(directory) split gives it below.
  if (!::MoveFileExW(temporary_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    ::DeleteFileW(temporary_path.c_str());
    result.error = PairingStoreError::Io;
    return result;
  }

  // Phase 2: durably commit that rename. Windows has no API that fsyncs a
  // directory by path; FlushFileBuffers on a directory HANDLE (opened here
  // with FILE_FLAG_BACKUP_SEMANTICS and GENERIC_WRITE, which
  // FlushFileBuffers requires) is the closest analogue, and is the
  // documented, NTFS-specific technique this store relies on to give the
  // rename's directory-entry update the same durability guarantee
  // fsync(dirfd) gives POSIX. A dedicated handle is opened for this rather
  // than reusing an already-validated one, because none of this function's
  // earlier handles were opened with GENERIC_WRITE (see the
  // FILE_LIST_DIRECTORY comment in open_secure_directory).
  //
  // This is the fail point's injection point:
  // PairingStoreFailPoint::AfterRenameBeforeDirectorySync short-circuits
  // this step exactly like the POSIX
  // `fail_point_ == AfterRenameBeforeDirectorySync || ::fsync(...) != 0`
  // does -- the rename above has already happened (the new content is
  // live and must be treated as committed) but this step's failure means
  // its durability across a crash is unconfirmed.
  bool flushed = false;
  if (!simulate_directory_sync_failure) {
    WidePath directory_path{};
    if (build_extended_path(directory, {}, directory_path)) {
      HANDLE flush_handle =
          ::CreateFileW(directory_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS, nullptr);
      if (flush_handle != INVALID_HANDLE_VALUE) {
        flushed = ::FlushFileBuffers(flush_handle) != 0;
        ::CloseHandle(flush_handle);
      }
    }
  }
  if (!flushed) {
    result.error = PairingStoreError::Io;
    result.commit = PairingCommitState::CommittedDurabilityUncertain;
    return result;
  }
  result.commit = PairingCommitState::CommittedDurable;
  return result;
}

}  // namespace noisefactor::sync::fs
