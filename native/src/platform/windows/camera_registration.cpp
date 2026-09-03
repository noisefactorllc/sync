#include <sync/platform/camera_registration.hpp>

#include <windows.h>

#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <string>

#include "camera_source/source_guids.hpp"

namespace noisefactor::sync::camera {

namespace {

using Microsoft::WRL::ComPtr;
using RegistrationFn = HRESULT(__stdcall*)();

constexpr int kSuccess = 0;
constexpr int kFailure = 1;

// SyncCamera.dll sits beside the running executable. Resolved from the
// executable's own path rather than passed to LoadLibraryW bare, which would
// search the working directory and the rest of the DLL search order -- a
// place an attacker can more easily put a file than Program Files.
[[nodiscard]] auto module_directory() -> std::wstring {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD written =
        ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (written == 0) return {};
    if (written < path.size()) {
      path.resize(written);
      break;
    }
    path.resize(path.size() * 2);
  }
  const std::size_t separator = path.find_last_of(L'\\');
  if (separator == std::wstring::npos) return {};
  return path.substr(0, separator + 1);
}

[[nodiscard]] auto load_camera_source() -> HMODULE {
  const std::wstring directory = module_directory();
  if (directory.empty()) return nullptr;
  return ::LoadLibraryW((directory + L"SyncCamera.dll").c_str());
}

[[nodiscard]] auto call_registration_entry(const char* name) -> int {
  const HMODULE module = load_camera_source();
  if (module == nullptr) return kFailure;
  const auto entry = reinterpret_cast<RegistrationFn>(::GetProcAddress(module, name));
  const HRESULT hr = entry == nullptr ? E_NOINTERFACE : entry();
  ::FreeLibrary(module);
  return SUCCEEDED(hr) ? kSuccess : kFailure;
}

// Removes the device itself. Separate from unregistering the CLSID because a
// camera created with Lifetime_System outlives the process that made it, so
// dropping only the registration would leave the device in every picker with
// nothing behind it.
void remove_virtual_camera() {
  if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_LITE))) return;
  ComPtr<IMFVirtualCamera> camera;
  const HRESULT created = ::MFCreateVirtualCamera(
      MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
      MFVirtualCameraAccess_CurrentUser, kSyncCameraDisplayName, kSyncCameraSourceClsidString,
      nullptr, 0, &camera);
  if (SUCCEEDED(created) && camera) {
    // Same parameters reopen the existing camera, so this removes the one that
    // is already there rather than making a new one to delete.
    camera->Remove();
    camera->Shutdown();
  }
  camera.Reset();
  ::MFShutdown();
}

}  // namespace

auto register_camera_source() noexcept -> int {
  return call_registration_entry("DllRegisterServer");
}

auto unregister_camera_source() noexcept -> int {
  remove_virtual_camera();
  return call_registration_entry("DllUnregisterServer");
}

}  // namespace noisefactor::sync::camera
