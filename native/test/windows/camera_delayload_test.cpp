// Asserts that nothing Sync ships resolves MFCreateVirtualCamera at load time.
//
// mfsensorgroup.dll only exports it on Windows build 22000+. Sync.iss keeps
// MinVersion=10.0 on purpose -- the installer is meant to work on Windows 10,
// with the camera line simply reading unavailable -- but an ordinary import
// makes the loader resolve every imported symbol before main runs, so a plain
// import turns "the camera is unavailable" into "the application will not
// start", and windows_supports_virtual_cameras() never executes to say so.
//
// That is not hypothetical: the release harness built on Windows Server 2022
// (build 20348), and both binaries importing mfsensorgroup exited 0xC0000139,
// STATUS_ENTRYPOINT_NOT_FOUND, before reaching main.
//
// The CMake side delay-loads mfsensorgroup for a named list of targets. This
// test does not trust that list -- it reads the built PE files and checks
// where the dependency actually landed, which is the only thing that decides
// whether Sync starts on Windows 10.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_harness.hpp"

namespace {

// The binaries a user actually receives, plus the probes and camera tests that
// link the same libraries. Anything here must reach Windows 10 far enough to
// report that the camera is unavailable.
constexpr const char* kShippedBinaries[] = {
    "syncd.exe",
    "Sync.exe",
    "SyncCamera.dll",
};

struct PeImage {
  std::vector<char> bytes;
  const IMAGE_NT_HEADERS64* nt = nullptr;
  const IMAGE_SECTION_HEADER* sections = nullptr;
  WORD section_count = 0;
};

[[nodiscard]] auto read_file(const std::filesystem::path& path) -> std::vector<char> {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Everything a PE data directory points at is an RVA -- an address in the
// image once the loader has mapped it, with each section at its virtual
// address. Reading the file on disk means walking the section table to find
// which section contains the RVA and applying that section's file offset.
[[nodiscard]] auto rva_to_offset(const PeImage& image, DWORD rva) -> std::size_t {
  for (WORD i = 0; i < image.section_count; ++i) {
    const IMAGE_SECTION_HEADER& section = image.sections[i];
    const DWORD size =
        section.SizeOfRawData > section.Misc.VirtualSize ? section.SizeOfRawData
                                                         : section.Misc.VirtualSize;
    if (rva >= section.VirtualAddress && rva < section.VirtualAddress + size) {
      return section.PointerToRawData + (rva - section.VirtualAddress);
    }
  }
  return 0;
}

[[nodiscard]] auto parse_pe(const std::filesystem::path& path) -> PeImage {
  PeImage image;
  image.bytes = read_file(path);
  if (image.bytes.size() < sizeof(IMAGE_DOS_HEADER)) return {};

  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.bytes.data());
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
  if (static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > image.bytes.size()) {
    return {};
  }

  image.nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.bytes.data() + dos->e_lfanew);
  if (image.nt->Signature != IMAGE_NT_SIGNATURE) return {};
  image.sections = IMAGE_FIRST_SECTION(image.nt);
  image.section_count = image.nt->FileHeader.NumberOfSections;
  return image;
}

[[nodiscard]] auto name_at(const PeImage& image, DWORD rva) -> std::string {
  const std::size_t offset = rva_to_offset(image, rva);
  if (offset == 0 || offset >= image.bytes.size()) return {};
  const char* start = image.bytes.data() + offset;
  const std::size_t available = image.bytes.size() - offset;
  const std::size_t length = ::strnlen(start, available);
  return std::string(start, length);
}

[[nodiscard]] auto contains_ignoring_case(const std::string& haystack, const char* needle)
    -> bool {
  std::string lowered = haystack;
  for (char& c : lowered) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  return lowered.find(needle) != std::string::npos;
}

// The ordinary import directory: everything here is resolved by the loader
// before the process runs.
[[nodiscard]] auto normal_imports(const PeImage& image) -> std::vector<std::string> {
  std::vector<std::string> names;
  const IMAGE_DATA_DIRECTORY& directory =
      image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (directory.VirtualAddress == 0) return names;

  std::size_t offset = rva_to_offset(image, directory.VirtualAddress);
  if (offset == 0) return names;
  for (;; offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
    if (offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) > image.bytes.size()) break;
    const auto* descriptor =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(image.bytes.data() + offset);
    if (descriptor->Name == 0) break;
    names.push_back(name_at(image, descriptor->Name));
  }
  return names;
}

// The delay-load directory: resolved on first call instead, which is what
// lets the process start on a Windows that has no such export.
[[nodiscard]] auto delayed_imports(const PeImage& image) -> std::vector<std::string> {
  std::vector<std::string> names;
  const IMAGE_DATA_DIRECTORY& directory =
      image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
  if (directory.VirtualAddress == 0) return names;

  std::size_t offset = rva_to_offset(image, directory.VirtualAddress);
  if (offset == 0) return names;
  for (;; offset += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
    if (offset + sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) > image.bytes.size()) break;
    const auto* descriptor =
        reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(image.bytes.data() + offset);
    if (descriptor->DllNameRVA == 0) break;
    names.push_back(name_at(image, descriptor->DllNameRVA));
  }
  return names;
}

[[nodiscard]] auto binary_directory() -> std::filesystem::path {
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
  return std::filesystem::path(path).parent_path();
}

}  // namespace

SYNC_TEST(the_shipped_binaries_never_bind_the_virtual_camera_api_at_load_time) {
  const std::filesystem::path directory = binary_directory();
  SYNC_REQUIRE(!directory.empty());

  // Guards against the test quietly passing because it examined nothing --
  // a rename would otherwise turn "no binaries found" into green.
  int examined = 0;

  for (const char* name : kShippedBinaries) {
    const std::filesystem::path path = directory / name;
    if (!std::filesystem::exists(path)) continue;
    ++examined;

    const PeImage image = parse_pe(path);
    SYNC_REQUIRE(image.nt != nullptr);

    const std::vector<std::string> ordinary = normal_imports(image);
    // The interesting assertion below is a negative, so it would pass just as
    // happily if the parser never found the import directory at all. Every PE
    // here imports at least KERNEL32, so requiring a non-empty list proves the
    // walk actually reached the table it is supposed to be checking.
    SYNC_REQUIRE(!ordinary.empty());

    bool normal = false;
    for (const std::string& dll : ordinary) {
      if (contains_ignoring_case(dll, "mfsensorgroup")) normal = true;
    }
    bool delayed = false;
    for (const std::string& dll : delayed_imports(image)) {
      if (contains_ignoring_case(dll, "mfsensorgroup")) delayed = true;
    }

    if (normal) {
      std::printf("%s imports mfsensorgroup at load time: it cannot start below build 22000\n",
                  name);
    }
    SYNC_REQUIRE(!normal);
    // Delay-loaded is the expected state. A binary carrying neither has simply
    // stopped using the API, which is fine and must not fail here.
    if (delayed) {
      std::printf("%s delay-loads mfsensorgroup\n", name);
    }
  }

  SYNC_REQUIRE(examined > 0);
}
