#include "update_adapter.hpp"

#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <array>
#include <atomic>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <utility>

#ifndef SYNC_HAS_WINSPARKLE
#define SYNC_HAS_WINSPARKLE 0
#endif
#if SYNC_HAS_WINSPARKLE
// Read only the version header: winsparkle.h injects an MSVC import-library
// dependency, defeating graceful startup when the optional DLL is missing.
// The C signatures below are from the pinned 0.9.4 public winsparkle.h.
#include <winsparkle-version.h>
#if WIN_SPARKLE_VERSION_MAJOR != 0 || WIN_SPARKLE_VERSION_MINOR != 9 || WIN_SPARKLE_VERSION_MICRO != 4
#error Sync requires WinSparkle 0.9.4
#endif
#endif

namespace noisefactor::sync::windows_update {
namespace {
struct File {
  HANDLE handle = INVALID_HANDLE_VALUE;
  explicit File(const std::wstring& path) {
    handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
      ::CloseHandle(handle);
      handle = INVALID_HANDLE_VALUE;
    }
  }
  ~File() { if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle); }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
};

std::string hex_digest(const BYTE* bytes, DWORD count) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(count * 2);
  for (DWORD i = 0; i < count; ++i) {
    result += digits[bytes[i] >> 4];
    result += digits[bytes[i] & 15];
  }
  return result;
}

#if SYNC_HAS_WINSPARKLE
std::uint64_t unix_seconds() {
  const auto now = std::time(nullptr);
  return now > 0 ? static_cast<std::uint64_t>(now) : 0;
}

std::wstring executable_path() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    DWORD written = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (written == 0 || path.size() > 32768) return {};
    if (written < path.size()) { path.resize(written); return path; }
    path.resize(path.size() * 2);
  }
}

bool file_digest_matches(HANDLE file, std::string_view expected) {
  HCRYPTPROV provider = 0;
  HCRYPTHASH hash = 0;
  if (!::CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
    return false;
  bool ok = ::CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
  std::array<BYTE, 65536> block{};
  DWORD read = 0;
  while (ok) {
    if (!::ReadFile(file, block.data(), static_cast<DWORD>(block.size()), &read, nullptr)) {
      ok = false;
      break;
    }
    if (read == 0) break;
    ok = ::CryptHashData(hash, block.data(), read, 0) != FALSE;
  }
  std::array<BYTE, 32> digest{};
  DWORD count = static_cast<DWORD>(digest.size());
  ok = ok && ::CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &count, 0) &&
       digest_matches(hex_digest(digest.data(), count), expected);
  if (hash) ::CryptDestroyHash(hash);
  ::CryptReleaseContext(provider, 0);
  return ok;
}
#endif
}  // namespace

bool automatic_checks_disabled_in_registry(const std::wstring& registry_path) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, registry_path.c_str(), 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
  wchar_t value[2]{};
  DWORD type = 0;
  DWORD bytes = sizeof(value);
  // WinSparkle writes a REG_SZ "0". Query the raw value rather than using
  // RegGetValue, which can silently append a missing string terminator.
  const LSTATUS result = ::RegQueryValueExW(key, L"CheckForUpdates", nullptr,
      &type, reinterpret_cast<BYTE*>(value), &bytes);
  ::RegCloseKey(key);
  return result == ERROR_SUCCESS && type == REG_SZ && bytes == sizeof(value) &&
         value[0] == L'0' && value[1] == L'\0';
}

