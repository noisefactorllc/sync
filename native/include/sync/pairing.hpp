#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

#include <sync/origin.hpp>
#include <sync/pairing_store.hpp>
#include <sync/secure_memory.hpp>

namespace noisefactor::sync::pairing {

inline constexpr std::size_t kMaximumPairingNameBytes = 64;
inline constexpr std::size_t kMaximumPairingVersions = 8;
inline constexpr std::size_t kMaximumPairingMessageBytes = 1024;

enum class ParseError {
  None,
  Malformed,
  DuplicateField,
  UnknownField,
  MissingField,
  InvalidType,
  InvalidValue,
  UnsupportedVersion
};

struct ParseResult;
class Parser;

class PairRequest {
public:
  [[nodiscard]] std::string_view name() const noexcept {
    return {name_.data(), name_length_};
  }
  [[nodiscard]] bool supports(std::uint16_t version) const noexcept;

private:
  friend class Parser;
  std::array<char, kMaximumPairingNameBytes> name_{};
  std::size_t name_length_ = 0;
  std::array<std::uint16_t, kMaximumPairingVersions> versions_{};
  std::size_t version_count_ = 0;
};

struct ParseResult {
  ParseError error = ParseError::Malformed;
  PairRequest request{};
  [[nodiscard]] bool ok() const noexcept { return error == ParseError::None; }
};

[[nodiscard]] ParseResult parse_request(std::string_view input);
[[nodiscard]] std::string encode_paired(std::uint16_t protocol_version,
                                        std::string_view token);
[[nodiscard]] std::string encode_error(std::string_view code,
                                       std::string_view message);

enum class PromptDecision { Approved, Denied, TimedOut };

struct PromptRequest {
  std::uint64_t generation = 0;
  NormalizedOrigin origin{};
  [[nodiscard]] bool assign(std::uint64_t value,
                            const NormalizedOrigin &request_origin,
                            std::string_view request_name) noexcept;
  [[nodiscard]] std::string_view name() const noexcept {
    return {name_.data(), name_length_};
  }

private:
  std::array<char, kMaximumPairingNameBytes> name_{};
  std::size_t name_length_ = 0;
};

struct PromptResult {
  bool available = false;
  std::uint64_t generation = 0;
  PromptDecision decision = PromptDecision::Denied;
};

class PairingPrompt {
public:
  virtual ~PairingPrompt() = default;
  [[nodiscard]] virtual bool begin(const PromptRequest &request) noexcept = 0;
  [[nodiscard]] virtual PromptResult poll() noexcept = 0;
  virtual void cancel(std::uint64_t generation) noexcept = 0;
};

class PairingAuthority {
public:
  virtual ~PairingAuthority() = default;
  [[nodiscard]] virtual PairingIssueResult
  issue(const NormalizedOrigin &origin, PairingCommitGate &gate) noexcept = 0;
  [[nodiscard]] virtual PairingAuthenticationResult
  authenticate(const NormalizedOrigin &origin,
               std::string_view token) noexcept = 0;
};

class PairingManagement {
public:
  virtual ~PairingManagement() = default;
  [[nodiscard]] virtual PairingListResult
  list(std::span<NormalizedOrigin> output) noexcept = 0;
  [[nodiscard]] virtual PairingRevocationResult
  revoke(const NormalizedOrigin &origin) noexcept = 0;
};

inline constexpr std::size_t kMaximumAuthorityRequests = 64;
inline constexpr std::size_t kMaximumAuthorityTokenBytes = 256;

enum class AuthorityOperation { Authenticate, Issue };

struct AuthorityResult {
  std::uint64_t generation = 0;
  AuthorityOperation operation = AuthorityOperation::Authenticate;
  PairingAuthenticationResult authentication{};
  PairingIssueResult issuance{};

  AuthorityResult() = default;
  AuthorityResult(const AuthorityResult &) = delete;
  AuthorityResult &operator=(const AuthorityResult &) = delete;
  AuthorityResult(AuthorityResult &&) noexcept = default;
  AuthorityResult &operator=(AuthorityResult &&) noexcept = default;
};

class AuthorityWorker {
public:
  // Invoked on the worker thread the moment a result becomes pollable, so the
  // owning event loop can collect it instead of waiting for its next sweep.
  // It runs while the worker holds its lock, so it must be non-blocking and
  // safe to call from another thread (uv_async_send is the intended use).
  using ResultNotifier = void (*)(void *) noexcept;

  explicit AuthorityWorker(PairingAuthority &authority,
                           CleanseObserver *cleanse_observer = nullptr);
  ~AuthorityWorker() noexcept;
  AuthorityWorker(const AuthorityWorker &) = delete;
  AuthorityWorker &operator=(const AuthorityWorker &) = delete;

  [[nodiscard]] bool submit_authenticate(
      std::uint64_t generation, const NormalizedOrigin &origin,
      std::string_view token) noexcept;
  [[nodiscard]] bool submit_issue(std::uint64_t generation,
                                  const NormalizedOrigin &origin) noexcept;
  [[nodiscard]] bool poll(AuthorityResult &result) noexcept;
  [[nodiscard]] bool has_result(std::uint64_t generation) noexcept;
  [[nodiscard]] bool cancel(std::uint64_t generation) noexcept;
  // Clearing the notifier is ordered against any concurrent delivery, so a
  // caller can retire the target before tearing it down.
  void set_result_notifier(ResultNotifier notifier, void *context) noexcept;
  void shutdown() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class StorePairingAuthority final : public PairingAuthority,
                                    public PairingManagement {
public:
  explicit StorePairingAuthority(PairingStore &store) noexcept
      : store_(store) {}
  [[nodiscard]] PairingIssueResult
  issue(const NormalizedOrigin &origin, PairingCommitGate &gate) noexcept override;
  [[nodiscard]] PairingAuthenticationResult
  authenticate(const NormalizedOrigin &origin,
               std::string_view token) noexcept override;
  [[nodiscard]] PairingListResult
  list(std::span<NormalizedOrigin> output) noexcept override;
  [[nodiscard]] PairingRevocationResult
  revoke(const NormalizedOrigin &origin) noexcept override;

private:
  PairingStore &store_;
  std::mutex store_mutex_;
};

} // namespace noisefactor::sync::pairing
