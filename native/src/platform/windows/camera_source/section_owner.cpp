#include "section_owner.hpp"

#include <sddl.h>

#include <string>

#include <sync/camera/frame_ring.hpp>

namespace noisefactor::sync::camera {

namespace {

// DACL, in SDDL:
//   GA to SYSTEM (SY) and Local Service (LS) -- the frame server itself, which
//     is what hosts this code
//   GA to INTERACTIVE (IU) -- the logged-in user's syncd, which writes frames
//   GR+GX to ALL APPLICATION PACKAGES (AC) and ALL RESTRICTED APPLICATION
//     PACKAGES (RC) -- Store and LPAC consumers, which read them
//
// INTERACTIVE rather than a specific account because the source cannot be told
// which user to pair with: IMFVirtualCamera has no SetProperty, and both
// AddProperty and AddRegistryEntry require administrator permissions that
// unelevated syncd does not have.
constexpr wchar_t kSectionSddl[] =
    L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;IU)(A;;GRGX;;;AC)(A;;GRGX;;;RC)";

}  // namespace

SectionOwner::~SectionOwner() {
  if (view_ != nullptr) ::UnmapViewOfFile(view_);
  if (section_ != nullptr) ::CloseHandle(section_);
}

auto SectionOwner::open() noexcept -> bool {
  if (view_ != nullptr) return true;

  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(kSectionSddl, SDDL_REVISION_1,
                                                             &descriptor, nullptr) == FALSE) {
    return false;
  }
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = descriptor;
  attributes.bInheritHandle = FALSE;

  const std::wstring section = section_name();
  bytes_ = frame_ring_bytes();
  section_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0,
                                  static_cast<DWORD>(bytes_), section.c_str());
  if (section_ != nullptr) {
    // Read/write, not read-only: frames travel one way, but the source stamps
    // the demand heartbeat in the header so the sender on the other side can
    // tell whether anything is still watching.
    view_ = ::MapViewOfFile(section_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, bytes_);
  }
  ::LocalFree(descriptor);

  if (view_ == nullptr) {
    if (section_ != nullptr) {
      ::CloseHandle(section_);
      section_ = nullptr;
    }
    return false;
  }

  // Stamp the ring here, at creation, rather than leaving it to whichever
  // side writes first. The source has to record demand before any frame
  // exists -- that is the whole point of the heartbeat -- and a reader only
  // trusts a stamped mapping. Left to the sender, the two would deadlock
  // politely: no demand until a frame arrives, no frame until demand does.
  // FrameRingWriter adopts an already-stamped ring, so this is idempotent.
  (void)FrameRingWriter(std::span<std::byte>(static_cast<std::byte*>(view_), bytes_));
  return true;
}

auto SectionOwner::mapping() const noexcept -> std::span<const std::byte> {
  if (view_ == nullptr) return {};
  return {static_cast<const std::byte*>(view_), bytes_};
}

}  // namespace noisefactor::sync::camera
