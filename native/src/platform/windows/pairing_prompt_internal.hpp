#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include <sync/platform/pairing_prompt.hpp>

// Test seam for WindowsPairingPrompt, mirroring
// native/src/platform/macos/pairing_prompt_internal.hpp: production code goes
// through a real Win32 adapter (Win32MessageBoxAdapter, defined in
// pairing_prompt.cpp) while tests inject a fake Adapter so no MessageBoxW
// ever has to appear -- and therefore be clicked by a human -- in CI.
namespace noisefactor::sync::platform::pairing_prompt_testing {

inline constexpr std::size_t kMaximumPromptTitleBytes = 64;
inline constexpr std::size_t kMaximumPromptMessageBytes = 900;

struct Presentation {
  [[nodiscard]] std::string_view title() const noexcept {
    return {title_bytes.data(), title_length};
  }
  [[nodiscard]] std::string_view message() const noexcept {
    return {message_bytes.data(), message_length};
  }

  std::array<char, kMaximumPromptTitleBytes> title_bytes{};
  std::size_t title_length = 0;
  std::array<char, kMaximumPromptMessageBytes> message_bytes{};
  std::size_t message_length = 0;
};

enum class AdapterResponse { Approved, Denied, Failed };

// Abstracts the modal Win32 MessageBoxW call.
//
// show() runs on the calling (worker) thread and blocks until the user
// responds, or until force_close() (called from another thread) makes the
// underlying modal loop return early. As soon as a native window exists,
// the real adapter must invoke report_window with its handle (packed into a
// std::uintptr_t so this header never has to include <windows.h>) so a
// concurrent cancel/deadline watcher can ask it to close.
//
// report_window may be invoked from a callback nested inside show() itself
// (the real adapter uses a WH_CBT hook that fires synchronously on the same
// thread), so it must be safe to call without re-entering any lock held by
// the caller of show().
class Adapter {
 public:
  virtual ~Adapter() = default;
  [[nodiscard]] virtual AdapterResponse show(
      const Presentation& presentation,
      const std::function<void(std::uintptr_t)>& report_window) = 0;

  // Best-effort request to close a window previously reported through
  // report_window. May be called from any thread, including while show() is
  // still running on another thread, and may be called with a handle whose
  // window has already closed by itself -- both must be safely ignorable.
  // This is NOT the correctness mechanism for a cancelled/timed-out prompt:
  // it only makes the UI go away promptly when it works. The actual
  // guarantee -- that a late decision for a stale generation is never
  // delivered -- comes from WindowsPairingPrompt::Impl's own bookkeeping and
  // holds even if force_close does nothing at all.
  virtual void force_close(std::uintptr_t window) = 0;
};

class Factory {
 public:
  [[nodiscard]] static std::unique_ptr<WindowsPairingPrompt> create(
      std::unique_ptr<Adapter> adapter,
      std::chrono::milliseconds ui_deadline);
};

}  // namespace noisefactor::sync::platform::pairing_prompt_testing
