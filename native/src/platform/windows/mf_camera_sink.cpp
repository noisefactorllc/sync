#include <sync/platform/mf_camera_sink.hpp>

#include <windows.h>

#include <winternl.h>

#include <string>

#include <sync/camera/frame_ring.hpp>
#include <sync/platform/camera_identity.hpp>

namespace noisefactor::sync::camera {

struct MfCameraSink::Impl {
  HANDLE section = nullptr;
  HANDLE frame_event = nullptr;
  void* view = nullptr;
  std::unique_ptr<FrameRingWriter> writer;
  CameraSinkUnavailableReason reason = CameraSinkUnavailableReason::SectionMissing;
  std::int32_t status = 0;

  explicit Impl(const MfCameraSink::Options& options) {
    if (!windows_supports_virtual_cameras()) {
      reason = CameraSinkUnavailableReason::NotSupported;
      return;
    }

    section = ::OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, options.section.c_str());
    if (section == nullptr) {
      const DWORD error = ::GetLastError();
      // Not found means the source exists but no consumer has activated it, so
      // it has not created its section. Access denied is a DACL problem and a
      // bug; the two need different words, so they are different reasons.
      reason = error == ERROR_ACCESS_DENIED ? CameraSinkUnavailableReason::SectionAccessDenied
                                            : CameraSinkUnavailableReason::SectionMissing;
      status = static_cast<std::int32_t>(HRESULT_FROM_WIN32(error));
      return;
    }

    view = ::MapViewOfFile(section, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, frame_ring_bytes());
    if (view == nullptr) {
      const DWORD error = ::GetLastError();
      reason = error == ERROR_ACCESS_DENIED ? CameraSinkUnavailableReason::SectionAccessDenied
                                            : CameraSinkUnavailableReason::SectionMissing;
      status = static_cast<std::int32_t>(HRESULT_FROM_WIN32(error));
      ::CloseHandle(section);
      section = nullptr;
      return;
    }

    writer = std::make_unique<FrameRingWriter>(
        std::span<std::byte>(static_cast<std::byte*>(view), frame_ring_bytes()));
    if (!writer->valid()) {
      reason = CameraSinkUnavailableReason::SectionMissing;
      return;
    }

    // Opened, not created: the source owns this event, and a missing one is
    // not fatal -- the reader polls as well as waits.
    frame_event = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, options.frame_event.c_str());
    reason = CameraSinkUnavailableReason::None;
  }

  ~Impl() {
    writer.reset();
    if (view != nullptr) ::UnmapViewOfFile(view);
    if (section != nullptr) ::CloseHandle(section);
    if (frame_event != nullptr) ::CloseHandle(frame_event);
  }
};

MfCameraSink::MfCameraSink() : MfCameraSink(Options{}) {}

MfCameraSink::MfCameraSink(Options options)
    : impl_(std::make_unique<Impl>(options)) {}

MfCameraSink::~MfCameraSink() = default;

auto MfCameraSink::available() const noexcept -> bool {
  return impl_->reason == CameraSinkUnavailableReason::None;
}

auto MfCameraSink::unavailable_reason() const noexcept -> CameraSinkUnavailableReason {
  return impl_->reason;
}

auto MfCameraSink::unavailable_status() const noexcept -> std::int32_t { return impl_->status; }

auto MfCameraSink::has_capacity() const noexcept -> bool {
  return impl_->writer != nullptr && impl_->writer->has_capacity();
}

auto MfCameraSink::submit(const CameraSinkFrame& frame) noexcept -> CameraSinkSubmit {
  if (!available() || impl_->writer == nullptr) return CameraSinkSubmit::Failed;
  if (frame.width != kCanvas.width || frame.height != kCanvas.height) {
    return CameraSinkSubmit::Failed;
  }
  if (!impl_->writer->write(frame.bgra, frame.row_stride, frame.presentation_time_us)) {
    return CameraSinkSubmit::Failed;
  }
  if (impl_->frame_event != nullptr) ::SetEvent(impl_->frame_event);
  return CameraSinkSubmit::Accepted;
}

auto windows_supports_virtual_cameras() noexcept -> bool {
  // Not VerifyVersionInfo or GetVersionEx: both go through the compatibility
  // shim, which reports 6.2 to any binary without a manifest declaring
  // Windows 10 support. A build check through either says "older than 22000"
  // on Windows 11 itself, which would disable the camera everywhere.
  // RtlGetVersion is not shimmed.
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) return false;
  const auto rtl_get_version =
      reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
  if (rtl_get_version == nullptr) return false;
  RTL_OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (rtl_get_version(&info) != 0) return false;
  return info.dwBuildNumber >= 22000;
}

}  // namespace noisefactor::sync::camera
