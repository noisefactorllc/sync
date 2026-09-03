// Reports what the running Windows image and the installed media source can
// actually do, one step at a time. Exit codes are the contract:
//
//   0  virtual cameras work here
//   2  Media Foundation works but virtual cameras do not
//   3  Media Foundation is absent
//   4  the media source is not registered, or refuses a required interface
//
// CI reads these to decide whether the end-to-end camera test can run on a
// given runner image, and a developer reads the per-step HRESULTs when the
// camera will not come up on a machine.

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <objbase.h>
#include <winternl.h>
#include <wrl/client.h>

#include <ks.h>
#include <ksproxy.h>

#include <cstdio>

#include "../../src/platform/windows/camera_source/source_guids.hpp"

namespace {

using Microsoft::WRL::ComPtr;

[[nodiscard]] auto build_number() -> unsigned long {
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) return 0;
  const auto rtl_get_version =
      reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
  if (rtl_get_version == nullptr) return 0;
  RTL_OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (rtl_get_version(&info) != 0) return 0;
  return info.dwBuildNumber;
}

template <typename Interface>
void report_interface(IMFMediaSource* source, const char* name) {
  ComPtr<Interface> queried;
  const HRESULT hr = source->QueryInterface(IID_PPV_ARGS(&queried));
  std::printf("source_implements_%s=%d (0x%08lX)\n", name, SUCCEEDED(hr) ? 1 : 0,
              static_cast<unsigned long>(hr));
}

}  // namespace

int main() {
  const unsigned long build = build_number();
  std::printf("build_number=%lu\n", build);
  std::printf("build_22000_or_later=%d\n", build >= 22000 ? 1 : 0);

  ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
    std::printf("mfplat=0\nverdict=no_media_foundation\n");
    return 3;
  }
  std::printf("mfplat=1\n");

  // Exactly what the frame server does: create the registered CLSID, which is
  // an activator rather than the source itself, then activate the source and
  // ask it for the interfaces the pipeline requires.
  ComPtr<IMFActivate> activator;
  const HRESULT created_activator = ::CoCreateInstance(
      kSyncCameraSourceClsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&activator));
  std::printf("cocreateinstance_imfactivate_hr=0x%08lX\n",
              static_cast<unsigned long>(created_activator));
  if (FAILED(created_activator)) {
    std::printf("verdict=source_not_registered_or_unloadable\n");
    ::MFShutdown();
    return 4;
  }

  ComPtr<IMFMediaSource> source;
  const HRESULT activated = activator->ActivateObject(IID_PPV_ARGS(&source));
  std::printf("activateobject_hr=0x%08lX\n", static_cast<unsigned long>(activated));
  if (FAILED(activated)) {
    std::printf("verdict=source_activation_failed\n");
    ::MFShutdown();
    return 4;
  }
  report_interface<IMFMediaSourceEx>(source.Get(), "imfmediasourceex");
  report_interface<IMFGetService>(source.Get(), "imfgetservice");
  report_interface<IKsControl>(source.Get(), "ikscontrol");

  ComPtr<IMFPresentationDescriptor> presentation;
  const HRESULT descriptor = source->CreatePresentationDescriptor(&presentation);
  std::printf("createpresentationdescriptor_hr=0x%08lX\n", static_cast<unsigned long>(descriptor));
  source->Shutdown();
  source.Reset();

  ComPtr<IMFVirtualCamera> camera;
  const HRESULT created = ::MFCreateVirtualCamera(
      MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
      MFVirtualCameraAccess_CurrentUser, kSyncCameraDisplayName, kSyncCameraSourceClsidString,
      nullptr, 0, &camera);
  std::printf("mfcreatevirtualcamera_hr=0x%08lX\n", static_cast<unsigned long>(created));

  HRESULT started = E_FAIL;
  if (SUCCEEDED(created) && camera) {
    started = camera->Start(nullptr);
    std::printf("virtualcamera_start_hr=0x%08lX\n", static_cast<unsigned long>(started));
    camera->Shutdown();
  }
  camera.Reset();
  ::MFShutdown();

  const bool capable = SUCCEEDED(created) && SUCCEEDED(started);
  std::printf("verdict=%s\n", capable ? "virtual_cameras_supported" : "no_virtual_cameras");
  return capable ? 0 : 2;
}