bool verify_authenticode(const std::wstring& path, std::string_view publisher_sha256) {
  if (!is_sha256(publisher_sha256)) return false;
  // Hold the same non-writable, non-deletable file while WinTrust inspects it.
  File file(path);
  if (file.handle == INVALID_HANDLE_VALUE) return false;
  WINTRUST_FILE_INFO subject{};
  subject.cbStruct = sizeof(subject);
  subject.pcwszFilePath = path.c_str();
  subject.hFile = file.handle;
  WINTRUST_DATA trust{};
  trust.cbStruct = sizeof(trust);
  trust.dwUIChoice = WTD_UI_NONE;
  trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
  trust.dwUnionChoice = WTD_CHOICE_FILE;
  trust.pFile = &subject;
  trust.dwStateAction = WTD_STATEACTION_VERIFY;
  // No network work or unpredictable revocation fetch on the tray owner
  // thread. Missing/expired cached revocation information fails closed.
  trust.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | WTD_CACHE_ONLY_URL_RETRIEVAL;
  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG result = ::WinVerifyTrust(nullptr, &policy, &trust);
  bool valid = false;
  if (result == ERROR_SUCCESS) {
    auto* data = ::WTHelperProvDataFromStateData(trust.hWVTStateData);
    auto* signer = data ? ::WTHelperGetProvSignerFromChain(data, 0, FALSE, 0) : nullptr;
    if (signer && signer->csCertChain && signer->pasCertChain[0].pCert) {
      const auto* cert = signer->pasCertChain[0].pCert;
      std::array<BYTE, 32> digest{};
      DWORD count = static_cast<DWORD>(digest.size());
      valid = ::CryptHashCertificate2(L"SHA256", 0, nullptr, cert->pbCertEncoded,
                                     cert->cbCertEncoded, digest.data(), &count) &&
              digest_matches(hex_digest(digest.data(), count), publisher_sha256);
    }
  }
  trust.dwStateAction = WTD_STATEACTION_CLOSE;
  ::WinVerifyTrust(nullptr, &policy, &trust);
  return valid;
}

struct UpdateAdapter::Impl {
  std::wstring unavailable = L"This build does not include signed automatic updates.";
  bool initialized = false;
#if SYNC_HAS_WINSPARKLE
  HMODULE module = nullptr;
  Configuration config{SYNC_WINDOWS_UPDATE_FEED, SYNC_WINDOWS_UPDATE_EDDSA_KEY,
                       SYNC_WINDOWS_UPDATE_PUBLISHER_SHA256, SYNC_WINSPARKLE_DLL_SHA256};
  enum class Result : DWORD { Never, Checking, Available, Current, Error, InstallBlocked, InvalidPublisher, Cancelled };
  CheckSchedule schedule;
  Result result = Result::Never;
  struct CallbackState {
    explicit CallbackState(std::string publisher) : publisher_sha256(std::move(publisher)) {}
    const std::string publisher_sha256;
    std::atomic<int> pending_result{-1};
    std::atomic<bool> checking{false};
  };
  std::shared_ptr<CallbackState> callbacks = std::make_shared<CallbackState>(config.publisher_sha256);
  bool settings_error = false;
  static constexpr wchar_t policy_path[] = L"Software\\Noise Factor\\Sync\\Updates\\Preview\\Policy";
  static std::atomic<std::shared_ptr<CallbackState>>& active() {
    // WinSparkle 0.9.4 cleanup does not join its workers. This callback slot
    // therefore has process lifetime, including during C++ static teardown.
    static auto* value = new std::atomic<std::shared_ptr<CallbackState>>;
    return *value;
  }
  using Callback = void (__cdecl*)();
  using CanShutdown = int (__cdecl*)();
  using RunInstaller = int (__cdecl*)(const wchar_t*);
  void (__cdecl* init)() = nullptr;
  void (__cdecl* cleanup)() = nullptr;
  void (__cdecl* set_appcast_url)(const char*) = nullptr;
  int (__cdecl* set_eddsa_public_key)(const char*) = nullptr;
  void (__cdecl* set_app_details)(const wchar_t*, const wchar_t*, const wchar_t*) = nullptr;
  void (__cdecl* set_registry_path)(const char*) = nullptr;
  void (__cdecl* set_automatic_check_for_updates)(int) = nullptr;
  void (__cdecl* set_did_find_update_callback)(Callback) = nullptr;
  void (__cdecl* set_did_not_find_update_callback)(Callback) = nullptr;
  void (__cdecl* set_error_callback)(Callback) = nullptr;
  void (__cdecl* set_update_dismissed_callback)(Callback) = nullptr;
  void (__cdecl* set_can_shutdown_callback)(CanShutdown) = nullptr;
  void (__cdecl* set_user_run_installer_callback)(RunInstaller) = nullptr;
  void (__cdecl* check_update_with_ui)() = nullptr;

