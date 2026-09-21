#include <sync/platform/mf_camera_sink.hpp>

#include <windows.h>

#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <winternl.h>
#include <wrl/client.h>

#include <cstring>
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

  HANDLE shm_file = nullptr;
  HANDLE shm_mapping = nullptr;
  void* shm_view = nullptr;
  std::unique_ptr<FrameRingWriter> shm_writer;
  std::wstring resolved_shm_path;

  explicit Impl(MfCameraSink::Options given) : options(std::move(given)) {
    init_shm(options.shm_path, options.enable_shm);

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
    close_shm();
    close_section();
    if (camera) camera->Shutdown();
    camera.Reset();
    if (media_foundation_started) ::MFShutdown();
  }

  void init_shm(const std::wstring& path, bool enable) noexcept {
    if (!enable) return;
    resolved_shm_path = path.empty() ? windows_shm_default_path() : path;
    if (resolved_shm_path.empty()) return;

    const std::size_t ring_bytes = frame_ring_bytes();
    shm_file = ::CreateFileW(
        resolved_shm_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (shm_file == INVALID_HANDLE_VALUE) {
      shm_file = nullptr;
      resolved_shm_path.clear();
      return;
    }

    LARGE_INTEGER size;
    size.QuadPart = static_cast<LONGLONG>(ring_bytes);
    if (!::SetFilePointerEx(shm_file, size, nullptr, FILE_BEGIN) || !::SetEndOfFile(shm_file)) {
      ::CloseHandle(shm_file);
      shm_file = nullptr;
      resolved_shm_path.clear();
      return;
    }

    shm_mapping = ::CreateFileMappingW(
        shm_file,
        nullptr,
        PAGE_READWRITE,
        0,
        0,
        nullptr);
    if (shm_mapping == nullptr) {
      ::CloseHandle(shm_file);
      shm_file = nullptr;
      resolved_shm_path.clear();
      return;
    }

    shm_view = ::MapViewOfFile(shm_mapping, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, ring_bytes);
    if (shm_view == nullptr) {
      ::CloseHandle(shm_mapping);
      shm_mapping = nullptr;
      ::CloseHandle(shm_file);
      shm_file = nullptr;
      resolved_shm_path.clear();
      return;
    }

    shm_writer = std::make_unique<FrameRingWriter>(
        std::span<std::byte>(static_cast<std::byte*>(shm_view), ring_bytes));
    if (!shm_writer->valid()) {
      shm_writer.reset();
      ::UnmapViewOfFile(shm_view);
      shm_view = nullptr;
      ::CloseHandle(shm_mapping);
      shm_mapping = nullptr;
      ::CloseHandle(shm_file);
      shm_file = nullptr;
      resolved_shm_path.clear();
    }
  }

  void close_shm() noexcept {
    if (shm_writer) shm_writer.reset();
    if (shm_view != nullptr) {
      const std::size_t ring_bytes = frame_ring_bytes();
      std::memset(shm_view, 0, ring_bytes);
      ::FlushViewOfFile(shm_view, ring_bytes);
      ::UnmapViewOfFile(shm_view);
      shm_view = nullptr;
    }
    if (shm_mapping != nullptr) {
      ::CloseHandle(shm_mapping);
      shm_mapping = nullptr;
    }
    if (shm_file != nullptr) {
      ::CloseHandle(shm_file);
      shm_file = nullptr;
    }
    if (!resolved_shm_path.empty()) {
      ::DeleteFileW(resolved_shm_path.c_str());
      resolved_shm_path.clear();
    }
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
      // Not found is the ordinary idle state: no consumer has activated the
      // source, so it has not created its section. Denied is a DACL problem
      // the user cannot guess at.
      const DWORD error = ::GetLastError();
      if (error == ERROR_ACCESS_DENIED) {
        reason = CameraSinkUnavailableReason::SectionAccessDenied;
        status = static_cast<std::int32_t>(HRESULT_FROM_WIN32(error));
      }
      return false;
    }
    view = ::MapViewOfFile(section, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, frame_ring_bytes());
    if (view == nullptr) {
      // The section opened, so this is not permissions -- it is a section
      // smaller than this build's header, which means the media source is a
      // different version. Windows reports that as ERROR_ACCESS_DENIED too,
      // and calling it a DACL problem would send the user to permissions for
      // a mismatch only a reinstall fixes.
      const DWORD error = ::GetLastError();
      reason = CameraSinkUnavailableReason::SectionVersionMismatch;
      status = static_cast<std::int32_t>(HRESULT_FROM_WIN32(error));
      close_section();
      return false;
    }
    writer = std::make_unique<FrameRingWriter>(
        std::span<std::byte>(static_cast<std::byte*>(view), frame_ring_bytes()));
    if (!writer->valid()) {
      // Right size, wrong contents: a magic or version this build does not
      // recognise is the same mixed-install problem seen from the inside.
      reason = CameraSinkUnavailableReason::SectionVersionMismatch;
      close_section();
      return false;
    }
    return true;
  } catch (...) {
    close_section();
    return false;
  }
};

