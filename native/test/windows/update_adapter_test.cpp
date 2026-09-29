#include "test_harness.hpp"
#include "../../src/platform/windows/update_adapter.hpp"

#include <array>

using namespace noisefactor::sync::windows_update;

SYNC_TEST(windows_update_rejects_unsigned_and_missing_payloads) {
  std::array<wchar_t, MAX_PATH + 1> directory{};
  std::array<wchar_t, MAX_PATH + 1> path{};
  const DWORD size = ::GetTempPathW(static_cast<DWORD>(directory.size()), directory.data());
  SYNC_REQUIRE(size > 0 && size < directory.size());
  SYNC_REQUIRE(::GetTempFileNameW(directory.data(), L"sup", 0, path.data()) != 0);
  const bool trusted = verify_authenticode(path.data(), std::string(64, 'a'));
  // Delete before assertions so a failing trust test does not leak its file.
  SYNC_REQUIRE(::DeleteFileW(path.data()) != FALSE);
  SYNC_REQUIRE(!trusted);
  SYNC_REQUIRE(!verify_authenticode(path.data(), std::string(64, 'a')));
}

SYNC_TEST(windows_update_unconfigured_adapter_preserves_check_only_state) {
  // This target deliberately compiles without release configuration or SDK.
  UpdateAdapter adapter;
  adapter.initialize(L"0.1.0");
  SYNC_REQUIRE(!adapter.available());
  SYNC_REQUIRE(!adapter.automatic_checks_available());
  SYNC_REQUIRE(!adapter.automatic_checks_enabled());
  adapter.set_automatic_checks_enabled(true);
  SYNC_REQUIRE(!adapter.automatic_checks_enabled());
  SYNC_REQUIRE(!adapter.status().empty());
  adapter.cleanup();
  adapter.cleanup();
  SYNC_REQUIRE(!adapter.available());
}

namespace {
struct UpdateRegistryFixture {
  std::wstring path = L"Software\\NoiseFactorSyncUpdaterTest_" +
      std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(::GetTickCount64());
  HKEY key = nullptr;
  UpdateRegistryFixture() {
    DWORD disposition = 0;
    const LSTATUS result = ::RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0,
        nullptr, REG_OPTION_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, &disposition);
    if (result != ERROR_SUCCESS || disposition != REG_CREATED_NEW_KEY) {
      if (key) ::RegCloseKey(key);
      key = nullptr;
      throw std::runtime_error("could not create isolated update registry fixture");
    }
  }
  ~UpdateRegistryFixture() {
    if (key) {
      ::RegCloseKey(key);
      ::RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
    }
  }
  void write(DWORD type, const void* value, DWORD bytes) {
    SYNC_REQUIRE(::RegSetValueExW(key, L"CheckForUpdates", 0, type,
        static_cast<const BYTE*>(value), bytes) == ERROR_SUCCESS);
  }
};
}  // namespace

SYNC_TEST(windows_update_accepts_winsparkle_disabled_registry_string) {
  UpdateRegistryFixture fixture;
  // WinSparkle 0.9.4 serializes false as REG_SZ L"0", not a DWORD.
  constexpr wchar_t disabled[] = L"0";
  fixture.write(REG_SZ, disabled, sizeof(disabled));
  SYNC_REQUIRE(automatic_checks_disabled_in_registry(fixture.path));
}

SYNC_TEST(windows_update_registry_verification_fails_closed) {
  UpdateRegistryFixture fixture;
  SYNC_REQUIRE(!automatic_checks_disabled_in_registry(fixture.path));
  SYNC_REQUIRE(!automatic_checks_disabled_in_registry(fixture.path + L"\\Missing"));
  constexpr wchar_t enabled[] = L"1";
  fixture.write(REG_SZ, enabled, sizeof(enabled));
  SYNC_REQUIRE(!automatic_checks_disabled_in_registry(fixture.path));
  const DWORD disabled_dword = 0;
  fixture.write(REG_DWORD, &disabled_dword, sizeof(disabled_dword));
  SYNC_REQUIRE(!automatic_checks_disabled_in_registry(fixture.path));
  constexpr wchar_t disabled[] = L"0";
  fixture.write(REG_EXPAND_SZ, disabled, sizeof(disabled));
  SYNC_REQUIRE(!automatic_checks_disabled_in_registry(fixture.path));
  // RegSetValueExW repairs an omitted terminator when the next source wchar
  // is NUL. Keep it nonzero so this fixture really stores an unterminated "0".
  constexpr wchar_t unterminated_source[] = L"01";
  fixture.write(REG_SZ, unterminated_source, sizeof(wchar_t));
  wchar_t stored[] = {L'?', L'?'};
  DWORD stored_type = 0;
  DWORD stored_bytes = sizeof(wchar_t);
  SYNC_REQUIRE(::RegQueryValueExW(fixture.key, L"CheckForUpdates", nullptr,
      &stored_type, reinterpret_cast<BYTE*>(stored), &stored_bytes) == ERROR_SUCCESS);
  SYNC_REQUIRE(stored_type == REG_SZ);
  SYNC_REQUIRE(stored_bytes == sizeof(wchar_t));
  SYNC_REQUIRE(stored[0] == L'0' && stored[1] == L'?');
  SYNC_REQUIRE(!automatic_checks_disabled_in_registry(fixture.path));
  constexpr wchar_t trailing[] = L"0 ";
  fixture.write(REG_SZ, trailing, sizeof(trailing));
  SYNC_REQUIRE(!automatic_checks_disabled_in_registry(fixture.path));
  constexpr wchar_t embedded[] = {L'0', L'\0', L'1', L'\0'};
  fixture.write(REG_SZ, embedded, sizeof(embedded));
  SYNC_REQUIRE(!automatic_checks_disabled_in_registry(fixture.path));
  constexpr wchar_t empty[] = L"";
  fixture.write(REG_SZ, empty, sizeof(empty));
  SYNC_REQUIRE(!automatic_checks_disabled_in_registry(fixture.path));
  fixture.write(REG_SZ, nullptr, 0);
  SYNC_REQUIRE(!automatic_checks_disabled_in_registry(fixture.path));
}