  template <typename T> bool bind(T& function, const char* name) {
    const FARPROC address = ::GetProcAddress(module, name);
    static_assert(sizeof(function) == sizeof(address));
    // The verified public ABI supplies the type. Copying the representation
    // avoids MSVC C4191 while retaining the explicit symbol-by-symbol check.
    std::memcpy(&function, &address, sizeof(function));
    return function != nullptr;
  }
  void load_policy() {
    HKEY key = nullptr;
    const LSTATUS opened = ::RegOpenKeyExW(HKEY_CURRENT_USER, policy_path, 0, KEY_QUERY_VALUE, &key);
    if (opened == ERROR_FILE_NOT_FOUND) return;
    if (opened != ERROR_SUCCESS) { settings_error = true; return; }
    const auto read = [key](const wchar_t* name, DWORD type, auto& value) {
      DWORD actual_type = 0;
      DWORD bytes = sizeof(value);
      decltype(value + 0) candidate = value;
      const LSTATUS status = ::RegQueryValueExW(key, name, nullptr, &actual_type,
          reinterpret_cast<BYTE*>(&candidate), &bytes);
      if (status == ERROR_FILE_NOT_FOUND) return true;
      if (status != ERROR_SUCCESS || actual_type != type || bytes != sizeof(value)) return false;
      value = candidate;
      return true;
    };
    DWORD enabled = 1;
    DWORD saved_result = 0;
    const bool loaded = read(L"Enabled", REG_DWORD, enabled) &&
                        read(L"PausedUntil", REG_QWORD, schedule.paused_until) &&
                        read(L"LastCheckStarted", REG_QWORD, schedule.last_check) &&
                        read(L"LastResult", REG_DWORD, saved_result);
    ::RegCloseKey(key);
    if (!loaded || enabled > 1 || saved_result > static_cast<DWORD>(Result::Cancelled)) {
      settings_error = true;
      return;
    }
    schedule.enabled = enabled != 0;
    result = static_cast<Result>(saved_result);
    if (result == Result::Checking) result = Result::Error;
  }
  bool save_policy() {
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, policy_path, 0, nullptr, 0, KEY_SET_VALUE,
                          nullptr, &key, nullptr) != ERROR_SUCCESS) {
      settings_error = true;
      return false;
    }
    const auto write = [key](const wchar_t* name, DWORD type, const auto& value) {
      return ::RegSetValueExW(key, name, 0, type, reinterpret_cast<const BYTE*>(&value),
                              sizeof(value)) == ERROR_SUCCESS;
    };
    const DWORD enabled = schedule.enabled ? 1 : 0;
    const DWORD saved_result = static_cast<DWORD>(result);
    const bool saved = write(L"Enabled", REG_DWORD, enabled) &&
                       write(L"PausedUntil", REG_QWORD, schedule.paused_until) &&
                       write(L"LastCheckStarted", REG_QWORD, schedule.last_check) &&
                       write(L"LastResult", REG_DWORD, saved_result);
    ::RegCloseKey(key);
    settings_error = !saved;
    return saved;
  }
  void start_check(bool manual) {
    // The pinned C API has no metadata-only probe. Its without_ui entry point
    // opens an offer dialog and download controls, so only explicit checks
    // may reach WinSparkle. A timer can never arm that UI.
    if (!manual || callbacks->checking.exchange(true)) return;
    callbacks->pending_result.store(-1, std::memory_order_release);
    schedule.last_check = unix_seconds();
    result = Result::Checking;
    // Persist the attempt before any network request, so a restart cannot
    // turn a failed request into a rapid retry loop. Manual checks keep pause.
    if (!save_policy()) { callbacks->checking.store(false); return; }
    check_update_with_ui();
  }
  static void record_result(Result value) {
    if (auto self = active().load(std::memory_order_acquire)) {
      // Framework callbacks may use any thread. Only the tray owner consumes
      // and persists these results, through poll().
      self->pending_result.store(static_cast<int>(value), std::memory_order_release);
      self->checking.store(false, std::memory_order_release);
    }
  }
  static void __cdecl found_update() { record_result(Result::Available); }
  static void __cdecl no_update() { record_result(Result::Current); }
  static void __cdecl check_error() { record_result(Result::Error); }
  static void __cdecl dismissed() {
    if (auto self = active().load(std::memory_order_acquire)) {
      // A closed progress dialog must not leave the scheduler permanently
      // busy. A completed check keeps its more specific result.
      if (self->checking.exchange(false))
        self->pending_result.store(static_cast<int>(Result::Cancelled), std::memory_order_release);
    }
  }
  static int __cdecl can_shutdown() {
    record_result(Result::InstallBlocked);
    // A sender count cannot prove external camera inactivity or reserve
    // daemon admission. No installed callback can bypass this veto.
    return 0;
  }
  static int __cdecl run_installer(const wchar_t* path) {
    if (auto self = active().load(std::memory_order_acquire)) {
      const bool valid = path && verify_authenticode(path, self->publisher_sha256);
      record_result(valid ? Result::InstallBlocked : Result::InvalidPublisher);
    }
    // WinSparkle: 0 means run the installer itself; -1 means reject it.
    // Never retain its temporary path or launch before an atomic reservation.
    return -1;
  }
