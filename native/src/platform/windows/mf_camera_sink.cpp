#include <sync/platform/mf_camera_sink.hpp>

#include <windows.h>

#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <winternl.h>
#include <wrl/client.h>

#include <string>

#include <sync/camera/frame_ring.hpp>
#include <sync/platform/camera_identity.hpp>

#include "camera_source/source_guids.hpp"

namespace noisefactor::sync::camera {

namespace {

using Microsoft::WRL::ComPtr;

// The frame server resolves the source through HKLM, so its absence there is
// exactly the condition the tray's Enable Sync Camera line fixes.
[[nodiscard]] auto source_is_registered() -> bool {
  const std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") +
                           kSyncCameraSourceClsidString + L"\\InprocServer32";
  HKEY handle = nullptr;
  if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) {
    return false;
  }
  ::RegCloseKey(handle);
  return true;
}

}  // namespace

struct MfCameraSink::Impl {
  MfCameraSink::Options options;
  bool media_foundation_started = false;
  ComPtr<IMFVirtualCamera> camera;
  HANDLE section = nullptr;
  void* view = nullptr;
  std::unique_ptr<FrameRingWriter> writer;
  CameraSinkUnavailableReason reason = CameraSinkUnavailableReason::None;
  std::int32_t status = 0;

  explicit Impl(MfCameraSink::Options given) : options(std::move(given)) {
    if (!windows_supports_virtual_cameras()) {
      reason = CameraSinkUnavailableReason::NotSupported;
      return;
    }
    if (!options.create_virtual_camera) return;

    if (!source_is_registered()) {
      reason = CameraSinkUnavailableReason::SourceNotRegistered;
      return;
    }
    if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
      reason = CameraSinkUnavailableReason::VirtualCameraRefused;
      return;
    }
    media_foundation_started = true;

    // Lifetime_System so the camera survives a reboot and keeps showing the
    // waiting card whether or not syncd is running, which is how the macOS
    // camera behaves. Access_CurrentUser needs no administrator; AllUsers
    // would.
    const HRESULT created = ::MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
        MFVirtualCameraAccess_CurrentUser, kSyncCameraDisplayName,
        kSyncCameraSourceClsidString, nullptr, 0, &camera);
    if (FAILED(created) || !camera) {
      reason = CameraSinkUnavailableReason::VirtualCameraRefused;
      status = static_cast<std::int32_t>(created);
      return;
    }
    const HRESULT started = camera->Start(nullptr);
    if (FAILED(started)) {
      reason = CameraSinkUnavailableReason::VirtualCameraRefused;
      status = static_cast<std::int32_t>(started);
      camera.Reset();
    }
  }

  ~Impl() {
    close_section();
    if (camera) camera->Shutdown();
    camera.Reset();
    if (media_foundation_started) ::MFShutdown();
  }

  void close_section() {
    writer.reset();
    if (view != nullptr) {
      ::UnmapViewOfFile(view);
      view = nullptr;
    }
    if (section != nullptr) {
      ::CloseHandle(section);
      section = nullptr;
    }
  }

  // The section exists only while the media source has been activated, so
  // this is tried on every frame rather than once at construction: a consumer
  // can arrive and leave many times over one run of the daemon.
  //
  // noexcept because both callers are: an allocation failure here has to read
  // as backpressure, not as std::terminate taking the daemon down.
  [[nodiscard]] auto ensure_section() noexcept -> bool try {
    if (writer != nullptr) return true;
    section = ::OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, options.section.c_str());
    if (section == nullptr) {
      note_open_failure(::GetLastError());
      return false;
    }
    view = ::MapViewOfFile(section, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, frame_ring_bytes());
    if (view == nullptr) {
      note_open_failure(::GetLastError());
      close_section();
      return false;
    }
    writer = std::make_unique<FrameRingWriter>(
        std::span<std::byte>(static_cast<std::byte*>(view), frame_ring_bytes()));
    if (!writer->valid()) {
      close_section();
      return false;
    }
    return true;
  } catch (...) {
    close_section();
    return false;
  }

  // A section that is simply not there yet is the ordinary idle state and
  // says nothing. A section that refuses this account is a DACL problem the
  // user cannot guess at, so it is recorded where the diagnostics can see it.
  void note_open_failure(DWORD error) noexcept {
    if (error != ERROR_ACCESS_DENIED) return;
    reason = CameraSinkUnavailableReason::SectionAccessDenied;
    status = static_cast<std::int32_t>(HRESULT_FROM_WIN32(error));
  }
};

MfCameraSink::MfCameraSink() : MfCameraSink(Options{}) {}

MfCameraSink::MfCameraSink(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}

MfCameraSink::~MfCameraSink() = default;

auto MfCameraSink::available() const noexcept -> bool {
  return impl_->reason == CameraSinkUnavailableReason::None;
}

auto MfCameraSink::unavailable_reason() const noexcept -> CameraSinkUnavailableReason {
  return impl_->reason;
}

auto MfCameraSink::unavailable_status() const noexcept -> std::int32_t { return impl_->status; }

auto MfCameraSink::has_capacity() const noexcept -> bool {
  // Nowhere to put a frame, or nobody asking for one. Answering no here is
  // what stops the publisher fitting a 1080p frame sixty times a second that
  // nothing would read.
  //
  // The section alone is not the test. This process keeps its own view mapped,
  // which keeps the named section alive after every consumer has gone, so
  // "the section opens" stays true forever once anything has ever opened the
  // camera. Whether the media source has recently asked for a frame is the
  // question that actually has an answer.
  if (!available() || !impl_->ensure_section()) return false;
  return impl_->writer->has_demand(camera_clock_us());
}

auto MfCameraSink::submit(const CameraSinkFrame& frame) noexcept -> CameraSinkSubmit {
  if (!available()) return CameraSinkSubmit::Failed;
  if (!impl_->ensure_section()) return CameraSinkSubmit::Backpressured;
  if (frame.width != kCanvas.width || frame.height != kCanvas.height) {
    return CameraSinkSubmit::Failed;
  }
  if (!impl_->writer->has_demand(camera_clock_us())) return CameraSinkSubmit::Backpressured;
  if (!impl_->writer->write(frame.bgra, frame.row_stride, frame.presentation_time_us)) {
    return CameraSinkSubmit::Failed;
  }
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
