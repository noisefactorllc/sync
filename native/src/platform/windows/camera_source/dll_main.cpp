#include <windows.h>

#include <mfapi.h>
#include <new>
#include <string>

#include "media_source.hpp"
#include "module_lock.hpp"
#include "source_guids.hpp"

namespace {

HMODULE g_module = nullptr;
std::atomic<LONG> g_locks{0};

// The frame server resolves the CLSID through HKLM, so registration writes
// there. HKCU is not an option: the frame server runs as Local Service and
// cannot see the interactive user's hive.
constexpr wchar_t kClassesRoot[] = L"SOFTWARE\\Classes\\CLSID\\";

[[nodiscard]] auto ModulePath() -> std::wstring {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD written =
        ::GetModuleFileNameW(g_module, path.data(), static_cast<DWORD>(path.size()));
    if (written == 0) return {};
    if (written < path.size()) {
      path.resize(written);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

[[nodiscard]] auto WriteString(HKEY key, const wchar_t* name, const std::wstring& value)
    -> LSTATUS {
  return ::RegSetValueExW(key, name, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

class SourceFactory final : public IClassFactory {
 public:
  auto STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) -> HRESULT override {
    if (object == nullptr) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *object = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }

  auto STDMETHODCALLTYPE AddRef() -> ULONG override { return ++references_; }

  auto STDMETHODCALLTYPE Release() -> ULONG override {
    const ULONG remaining = --references_;
    if (remaining == 0) delete this;
    return remaining;
  }

  auto STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** object)
      -> HRESULT override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (outer != nullptr) return CLASS_E_NOAGGREGATION;

    // The activator, not the media source. The frame server creates this
    // CLSID, asks for IMFActivate and calls ActivateObject to reach the
    // source; handing back the source directly fails at
    // IMFVirtualCamera::Start with E_NOINTERFACE.
    auto* activator = new (std::nothrow) noisefactor::sync::camera::SyncCameraActivator();
    if (activator == nullptr) return E_OUTOFMEMORY;
    const HRESULT hr = activator->Initialize();
    if (FAILED(hr)) {
      activator->Release();
      return hr;
    }
    const HRESULT queried = activator->QueryInterface(riid, object);
    activator->Release();
    return queried;
  }

  auto STDMETHODCALLTYPE LockServer(BOOL lock) -> HRESULT override {
    if (lock) {
      ++g_locks;
    } else {
      --g_locks;
    }
    return S_OK;
  }

 private:
  noisefactor::sync::camera::ModuleReference module_reference_;
  std::atomic<ULONG> references_{1};
};

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    ::DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** object) {
  if (object == nullptr) return E_POINTER;
  *object = nullptr;
  if (!::IsEqualCLSID(clsid, kSyncCameraSourceClsid)) return CLASS_E_CLASSNOTAVAILABLE;

  auto* factory = new (std::nothrow) SourceFactory();
  if (factory == nullptr) return E_OUTOFMEMORY;
  const HRESULT hr = factory->QueryInterface(riid, object);
  factory->Release();
  return hr;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
  // Outstanding objects count as much as explicit locks. Answering on locks
  // alone lets COM unload this DLL while the frame server still holds a live
  // media source, and every vtable pointer in it then dangles.
  if (noisefactor::sync::camera::module_references().load(std::memory_order_acquire) != 0) {
    return S_FALSE;
  }
  return g_locks.load(std::memory_order_acquire) == 0 ? S_OK : S_FALSE;
}

extern "C" HRESULT __stdcall DllRegisterServer() {
  const std::wstring path = ModulePath();
  if (path.empty()) return HRESULT_FROM_WIN32(::GetLastError());

  const std::wstring clsid_key = std::wstring(kClassesRoot) + kSyncCameraSourceClsidString;
  HKEY key = nullptr;
  LSTATUS status = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, clsid_key.c_str(), 0, nullptr,
                                     REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  status = WriteString(key, nullptr, kSyncCameraSourceFriendlyName);
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

  const std::wstring inproc_key = clsid_key + L"\\InprocServer32";
  status = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, inproc_key.c_str(), 0, nullptr,
                             REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  status = WriteString(key, nullptr, path);
  if (status == ERROR_SUCCESS) status = WriteString(key, L"ThreadingModel", L"Both");
  ::RegCloseKey(key);
  return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
  const std::wstring clsid_key = std::wstring(kClassesRoot) + kSyncCameraSourceClsidString;
  const LSTATUS status = ::RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsid_key.c_str());
  // Uninstall runs this whether or not registration ever happened, and a
  // second uninstall must not fail either, so "already gone" is success.
  if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) return S_OK;
  return HRESULT_FROM_WIN32(status);
}
