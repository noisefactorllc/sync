#pragma once

#include <windows.h>

#include <cstddef>
#include <span>

namespace noisefactor::sync::camera {

// Creates the shared ring. This runs inside the frame server, in session 0,
// which is what makes it possible at all: creating a Global object needs
// SeCreateGlobalPrivilege, which the service has and syncd -- unelevated, in
// the user's session -- does not. So the source creates and syncd opens,
// the inverse of macOS where the extension hands its queue to the daemon.
//
// The DACL grants INTERACTIVE write access and both application package SIDs
// read access, so the logged-in user's syncd can feed the ring and
// AppContainer and LPAC consumers can read from it.
class SectionOwner {
 public:
  SectionOwner() = default;
  ~SectionOwner();

  SectionOwner(const SectionOwner&) = delete;
  auto operator=(const SectionOwner&) -> SectionOwner& = delete;
  SectionOwner(SectionOwner&&) = delete;
  auto operator=(SectionOwner&&) -> SectionOwner& = delete;

  // Idempotent: opens the section when it already exists.
  [[nodiscard]] auto open() noexcept -> bool;
  [[nodiscard]] auto mapping() const noexcept -> std::span<const std::byte>;

 private:
  HANDLE section_ = nullptr;
  void* view_ = nullptr;
  std::size_t bytes_ = 0;
};

}  // namespace noisefactor::sync::camera
