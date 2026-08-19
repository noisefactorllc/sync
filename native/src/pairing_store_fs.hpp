#pragma once

// Internal filesystem seam for PairingStore.
//
// This is NOT a public header -- it is kept out of native/include/ on
// purpose. native/src/pairing_store.cpp contains every bit of
// platform-neutral PairingStore logic (record parsing, versioning, capacity
// limits, token/digest handling, commit-gate interaction, cleansing) and
// never touches a filesystem primitive directly. The primitives that give
// PairingStore its actual guarantees -- owner-only access, refusal to
// follow a symlink/reparse point, bounded paths, and durable atomic commit
// -- live entirely in exactly one of:
//   native/src/platform/posix/pairing_store_fs.cpp
//   native/src/platform/windows/pairing_store_fs.cpp
// Both translation units implement precisely the surface declared below, so
// pairing_store.cpp contains no `#if defined(_WIN32)` of its own for
// filesystem behaviour.
//
// Every function below independently resolves and validates the directory
// it is given (via open_secure_directory, or the platform's equivalent
// walk) rather than accepting an already-open directory handle from a
// sibling call. This mirrors what acquire_store_lock already did before
// this refactor -- it has always re-resolved its directory rather than
// reusing a handle threaded in from PairingStore::open() -- so every entry
// point below now follows that same, already-proven-safe pattern. The
// extra directory open this costs on some call paths is the same walk
// acquire_store_lock already performs on nearly every PairingStore
// operation; it buys a uniform, easy-to-review seam that is trivial to keep
// identical in shape across two platform implementations.

#include <sync/pairing_store.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace noisefactor::sync::fs {

// Owner-only, symlink/reparse-refusing handle to an open directory or file.
// Move-only; closes on destruction, mirroring pairing_store.cpp's own
// ScopedFd. The representation (a POSIX fd vs. a Win32 HANDLE) is private to
// whichever platform .cpp constructs an instance; pairing_store.cpp only
// ever moves instances around and asks whether one is valid -- it never
// reaches into the native value.
class ScopedHandle {
 public:
  ScopedHandle() noexcept;
  ~ScopedHandle() noexcept;
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ScopedHandle(ScopedHandle&& other) noexcept;
  ScopedHandle& operator=(ScopedHandle&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept;
  void close() noexcept;

#if defined(_WIN32)
  explicit ScopedHandle(void* native) noexcept;
  [[nodiscard]] void* native() const noexcept;

 private:
  void* value_;
#else
  explicit ScopedHandle(int native) noexcept;
  [[nodiscard]] int native() const noexcept;

 private:
  int value_;
#endif
};

// Maximum bytes in a single '/'- or '\'-separated path component this seam
// will accept. POSIX's NAME_MAX is 255 on every target this ships for
// (Linux, macOS); Windows' long-filename component limit is likewise 255
// UTF-16 code units. Sharing one constant keeps
// pairing_store.cpp's path_is_bounded_absolute (platform-neutral) and both
// platform files' component walks bounded identically without either side
// needing the other's platform headers.
inline constexpr std::size_t kMaximumPathComponentBytes = 255;

// Opens `directory` (an absolute path already validated by
// path_is_bounded_absolute), refusing to traverse through any
// symlink/reparse point anywhere along it. If `create`, creates any missing
// ancestor -- and the directory itself -- with owner-only permissions
// applied at creation time. On success `output` holds an open handle to the
// final directory; callers that only need the validation/creation side
// effect (PairingStore::open()) may let it drop immediately.
[[nodiscard]] PairingStoreError open_secure_directory(std::string_view directory,
                                                       ScopedHandle& output,
                                                       bool create) noexcept;

// Creates (if absent) and locks the advisory ".pairings.lock" file inside
// `directory`, refusing a symlink/reparse point and requiring an
// owner-only, single-link regular file. Retries briefly on contention
// before returning PairingStoreError::Busy, exactly like the POSIX flock
// retry loop this replaces on Windows.
[[nodiscard]] PairingStoreError acquire_store_lock(std::string_view directory,
                                                    ScopedHandle& lock) noexcept;

struct LoadResult {
  PairingStoreError error = PairingStoreError::None;
  bool exists = false;
  std::size_t size = 0;
};

// Loads `filename` (a single path component, no separators) out of
// `directory` into `buffer`, refusing a symlink/reparse point and requiring
// an owner-only, single-link regular file whose size is in
// (0, buffer.size()] bytes with nothing past that size. `exists == false`
// (with error == None) means no such file exists yet -- callers treat that
// as an empty store, not an error.
[[nodiscard]] LoadResult load_secure_regular_file(std::string_view directory,
                                                   std::string_view filename,
                                                   std::span<unsigned char> buffer) noexcept;

// Creates `filename` inside `directory` exclusively (fails if it already
// exists), applies owner-only permissions, writes `bytes` in full, and
// durably flushes the new file's own content before returning. On any
// failure the partially-written file is removed before returning an error.
[[nodiscard]] PairingStoreError write_new_secure_regular_file(
    std::string_view directory, std::string_view filename,
    std::span<const unsigned char> bytes) noexcept;

// Best-effort removal of `filename` inside `directory`. Failures are
// intentionally ignored -- every caller uses this only to clean up a
// temporary file on a path that is already returning a different error, so
// a failed cleanup must never itself become a fresh, more confusing error.
void remove_file(std::string_view directory, std::string_view filename) noexcept;

struct CommitResult {
  PairingStoreError error = PairingStoreError::None;
  PairingCommitState commit = PairingCommitState::NotCommitted;
};

// Atomically renames `temporary_filename` over `final_filename`, both
// inside `directory`, then durably commits that rename to storage.
// `simulate_directory_sync_failure` carries PairingStoreFailPoint::
// AfterRenameBeforeDirectorySync in from pairing_store.cpp: when set, the
// post-rename durability step is treated as having failed without even
// being attempted, exactly mirroring the POSIX fail point's short-circuit
// (`fail_point_ == AfterRenameBeforeDirectorySync || ::fsync(...) != 0`).
//
// - Rename failure: the temporary file is removed and
//   {Io, NotCommitted} is returned -- nothing was committed.
// - Rename success and the durability step also succeeds:
//   {None, CommittedDurable}.
// - Rename success but the durability step fails or is simulated as
//   failed: {Io, CommittedDurabilityUncertain} -- the rename is visible
//   (the new content IS live and must be treated as committed) but its
//   durability across a crash is not guaranteed.
[[nodiscard]] CommitResult rename_into_place(std::string_view directory,
                                              std::string_view temporary_filename,
                                              std::string_view final_filename,
                                              bool simulate_directory_sync_failure) noexcept;

}  // namespace noisefactor::sync::fs
