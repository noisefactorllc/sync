#include "test_harness.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <membership.h>
#include <sys/acl.h>
#endif

#include <sync/origin.hpp>
#include <sync/pairing_store.hpp>

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
    std::array<char, 256> pattern{};
    const std::string seed = "/private/tmp/sync-pairing-test-XXXXXX";
    std::copy(seed.begin(), seed.end(), pattern.begin());
    char* made = ::mkdtemp(pattern.data());
    SYNC_REQUIRE(made != nullptr);
    path_ = made;
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

void write_bytes(const std::filesystem::path& path, std::span<const unsigned char> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  output.close();
  SYNC_REQUIRE(::chmod(path.c_str(), 0600) == 0);
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
  SYNC_REQUIRE(::chmod(path.c_str(), 0600) == 0);
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

SYNC_TEST(pairing_store_rejects_symlinked_directories_files_and_parent_traversal) {
  TempDirectory temporary;
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

  PairingStore traversal;
  SYNC_REQUIRE(traversal.open({.path = (real / ".." / "escape.bin").string()}) ==
               PairingStoreError::InvalidPath);
}

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

SYNC_TEST(pairing_store_validates_filename_plus_temporary_suffix_against_name_max) {
  TempDirectory temporary;
  constexpr std::size_t suffix_bytes = 21;
  const std::string accepted_name(NAME_MAX - suffix_bytes, 'a');
  PairingStore accepted;
  SYNC_REQUIRE(accepted.open({.path = (temporary.path() / accepted_name).string()}) ==
               PairingStoreError::None);

  const std::string rejected_name(NAME_MAX - suffix_bytes + 1, 'b');
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