#endif
};

UpdateAdapter::UpdateAdapter() : impl_(std::make_unique<Impl>()) {}
UpdateAdapter::~UpdateAdapter() { cleanup(); }

void UpdateAdapter::initialize(std::wstring_view version) {
#if SYNC_HAS_WINSPARKLE
  if (impl_->initialized || impl_->module) return;
  const auto error = configuration_error(impl_->config);
  if (!error.empty()) { impl_->unavailable.assign(error.begin(), error.end()); return; }
  const std::wstring executable = executable_path();
  if (executable.empty() || !verify_authenticode(executable, impl_->config.publisher_sha256)) {
    impl_->unavailable = L"This copy of Sync could not verify its Windows publisher.";
    return;
  }
  const auto separator = executable.find_last_of(L"\\/");
  if (separator == std::wstring::npos) return;
  const std::wstring path = executable.substr(0, separator + 1) + L"WinSparkle.dll";
  File file(path);
  if (file.handle == INVALID_HANDLE_VALUE ||
      !file_digest_matches(file.handle, impl_->config.framework_sha256)) {
    impl_->unavailable = L"The pinned WinSparkle component is missing or changed.";
    return;
  }
  // The pinned framework has process-global callbacks and cleanup does not
  // join workers. Never initialize another instance in this process: an old
  // worker could otherwise deliver its result to the new adapter's callbacks.
  // Claim before loading or changing any global framework configuration.
  static std::atomic_flag framework_used = ATOMIC_FLAG_INIT;
  if (framework_used.test_and_set(std::memory_order_acq_rel)) {
    impl_->unavailable = L"Update checks require restarting Sync.";
    return;
  }
  impl_->module = ::LoadLibraryExW(path.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!impl_->module) { impl_->unavailable = L"WinSparkle could not be loaded."; return; }
#define SYNC_BIND_WINSPARKLE(name) impl_->bind(impl_->name, "win_sparkle_" #name)
  const bool complete = SYNC_BIND_WINSPARKLE(init) && SYNC_BIND_WINSPARKLE(cleanup) &&
      SYNC_BIND_WINSPARKLE(set_appcast_url) && SYNC_BIND_WINSPARKLE(set_eddsa_public_key) &&
      SYNC_BIND_WINSPARKLE(set_app_details) && SYNC_BIND_WINSPARKLE(set_registry_path) &&
      SYNC_BIND_WINSPARKLE(set_automatic_check_for_updates) &&
      SYNC_BIND_WINSPARKLE(set_did_find_update_callback) &&
      SYNC_BIND_WINSPARKLE(set_did_not_find_update_callback) &&
      SYNC_BIND_WINSPARKLE(set_error_callback) &&
      SYNC_BIND_WINSPARKLE(set_update_dismissed_callback) &&
      SYNC_BIND_WINSPARKLE(set_can_shutdown_callback) &&
      SYNC_BIND_WINSPARKLE(set_user_run_installer_callback) &&
      SYNC_BIND_WINSPARKLE(check_update_with_ui);
#undef SYNC_BIND_WINSPARKLE
  if (!complete || impl_->set_eddsa_public_key(impl_->config.eddsa_public_key.c_str()) != 1) {
    impl_->unavailable = L"WinSparkle rejected its API configuration or signing key.";
    ::FreeLibrary(impl_->module);
    impl_->module = nullptr;
    return;
  }
  const std::wstring owned_version(version);
  impl_->set_app_details(L"Noise Factor LLC", L"Sync", owned_version.c_str());
  impl_->set_registry_path("Software\\Noise Factor\\Sync\\Updates\\Preview");
  impl_->set_appcast_url(impl_->config.feed_url.c_str());
  // The framework's scheduler can open UI/download controls. Keep it off,
  // including an enabled preference saved by a previous build. Its setter
  // returns void and catches write failures: confirm its persisted REG_SZ
  // false value before init, rather than treating a failed write as safe.
  impl_->set_automatic_check_for_updates(0);
  if (!automatic_checks_disabled_in_registry(
        L"Software\\Noise Factor\\Sync\\Updates\\Preview")) {
    impl_->unavailable = L"Update checks could not be configured safely.";
    ::FreeLibrary(impl_->module);
    impl_->module = nullptr;
    return;
  }
  impl_->set_did_find_update_callback(&Impl::found_update);
  impl_->set_did_not_find_update_callback(&Impl::no_update);
  impl_->set_error_callback(&Impl::check_error);
  impl_->set_update_dismissed_callback(&Impl::dismissed);
  impl_->set_can_shutdown_callback(&Impl::can_shutdown);
  impl_->set_user_run_installer_callback(&Impl::run_installer);
  std::shared_ptr<Impl::CallbackState> empty;
  if (!Impl::active().compare_exchange_strong(empty, impl_->callbacks)) {
    impl_->unavailable = L"Another Sync updater is already active.";
    ::FreeLibrary(impl_->module);
    impl_->module = nullptr;
    return;
  }
  impl_->load_policy();
  impl_->init();
  impl_->initialized = true;
#else
  (void)version;
#endif
}

