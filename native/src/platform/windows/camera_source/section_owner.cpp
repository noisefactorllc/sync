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
  if (event_ != nullptr) ::CloseHandle(event_);
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
  const std::wstring event = frame_event_name();
  bytes_ = frame_ring_bytes();
  section_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0,
                                  static_cast<DWORD>(bytes_), section.c_str());
  if (section_ != nullptr) {
    event_ = ::CreateEventW(&attributes, FALSE, FALSE, event.c_str());
    view_ = ::MapViewOfFile(section_, FILE_MAP_READ, 0, 0, bytes_);
  }
  ::LocalFree(descriptor);

  if (view_ == nullptr) {
    if (section_ != nullptr) {
      ::CloseHandle(section_);
      section_ = nullptr;
    }
    if (event_ != nullptr) {
      ::CloseHandle(event_);
      event_ = nullptr;
    }
    return false;
  }
  return true;
}

auto SectionOwner::mapping() const noexcept -> std::span<const std::byte> {
  if (view_ == nullptr) return {};
  return {static_cast<const std::byte*>(view_), bytes_};
}

}  // namespace noisefactor::sync::camera
