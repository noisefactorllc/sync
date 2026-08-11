#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <string_view>

#include <sync/origin.hpp>

namespace noisefactor::sync {

inline constexpr std::size_t kMaximumPairingOrigins = 64;
inline constexpr std::size_t kPairingTokenBytes = 32;
inline constexpr std::size_t kPairingTokenHexBytes = 64;
inline constexpr std::size_t kMaximumPairingStorePathBytes = 1024;

enum class PairingStoreError {
  None,
  InvalidPath,
  DirectorySecurity,
  FileSecurity,
  Io,
  Corrupt,
  UnknownVersion,
  Capacity,
  RandomFailure,
  InvalidToken,
  Busy,
  Canceled,
};

class PairingCommitGate {
 public:
  PairingCommitGate() noexcept = default;
  PairingCommitGate(const PairingCommitGate&) = delete;
  PairingCommitGate& operator=(const PairingCommitGate&) = delete;

  [[nodiscard]] bool cancel() noexcept;
  [[nodiscard]] bool try_begin_commit() noexcept;
  [[nodiscard]] bool canceled() const noexcept;

 private:
  enum class State : unsigned char { Open, Canceled, CommitClaimed };
  std::atomic<State> state_{State::Open};
};

class PairingStoreCommitHook {
 public:
  // Caller-owned test seam; it must outlive any PairingStore opened with it.
  virtual ~PairingStoreCommitHook() = default;
  virtual void before_commit() noexcept = 0;
};

enum class PairingStoreFailPoint {
  None,
  BeforeRename,
  AfterRenameBeforeDirectorySync,
};

enum class PairingCommitState {
  NotCommitted,
  CommittedDurable,
  CommittedDurabilityUncertain,
};

struct PairingStoreOptions {
  std::string_view path;
  PairingStoreFailPoint fail_point = PairingStoreFailPoint::None;
  PairingStoreCommitHook* commit_hook = nullptr;
};

class PairingToken {
 public:
  PairingToken() = default;
  ~PairingToken() noexcept;
  PairingToken(const PairingToken&) = delete;
  PairingToken& operator=(const PairingToken&) = delete;
  PairingToken(PairingToken&& other) noexcept;
  PairingToken& operator=(PairingToken&& other) noexcept;
  [[nodiscard]] std::string_view view() const noexcept { return {bytes_.data(), length_}; }

 private:
  friend class PairingStore;
  void clear() noexcept;
  std::array<char, kPairingTokenHexBytes> bytes_{};
  std::size_t length_ = 0;
};

struct PairingIssueResult {
  PairingStoreError error = PairingStoreError::Io;
  PairingToken token{};
  bool replaced = false;
  PairingCommitState commit = PairingCommitState::NotCommitted;
};

struct PairingAuthenticationResult {
  PairingStoreError error = PairingStoreError::Io;
  bool authenticated = false;
};

struct PairingRevocationResult {
  PairingStoreError error = PairingStoreError::Io;
  bool revoked = false;
  PairingCommitState commit = PairingCommitState::NotCommitted;
};

struct PairingListResult {
  PairingStoreError error = PairingStoreError::Io;
  std::size_t count = 0;
};

class PairingStore {
 public:
  PairingStore() = default;
  ~PairingStore() noexcept;
  PairingStore(const PairingStore&) = delete;
  PairingStore& operator=(const PairingStore&) = delete;
  PairingStore(PairingStore&& other) noexcept;
  PairingStore& operator=(PairingStore&& other) noexcept;

  [[nodiscard]] PairingStoreError open(PairingStoreOptions options) noexcept;
  [[nodiscard]] PairingIssueResult issue(const NormalizedOrigin& origin) noexcept;
  [[nodiscard]] PairingIssueResult issue(const NormalizedOrigin& origin,
                                         PairingCommitGate& gate) noexcept;
  [[nodiscard]] PairingAuthenticationResult authenticate(
      const NormalizedOrigin& origin, std::string_view token) noexcept;
  [[nodiscard]] PairingRevocationResult revoke(const NormalizedOrigin& origin) noexcept;
  [[nodiscard]] PairingListResult list(std::span<NormalizedOrigin> output) noexcept;

 private:
  struct Record {
    NormalizedOrigin origin{};
    std::array<unsigned char, 32> digest{};
  };
  struct PersistResult {
    PairingStoreError error = PairingStoreError::Io;
    PairingCommitState commit = PairingCommitState::NotCommitted;
  };

  [[nodiscard]] PairingStoreError reload() noexcept;
  [[nodiscard]] PersistResult persist(
      const std::array<Record, kMaximumPairingOrigins>& records,
      std::size_t count, PairingCommitGate* gate = nullptr) noexcept;
  void clear() noexcept;

  std::array<char, kMaximumPairingStorePathBytes> path_{};
  std::size_t path_length_ = 0;
  std::array<Record, kMaximumPairingOrigins> records_{};
  std::size_t record_count_ = 0;
  PairingStoreFailPoint fail_point_ = PairingStoreFailPoint::None;
  PairingStoreCommitHook* commit_hook_ = nullptr;
  bool opened_ = false;
};

[[nodiscard]] PairingStoreError default_pairing_store_path(
    std::span<char> output, std::size_t& length) noexcept;

}  // namespace noisefactor::sync