void UpdateAdapter::cleanup() {
#if SYNC_HAS_WINSPARKLE
  auto expected = impl_->callbacks;
  Impl::active().compare_exchange_strong(expected, nullptr);
  if (impl_->initialized) {
    impl_->cleanup();
    // Contrary to its header description, pinned dll_api.cpp only stops the
    // UI thread. Checker/downloader workers may still execute. Keep this DLL
    // reference until process exit; their callbacks own detached state and
    // can no longer access this adapter. Never unload beneath those workers.
  } else if (impl_->module) {
    ::FreeLibrary(impl_->module);
  }
  impl_->module = nullptr;
#endif
  impl_->initialized = false;
}

bool UpdateAdapter::available() const { return impl_->initialized; }
bool UpdateAdapter::automatic_checks_available() const {
  // WinSparkle 0.9.4 offers no C API for a metadata-only, quiet check.
  return false;
}
bool UpdateAdapter::automatic_checks_enabled() const {
#if SYNC_HAS_WINSPARKLE
  return available() && automatic_checks_available() && impl_->schedule.enabled;
#else
  return false;
#endif
}
void UpdateAdapter::set_automatic_checks_enabled(bool enabled) {
#if SYNC_HAS_WINSPARKLE
  if (available() && automatic_checks_available()) { impl_->schedule.enabled = enabled; impl_->save_policy(); }
#else
  (void)enabled;
#endif
}
bool UpdateAdapter::paused() const {
#if SYNC_HAS_WINSPARKLE
  return available() && automatic_checks_available() && impl_->schedule.paused(unix_seconds());
#else
  return false;
#endif
}
void UpdateAdapter::pause_for_24_hours() {
#if SYNC_HAS_WINSPARKLE
  if (available() && automatic_checks_available()) { impl_->schedule.pause_for_day(unix_seconds()); impl_->save_policy(); }
#endif
}
void UpdateAdapter::pause_indefinitely() {
#if SYNC_HAS_WINSPARKLE
  if (available() && automatic_checks_available()) { impl_->schedule.pause_indefinitely(); impl_->save_policy(); }
#endif
}
void UpdateAdapter::resume_checks() {
#if SYNC_HAS_WINSPARKLE
  if (available() && automatic_checks_available()) { impl_->schedule.resume(); impl_->save_policy(); }
#endif
}
void UpdateAdapter::poll() {
#if SYNC_HAS_WINSPARKLE
  if (!available()) return;
  const int result = impl_->callbacks->pending_result.exchange(-1, std::memory_order_acquire);
  if (result >= 0) {
    impl_->result = static_cast<Impl::Result>(result);
    impl_->save_policy();
  }
  // No network or offer UI is started from this timer. Keep status persistence
  // on the owner thread while the framework lacks a quiet information API.
#endif
}
std::wstring UpdateAdapter::status() const {
  if (!available()) return impl_->unavailable;
#if SYNC_HAS_WINSPARKLE
  if (impl_->settings_error) return L"Update preferences could not be read or saved.";
  if (paused()) return L"Update checks paused; installation remains manual.";
  switch (impl_->result) {
    case Impl::Result::Checking: return L"Checking for updates; installation remains manual.";
    case Impl::Result::Available: return L"An update is available; download and install it manually.";
    case Impl::Result::Current: return L"Sync is up to date; automatic installation is not enabled.";
    case Impl::Result::Error: return L"Update check failed; Sync is still running normally.";
    case Impl::Result::InstallBlocked: return L"Updater installation is disabled; install manually after closing sessions.";
    case Impl::Result::InvalidPublisher: return L"The downloaded installer failed Windows publisher verification.";
    case Impl::Result::Cancelled: return L"Update check cancelled; Sync is still running normally.";
    case Impl::Result::Never: break;
  }
#endif
  return L"Use Check for Updates; automatic checks and installation are unavailable in this build.";
}
std::wstring UpdateAdapter::last_check_status() const {
#if SYNC_HAS_WINSPARKLE
  if (!available()) return {};
  if (impl_->schedule.last_check == 0) return L"Last check: never";
  const auto seconds = static_cast<std::time_t>(impl_->schedule.last_check);
  std::tm local{};
  std::array<wchar_t, 64> text{};
  if (localtime_s(&local, &seconds) != 0 ||
      std::wcsftime(text.data(), text.size(), L"%Y-%m-%d %H:%M", &local) == 0)
    return L"Last check time unavailable";
  return std::wstring(L"Last check started: ") + text.data() + L" (local)";
#else
  return {};
#endif
}
void UpdateAdapter::check_for_updates(HWND owner) {
  if (!available()) {
    ::MessageBoxW(owner, status().c_str(), L"Sync updates", MB_OK | MB_ICONINFORMATION);
    return;
  }
#if SYNC_HAS_WINSPARKLE
  if (impl_->callbacks->checking.load(std::memory_order_acquire)) {
    ::MessageBoxW(owner, L"An update check is already in progress.", L"Sync updates",
                  MB_OK | MB_ICONINFORMATION);
    return;
  }
#endif
  ::MessageBoxW(owner,
      L"Automatic installation is unavailable in this build. If an update is available, "
      L"download it from the Sync website and install it after closing your sessions.",
      L"Sync updates — checks only", MB_OK | MB_ICONINFORMATION);
#if SYNC_HAS_WINSPARKLE
  impl_->start_check(true);
#endif
}
}  // namespace noisefactor::sync::windows_update
