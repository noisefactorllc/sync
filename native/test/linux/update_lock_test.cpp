#include "test_harness.hpp"
#include <sync/platform/linux_update_lock.hpp>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
struct Fixture {
  std::string directory, lock, config, marker;
  Fixture() {
    std::array<char, 64> pattern{};
    const std::string base = "/tmp/sync-update-lock-XXXXXX";
    std::copy(base.begin(), base.end(), pattern.begin());
    const auto result = ::mkdtemp(pattern.data());
    SYNC_REQUIRE(result != nullptr);
    directory = result;
    lock = directory + "/lock"; config = directory + "/config";
    marker = directory + "/marker";
  }
  ~Fixture() { std::filesystem::remove_all(directory); }
  bool acquire(noisefactor::sync::update::LinuxStartupLock& gate) {
    return gate.acquire(lock.c_str(), config.c_str(), marker.c_str(), ::geteuid());
  }
  void create(const std::string& file) { std::ofstream stream(file); stream << "test"; }
};
}
SYNC_TEST(update_startup_allows_unenrolled_and_blocks_missing_enrolled_lock) {
  Fixture f;
  noisefactor::sync::update::LinuxStartupLock first, second;
  SYNC_REQUIRE(f.acquire(first));
  f.create(f.config);
  SYNC_REQUIRE(!f.acquire(second));
}
SYNC_TEST(update_startup_holds_shared_lock_until_exit) {
  Fixture f; f.create(f.lock);
  const int exclusive = ::open(f.lock.c_str(), O_RDONLY | O_CLOEXEC);
  SYNC_REQUIRE(exclusive >= 0);
  {
    noisefactor::sync::update::LinuxStartupLock first, second;
    SYNC_REQUIRE(f.acquire(first));
    SYNC_REQUIRE(f.acquire(second));
    SYNC_REQUIRE(::flock(exclusive, LOCK_EX | LOCK_NB) != 0);
  }
  SYNC_REQUIRE(::flock(exclusive, LOCK_EX | LOCK_NB) == 0);
  noisefactor::sync::update::LinuxStartupLock third;
  SYNC_REQUIRE(!f.acquire(third));
  ::close(exclusive);
}
SYNC_TEST(update_startup_refuses_incomplete_transaction_and_unsafe_lock) {
  Fixture f; f.create(f.lock); f.create(f.marker);
  noisefactor::sync::update::LinuxStartupLock first, second, third;
  SYNC_REQUIRE(!f.acquire(first));
  std::filesystem::remove(f.marker);
  SYNC_REQUIRE(::chmod(f.lock.c_str(), 0666) == 0);
  SYNC_REQUIRE(!f.acquire(second));
  std::filesystem::remove(f.lock);
  std::filesystem::create_directory(f.lock);
  SYNC_REQUIRE(!f.acquire(third));
}
