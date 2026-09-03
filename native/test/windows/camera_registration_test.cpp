#include "test_harness.hpp"

#include <windows.h>

#include <string>

#include "../../src/platform/windows/camera_source/source_guids.hpp"

namespace {

using RegistrationFn = HRESULT(__stdcall*)();

constexpr wchar_t kScratchKey[] = L"Software\\SyncCameraRegistrationTest";

// Redirects HKLM into a scratch key under HKCU for the life of the scope, so
// registration is exercised end to end without an administrator and without
// touching the machine's real hive.
struct RedirectedHklm {
  HKEY scratch = nullptr;
  RedirectedHklm() {
    ::RegCreateKeyExW(HKEY_CURRENT_USER, kScratchKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                      KEY_ALL_ACCESS, nullptr, &scratch, nullptr);
    ::RegOverridePredefKey(HKEY_LOCAL_MACHINE, scratch);
  }
  ~RedirectedHklm() {
    ::RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
    if (scratch != nullptr) ::RegCloseKey(scratch);
    ::RegDeleteTreeW(HKEY_CURRENT_USER, kScratchKey);
  }

  RedirectedHklm(const RedirectedHklm&) = delete;
  auto operator=(const RedirectedHklm&) -> RedirectedHklm& = delete;
};

struct LoadedDll {
  HMODULE module = ::LoadLibraryW(L"SyncCamera.dll");

  // Explicit because the deleted copy constructor below is user-declared,
  // which suppresses the implicit default one.
  LoadedDll() = default;
  ~LoadedDll() {
    if (module != nullptr) ::FreeLibrary(module);
  }

  [[nodiscard]] auto entry(const char* name) const -> RegistrationFn {
    return module == nullptr
               ? nullptr
               : reinterpret_cast<RegistrationFn>(::GetProcAddress(module, name));
  }

  LoadedDll(const LoadedDll&) = delete;
  auto operator=(const LoadedDll&) -> LoadedDll& = delete;
};

[[nodiscard]] auto inproc_path() -> std::wstring {
  HKEY key = nullptr;
  const std::wstring path = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") +
                            kSyncCameraSourceClsidString + L"\\InprocServer32";
  if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return {};
  }
  wchar_t value[MAX_PATH]{};
  DWORD bytes = sizeof(value);
  const LSTATUS status =
      ::RegQueryValueExW(key, nullptr, nullptr, nullptr, reinterpret_cast<LPBYTE>(value), &bytes);
  ::RegCloseKey(key);
  return status == ERROR_SUCCESS ? std::wstring(value) : std::wstring{};
}

[[nodiscard]] auto threading_model() -> std::wstring {
  HKEY key = nullptr;
  const std::wstring path = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") +
                            kSyncCameraSourceClsidString + L"\\InprocServer32";
  if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return {};
  }
  wchar_t value[64]{};
  DWORD bytes = sizeof(value);
  const LSTATUS status = ::RegQueryValueExW(key, L"ThreadingModel", nullptr, nullptr,
                                            reinterpret_cast<LPBYTE>(value), &bytes);
  ::RegCloseKey(key);
  return status == ERROR_SUCCESS ? std::wstring(value) : std::wstring{};
}

SYNC_TEST(register_then_unregister_round_trips) {
  RedirectedHklm redirect;
  LoadedDll dll;
  SYNC_REQUIRE(dll.module != nullptr);
  auto register_server = dll.entry("DllRegisterServer");
  auto unregister_server = dll.entry("DllUnregisterServer");
  SYNC_REQUIRE(register_server != nullptr && unregister_server != nullptr);

  SYNC_REQUIRE(inproc_path().empty());
  SYNC_REQUIRE(SUCCEEDED(register_server()));
  SYNC_REQUIRE(inproc_path().find(L"SyncCamera.dll") != std::wstring::npos);

  SYNC_REQUIRE(SUCCEEDED(unregister_server()));
  SYNC_REQUIRE(inproc_path().empty());
}

SYNC_TEST(registration_records_a_both_threading_model) {
  RedirectedHklm redirect;
  LoadedDll dll;
  SYNC_REQUIRE(dll.module != nullptr);
  SYNC_REQUIRE(SUCCEEDED(dll.entry("DllRegisterServer")()));
  // The frame server and every consuming process load this DLL into their own
  // apartments; Both is what lets it be created in either.
  SYNC_REQUIRE(threading_model() == L"Both");
}

SYNC_TEST(registration_records_an_absolute_path_to_the_dll) {
  RedirectedHklm redirect;
  LoadedDll dll;
  SYNC_REQUIRE(dll.module != nullptr);
  SYNC_REQUIRE(SUCCEEDED(dll.entry("DllRegisterServer")()));
  const std::wstring path = inproc_path();
  // A bare filename would send the frame server searching its own working
  // directory, which is not where Sync is installed.
  SYNC_REQUIRE(path.size() > 3 && path[1] == L':' && path[2] == L'\\');
}

SYNC_TEST(registration_is_idempotent) {
  RedirectedHklm redirect;
  LoadedDll dll;
  SYNC_REQUIRE(dll.module != nullptr);
  auto register_server = dll.entry("DllRegisterServer");
  SYNC_REQUIRE(SUCCEEDED(register_server()));
  SYNC_REQUIRE(SUCCEEDED(register_server()));
  SYNC_REQUIRE(!inproc_path().empty());
}

SYNC_TEST(unregistering_what_was_never_registered_succeeds) {
  RedirectedHklm redirect;
  LoadedDll dll;
  SYNC_REQUIRE(dll.module != nullptr);
  // Uninstall runs this whether or not registration ever happened, and a
  // second uninstall must not fail either.
  SYNC_REQUIRE(SUCCEEDED(dll.entry("DllUnregisterServer")()));
  SYNC_REQUIRE(SUCCEEDED(dll.entry("DllUnregisterServer")()));
}

}  // namespace