MfCameraSink::MfCameraSink() : MfCameraSink(Options{}) {}

MfCameraSink::MfCameraSink(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}

MfCameraSink::~MfCameraSink() = default;

auto MfCameraSink::available() const noexcept -> bool {
  return (impl_->shm_writer != nullptr && impl_->shm_writer->valid()) ||
         impl_->reason == CameraSinkUnavailableReason::None;
}

auto MfCameraSink::unavailable_reason() const noexcept -> CameraSinkUnavailableReason {
  if (impl_->shm_writer != nullptr && impl_->shm_writer->valid()) {
    return CameraSinkUnavailableReason::None;
  }
  return impl_->reason;
}

auto MfCameraSink::unavailable_status() const noexcept -> std::int32_t {
  if (impl_->shm_writer != nullptr && impl_->shm_writer->valid()) {
    return 0;
  }
  return impl_->status;
}

auto MfCameraSink::has_capacity() const noexcept -> bool {
  const std::uint64_t now = camera_clock_us();
  const bool shm_demand =
      (impl_->shm_writer != nullptr && impl_->shm_writer->valid() && impl_->shm_writer->has_demand(now));
  const bool vcam_demand =
      (impl_->reason == CameraSinkUnavailableReason::None && impl_->ensure_section() &&
       impl_->writer != nullptr && impl_->writer->has_demand(now));
  return shm_demand || vcam_demand;
}

auto MfCameraSink::submit(const CameraSinkFrame& frame) noexcept -> CameraSinkSubmit {
  if (frame.width != kCanvas.width || frame.height != kCanvas.height) {
    return CameraSinkSubmit::Failed;
  }
  if (!available()) return CameraSinkSubmit::Failed;

  const std::uint64_t now = camera_clock_us();
  bool wrote_any = false;

  if (impl_->shm_writer != nullptr && impl_->shm_writer->valid() &&
      impl_->shm_writer->has_demand(now)) {
    if (impl_->shm_writer->write(frame.bgra, frame.row_stride, frame.presentation_time_us)) {
      wrote_any = true;
    }
  }

  if (impl_->reason == CameraSinkUnavailableReason::None && impl_->ensure_section() &&
      impl_->writer != nullptr && impl_->writer->has_demand(now)) {
    if (impl_->writer->write(frame.bgra, frame.row_stride, frame.presentation_time_us)) {
      wrote_any = true;
    }
  }

  return wrote_any ? CameraSinkSubmit::Accepted : CameraSinkSubmit::Backpressured;
}

auto MfCameraSink::submit_written(CameraFrameWriter writer, void* context,
                                  std::uint64_t presentation_time_us) noexcept
    -> CameraSinkWrite {
  if (!available()) return CameraSinkWrite::Failed;

  const std::uint64_t now = camera_clock_us();
  const bool shm_demand =
      (impl_->shm_writer != nullptr && impl_->shm_writer->valid() && impl_->shm_writer->has_demand(now));
  const bool vcam_demand =
      (impl_->reason == CameraSinkUnavailableReason::None && impl_->ensure_section() &&
       impl_->writer != nullptr && impl_->writer->has_demand(now));

  if (!shm_demand && !vcam_demand) {
    return CameraSinkWrite::Backpressured;
  }

  if (shm_demand && vcam_demand) {
    struct DualContext {
      CameraFrameWriter writer;
      void* context;
      FrameRingWriter* vcam_writer;
      std::uint64_t presentation_time_us;
    } dual{writer, context, impl_->writer.get(), presentation_time_us};

    const auto copy_to_both = [](void* ctx, std::span<std::byte> dest,
                                 std::size_t stride) noexcept -> bool {
      auto& d = *static_cast<DualContext*>(ctx);
      if (!d.writer(d.context, dest, stride)) return false;
      if (d.vcam_writer != nullptr && d.vcam_writer->valid() &&
          d.vcam_writer->has_demand(camera_clock_us())) {
        (void)d.vcam_writer->write(dest, stride, d.presentation_time_us);
      }
      return true;
    };
    return impl_->shm_writer->write_with(copy_to_both, &dual, presentation_time_us)
               ? CameraSinkWrite::Accepted
               : CameraSinkWrite::Failed;
  }

  if (shm_demand) {
    return impl_->shm_writer->write_with(writer, context, presentation_time_us)
               ? CameraSinkWrite::Accepted
               : CameraSinkWrite::Failed;
  }

  return impl_->writer->write_with(writer, context, presentation_time_us)
             ? CameraSinkWrite::Accepted
             : CameraSinkWrite::Failed;
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
