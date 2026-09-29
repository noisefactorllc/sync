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
