#pragma once

#include "update_policy.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <string>
#include <string_view>

namespace noisefactor::sync::windows_update {
// Verifies the Windows trust chain and the pinned signing certificate. No
// executable is launched, including on a successful verification.
bool verify_authenticode(const std::wstring& path, std::string_view publisher_sha256);

// Read back WinSparkle 0.9.4's exact persisted false value before its scheduler
// is initialized. Missing, malformed and unreadable settings fail closed.
bool automatic_checks_disabled_in_registry(const std::wstring& registry_path);

class UpdateAdapter {
 public:
  UpdateAdapter();
  ~UpdateAdapter();
  UpdateAdapter(const UpdateAdapter&) = delete;
  UpdateAdapter& operator=(const UpdateAdapter&) = delete;

  void initialize(std::wstring_view version);
  void cleanup();
  void check_for_updates(HWND owner);
  void poll();
  bool available() const;
  bool automatic_checks_available() const;
  bool automatic_checks_enabled() const;
  void set_automatic_checks_enabled(bool enabled);
  bool paused() const;
  void pause_for_24_hours();
  void pause_indefinitely();
  void resume_checks();
  std::wstring status() const;
  std::wstring last_check_status() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace noisefactor::sync::windows_update
