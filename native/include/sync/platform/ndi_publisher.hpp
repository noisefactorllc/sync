#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <sync/frame_receiver.hpp>

namespace noisefactor::sync {

enum class NdiUnavailableReason : std::uint8_t {
  None,
  RuntimeNotFound,
  LoadFailed,
  EntryPointMissing,
  InitializationFailed,
  UnsupportedCpu,
  // macOS: the runtime file does not carry NewTek's Developer ID signature.
  SignatureRejected,
};

[[nodiscard]] auto describe(NdiUnavailableReason reason) noexcept
    -> const char*;

// Direct CPU-frame send provider for NDI. Available on every desktop
// platform Sync targets; unlike MetalFramePublisher this class carries no
// platform #error guard because the NDI runtime is discovered dynamically
// (see ndi_publisher.cpp) rather than linked, so the same translation unit
// builds on Windows, macOS, and Linux.
class NdiFramePublisher final : public FramePublisher {
 public:
  static constexpr std::size_t kMaximumSenderEntries = 8;
  static constexpr std::size_t kProductAllocationBudgetBytes = 512ULL * 1024ULL * 1024ULL;

  struct Options {
    // Directory to search first. Wired from the --ndi-runtime CLI flag by
    // the caller. On Windows and Linux it comes ahead of the vendor's
    // documented NDI_RUNTIME_DIR_V6 / NDI_RUNTIME_DIR_V5 environment
    // variables and the OS loader's default search; on macOS it comes ahead
    // of the vendor's install path, /usr/local/lib/libndi.dylib. Empty
    // means "search only those".
    std::string_view runtime_path{};
    std::size_t allocation_budget_bytes = kProductAllocationBudgetBytes;
    // macOS only: the code requirement every candidate must satisfy before
    // it is loaded. Empty selects the NewTek Team ID pin, which is what
    // syncd always uses; the tests set it so that their stub runtime loads.
    std::string_view signature_requirement{};
  };

  NdiFramePublisher();
  explicit NdiFramePublisher(Options options);
  ~NdiFramePublisher() override;

  NdiFramePublisher(const NdiFramePublisher&) = delete;
  auto operator=(const NdiFramePublisher&) -> NdiFramePublisher& = delete;
  NdiFramePublisher(NdiFramePublisher&&) = delete;
  auto operator=(NdiFramePublisher&&) -> NdiFramePublisher& = delete;

  // False whenever the NDI runtime could not be found, failed its ABI
  // probe, or reported an unsupported CPU. Absence is never an error: a
  // caller simply does not offer this provider when available() is false.
  [[nodiscard]] auto available() const noexcept -> bool;
  [[nodiscard]] auto unavailable_reason() const noexcept
      -> NdiUnavailableReason;

  auto open_sender(std::string_view sender_id, std::string_view name) noexcept -> bool override;
  void close_sender(std::string_view sender_id) noexcept override;
  auto publish(std::string_view sender_id, const protocol::FrameView& frame) noexcept
      -> PublishResult override;
  auto poll_failure(std::uint64_t now_ms) noexcept
      -> std::optional<ProviderFailure> override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace noisefactor::sync
