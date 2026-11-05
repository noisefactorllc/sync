#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <sys/stat.h>
#endif

namespace noisefactor::sync::ndi {

// The runtime library paths NdiFramePublisher tries, in order, for an
// explicit runtime directory (empty when the caller named none), followed by
// the platform's own locations or, when given, `system_candidates`.
[[nodiscard]] auto runtime_candidates(std::string_view runtime_path,
                                      const std::vector<std::string>* system_candidates = nullptr)
    -> std::vector<std::string>;

#if defined(__APPLE__)
// The vendor's macOS runtime package installs the library here and sets no
// environment variable, so this is the only place Sync looks without an
// explicit --ndi-runtime.
inline constexpr std::string_view kVendorRuntimePath = "/usr/local/lib/libndi.dylib";

// NewTek, Inc.'s Apple Developer Team ID, from the Developer ID Application
// certificate that signs the vendor's libndi.dylib. syncd runs with library
// validation disabled, because the hardened runtime otherwise refuses a
// library signed by another team. This pin is what keeps that exception
// narrow: a candidate that is unsigned, ad hoc signed or signed by any other
// team is never loaded.
inline constexpr std::string_view kNdiRuntimeTeamId = "W8U66ET244";

// The code requirement built from kNdiRuntimeTeamId.
[[nodiscard]] auto vendor_runtime_requirement() -> std::string;

enum class RuntimeSignature : std::uint8_t { Accepted, Missing, Rejected };

enum class RuntimeOwnership : std::uint8_t { RootOwned, Missing, Exposed };

// RootOwned when `path` is a regular file, not a symbolic link, owned by
// root and writable by neither its group nor others, in a directory with the
// same owner and modes. Only root can then replace the file between the
// signature check and dlopen. Applied to kVendorRuntimePath only: the
// --ndi-runtime directory is a developer flag, which Sync.app never passes.
[[nodiscard]] auto check_runtime_ownership(const std::string& path) noexcept -> RuntimeOwnership;

// The same rule on the lstat results of the file and of its directory.
[[nodiscard]] auto runtime_ownership(const struct stat& file, const struct stat& directory) noexcept
    -> RuntimeOwnership;

// Checks a candidate's signature on disk, for every architecture it holds,
// against `requirement`, without loading it.
[[nodiscard]] auto check_runtime_signature(const std::string& path, const std::string& requirement) noexcept
    -> RuntimeSignature;
#endif

}  // namespace noisefactor::sync::ndi
