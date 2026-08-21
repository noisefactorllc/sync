#include <sync/platform/spout_publisher.hpp>

#include "../../replacement_budget.hpp"

// This is the one file in the Spout provider allowed to see Win32 types --
// the header deliberately does not, so every other translation unit that
// merely wants to hold a SpoutFramePublisher stays windows.h-free. See
// docs/dependencies/spout.md for the full runtime-discovery contract this
// file implements.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace noisefactor::sync {
namespace {

constexpr std::size_t kMaximumSenderIdBytes = 128;
constexpr std::size_t kMaximumSenderNameBytes = 64;
constexpr std::size_t kMaximumDiscoveryPaths = 4;
constexpr std::uint32_t kMaximumDimension = 4096;
constexpr std::uint32_t kMaximumPayloadBytes = 64U * 1024U * 1024U;

// GL_RGBA from the OpenGL 1.0 / GL/gl.h token space. We cannot include GL
// headers here (see the vendor boundary discussion in
// docs/dependencies/spout.md), and this token has been part of the stable
// OpenGL 1.x core enum space since 1992, so a literal is safe and does not
// need an upstream header to stay correct.
constexpr unsigned int kGlRgbaFormat = 0x1908U;

// ---------------------------------------------------------------------------
// Checked arithmetic (identical shape to metal_frame_publisher.mm's helper;
// duplicated rather than shared because the two platforms do not share a
// translation unit and this file must not pull in an Apple-only header).
// ---------------------------------------------------------------------------

auto checked_multiply(std::size_t left, std::size_t right, std::size_t& result) noexcept -> bool {
  if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
    return false;
  }
  result = left * right;
  return true;
}

// Same shape as metal_frame_publisher.mm's view_is_valid: version 1,
// header_bytes 64, flags 1, top_down, pixel_format 1 (RGBA8), color_space
// 1|2, alpha_mode 1|2|3, bounded dimensions, stride covers the packed row,
// and the payload size agrees with width/height/stride under checked
// arithmetic.
auto view_is_valid(const protocol::FrameView& frame) noexcept -> bool {
  if (frame.version != 1 || frame.header_bytes != 64 || frame.flags != 1 || !frame.top_down ||
      frame.pixel_format != 1 || (frame.color_space != 1 && frame.color_space != 2) ||
      (frame.alpha_mode != 1 && frame.alpha_mode != 2 && frame.alpha_mode != 3) ||
      frame.width == 0 || frame.height == 0 || frame.width > kMaximumDimension ||
      frame.height > kMaximumDimension) {
    return false;
  }

  std::size_t packed_row_bytes = 0;
  if (!checked_multiply(static_cast<std::size_t>(frame.width), 4U, packed_row_bytes) ||
      static_cast<std::size_t>(frame.row_stride) < packed_row_bytes) {
    return false;
  }
  std::size_t expected_payload_bytes = 0;
  if (!checked_multiply(static_cast<std::size_t>(frame.row_stride),
                        static_cast<std::size_t>(frame.height), expected_payload_bytes) ||
      expected_payload_bytes > kMaximumPayloadBytes ||
      expected_payload_bytes != static_cast<std::size_t>(frame.payload_bytes) ||
      expected_payload_bytes != frame.payload.size()) {
    return false;
  }
  return true;
}

// sender_id is Sync's internal correlation key, never shown to anyone and
// never handed to Spout, so it is only bounded. The bound must match the
// server's own 128-byte limit: capping it lower here would reject an id the
// server considers valid and fail the whole sender for every provider,
// because PublisherHub opens a sender across its providers as a unit.
auto valid_sender_id_bytes(std::string_view value) noexcept -> bool {
  return !value.empty() && value.size() <= kMaximumSenderIdBytes;
}

// The name IS handed to Spout's SetSenderName and is rendered in other
// applications' source pickers: 1..64 bytes, no control characters
// (0x00-0x1F, 0x7F), which also excludes an embedded NUL.
auto valid_sender_name_bytes(std::string_view value) noexcept -> bool {
  if (value.empty() || value.size() > kMaximumSenderNameBytes) {
    return false;
  }
  for (const char byte : value) {
    const auto unsigned_byte = static_cast<unsigned char>(byte);
    if (unsigned_byte < 0x20U || unsigned_byte == 0x7FU) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Transcribed SPOUTLIBRARY ABI.
//
// SpoutLibrary.dll's GetSpout() (documented signature: extern "C" SPOUTAPI
// SPOUTHANDLE WINAPI GetSpout(VOID);) returns a pointer to an object
// implementing the public SPOUTLIBRARY pure-virtual interface declared in
// Spout2's SpoutLibrary.h (BSD-2-Clause, Copyright (c) 2016-2025 Lynn
// Jarvis; see docs/dependencies/spout.md for the preserved notice). That
// interface is a stable C++ vtable published precisely so applications can
// call into it without compiling Spout2 from source, which is why mirroring
// its shape here is legitimate even though this repository's vendor
// boundary forbids vendoring the header itself.
//
// A virtual call dispatches by slot INDEX, not by name, so only the number
// and order of declarations below has to be right. The list is therefore a
// mechanical transcription of the real SPOUTLIBRARY declaration rather than
// a reconstruction: 172 slots, from SetSenderName at 0 to Release at 171.
// Methods Sync does not call are declared as correctly-ordered spacers named
// after the slot they occupy, so a reviewer can diff this list against the
// upstream header line for line.
//
// THE ONE BUILD-CONFIGURATION ASSUMPTION, and it is load-bearing:
// SpoutLibrary.h wraps six adapter-preference methods (slots 145-150) in
// `#ifdef NTDDI_WIN10_RS4`. That macro is defined by every Windows SDK from
// 10.0.17134 (2018) onward simply by including sdkddkver.h, so every
// realistic build of the DLL contains them -- but a DLL built without them
// has a 166-slot vtable in which CreateOpenGL, CloseOpenGL and Release sit
// six slots earlier, and calling slot 151 there would invoke an unrelated
// function with the wrong arguments. This mirror assumes the block IS
// present. Two things keep that assumption honest:
//   1. The installer ships a pinned SpoutLibrary.dll built by the Noise
//      Factor release workflow, so the shipped configuration is controlled
//      rather than discovered.
//   2. probe_abi() below reads the vtable entry at index 171 and refuses
//      the module unless it points into the module image. Treat that as a
//      cheap smoke test, NOT a proof: reading one entry past a shorter
//      vtable lands on whatever the linker happened to place next, and in
//      a DLL full of vtables and RTTI records that is quite likely to be
//      another in-image pointer, which would pass. It reliably rejects a
//      module that is not SPOUTLIBRARY at all; it does not reliably
//      distinguish a 166-slot build from a 172-slot one. The pinned DLL is
//      what actually carries that guarantee.
// See docs/dependencies/spout.md.

// OVERLOADED NAMES ARE NOT CALLABLE THROUGH THIS MIRROR.
//
// Five names in SPOUTLIBRARY are declared more than once: GetName (7, 105),
// SpoutMessageBox (75-81), SpoutMessageBoxIcon (82, 83), GetAdapterInfo
// (143, 144), and FlipBuffer (157, 158). For those, the slot a GCC-built
// mirror computes from declaration order does not agree with the slot the
// MSVC-built DLL actually uses, and calling one lands on a function with a
// different signature -- for GetName, on an overload returning std::string by
// value, whose hidden return-buffer argument makes the call corrupt the stack
// and crash.
//
// This is a property of the interface, not a mistake in this file: the
// vendor's own shipped SpoutLibrary.h, compiled here and run against the
// shipped 2.007.017 DLL, crashes on exactly the same call in exactly the same
// place. Every name Sync does call -- SetSenderName, ReleaseSender, SendImage,
// IsInitialized, CreateOpenGL, CloseOpenGL, Release -- is declared exactly
// once, and an overload set only permutes its own slots, so none of them are
// affected.
//
// If you extend this mirror, check the name is declared exactly once first.

// Index of Release() in the mirrored vtable. Named because probe_abi()
// checks this exact entry to validate the layout assumption above.
constexpr std::size_t kReleaseSlotIndex = 171;

class SpoutLibraryAbi {
 public:
  // Deliberately NO virtual destructor: declaring one would occupy slot 0
  // and shift every slot below it by one. This mirrors how the real
  // interface works -- lifetime is managed through Release(), never through
  // `delete` -- so a SpoutLibraryAbi* must never be deleted.

  virtual void SetSenderName(const char* sender_name) = 0;  // slot 000
  virtual void Slot001_SetSenderFormat() = 0;
  virtual void ReleaseSender(unsigned int milliseconds) = 0;  // slot 002
  virtual void Slot003_SendFbo() = 0;
  virtual void Slot004_SendTexture() = 0;
  virtual bool SendImage(const unsigned char* pixels, unsigned int width,
                        unsigned int height, unsigned int gl_format,
                        bool invert) = 0;  // slot 005
  virtual bool IsInitialized() = 0;  // slot 006
  // Spacer, NOT callable -- see the overload warning above. GetName is
  // declared twice in SPOUTLIBRARY (here and at 105), and which of the two
  // occupies which slot in an MSVC-built DLL does not match the declaration
  // order a GCC-built mirror assumes. Calling either one crashes; verified
  // against the shipped 2.007.017 DLL using the vendor's own header, so this
  // is a property of the interface and not of this mirror.
  virtual void Slot007_GetName_DO_NOT_CALL() = 0;
  virtual void Slot008_GetWidth() = 0;
  virtual void Slot009_GetHeight() = 0;
  virtual void Slot010_GetFps() = 0;
  virtual void Slot011_GetFrame() = 0;
  virtual void Slot012_GetHandle() = 0;
  virtual void Slot013_GetCPU() = 0;
  virtual void Slot014_GetGLDX() = 0;
  virtual void Slot015_SetReceiverName() = 0;
  virtual void Slot016_GetReceiverName() = 0;
  virtual void Slot017_ReleaseReceiver() = 0;
  virtual void Slot018_ReceiveTexture() = 0;
  virtual void Slot019_ReceiveImage() = 0;
  virtual void Slot020_IsUpdated() = 0;
  virtual void Slot021_IsConnected() = 0;
  virtual void Slot022_IsFrameNew() = 0;
  virtual void Slot023_GetSenderName() = 0;
  virtual void Slot024_GetSenderWidth() = 0;
  virtual void Slot025_GetSenderHeight() = 0;
  virtual void Slot026_GetSenderFormat() = 0;
  virtual void Slot027_GetSenderFps() = 0;
  virtual void Slot028_GetSenderFrame() = 0;
  virtual void Slot029_GetSenderHandle() = 0;
  virtual void Slot030_GetSenderTexture() = 0;
  virtual void Slot031_GetSenderCPU() = 0;
  virtual void Slot032_GetSenderGLDX() = 0;
  virtual void Slot033_GetHostPath() = 0;
  virtual void Slot034_GetSenderList() = 0;
  virtual void Slot035_SelectSender() = 0;
  virtual void Slot036_SelectSenderPanel() = 0;
  virtual void Slot037_SetFrameCount() = 0;
  virtual void Slot038_DisableFrameCount() = 0;
  virtual void Slot039_IsFrameCountEnabled() = 0;
  virtual void Slot040_HoldFps() = 0;
  virtual void Slot041_GetRefreshRate() = 0;
  virtual void Slot042_SetFrameSync() = 0;
  virtual void Slot043_WaitFrameSync() = 0;
  virtual void Slot044_EnableFrameSync() = 0;
  virtual void Slot045_CloseFrameSync() = 0;
  virtual void Slot046_IsFrameSyncEnabled() = 0;
  virtual void Slot047_GetVerticalSync() = 0;
  virtual void Slot048_SetVerticalSync() = 0;
  virtual void Slot049_WriteMemoryBuffer() = 0;
  virtual void Slot050_ReadMemoryBuffer() = 0;
  virtual void Slot051_CreateMemoryBuffer() = 0;
  virtual void Slot052_DeleteMemoryBuffer() = 0;
  virtual void Slot053_GetMemoryBufferSize() = 0;
  virtual void Slot054_OpenSpoutConsole() = 0;
  virtual void Slot055_CloseSpoutConsole() = 0;
  virtual void Slot056_EnableSpoutLog() = 0;
  virtual void Slot057_EnableSpoutLogFile() = 0;
  virtual void Slot058_DisableSpoutLogFile() = 0;
  virtual void Slot059_RemoveSpoutLogFile() = 0;
  virtual void Slot060_DisableSpoutLog() = 0;
  virtual void Slot061_DisableLogs() = 0;
  virtual void Slot062_EnableLogs() = 0;
  virtual void Slot063_LogsEnabled() = 0;
  virtual void Slot064_LogFileEnabled() = 0;
  virtual void Slot065_GetSpoutLogPath() = 0;
  virtual void Slot066_GetSpoutLog() = 0;
  virtual void Slot067_ShowSpoutLogs() = 0;
  virtual void Slot068_SetSpoutLogLevel() = 0;
  virtual void Slot069_SpoutLog() = 0;
  virtual void Slot070_SpoutLogVerbose() = 0;
  virtual void Slot071_SpoutLogNotice() = 0;
  virtual void Slot072_SpoutLogWarning() = 0;
  virtual void Slot073_SpoutLogError() = 0;
  virtual void Slot074_SpoutLogFatal() = 0;
  virtual void Slot075_SpoutMessageBox() = 0;
  virtual void Slot076_SpoutMessageBox_2() = 0;
  virtual void Slot077_SpoutMessageBox_3() = 0;
  virtual void Slot078_SpoutMessageBox_4() = 0;
  virtual void Slot079_SpoutMessageBox_5() = 0;
  virtual void Slot080_SpoutMessageBox_6() = 0;
  virtual void Slot081_SpoutMessageBox_7() = 0;
  virtual void Slot082_SpoutMessageBoxIcon() = 0;
  virtual void Slot083_SpoutMessageBoxIcon_2() = 0;
  virtual void Slot084_SpoutMessageBoxButton() = 0;
  virtual void Slot085_SpoutMessageBoxModeless() = 0;
  virtual void Slot086_SpoutMessageBoxWindow() = 0;
  virtual void Slot087_SpoutMessageBoxPosition() = 0;
  virtual void Slot088_CopyToClipBoard() = 0;
  virtual void Slot089_OpenSpoutLogs() = 0;
  virtual void Slot090_ReadDwordFromRegistry() = 0;
  virtual void Slot091_WriteDwordToRegistry() = 0;
  virtual void Slot092_ReadPathFromRegistry() = 0;
  virtual void Slot093_WritePathToRegistry() = 0;
  virtual void Slot094_WriteBinaryToRegistry() = 0;
  virtual void Slot095_RemovePathFromRegistry() = 0;
  virtual void Slot096_RemoveSubKey() = 0;
  virtual void Slot097_FindSubKey() = 0;
  virtual void Slot098_GetSDKversion() = 0;
  virtual void Slot099_IsLaptop() = 0;
  virtual void Slot100_GetCurrentModule() = 0;
  virtual void Slot101_GetExeVersion() = 0;
  virtual void Slot102_GetExePath() = 0;
  virtual void Slot103_GetExeName() = 0;
  virtual void Slot104_GetPath() = 0;
  virtual void Slot105_GetName_2() = 0;
  virtual void Slot106_StartTiming() = 0;
  virtual void Slot107_EndTiming() = 0;
  virtual void Slot108_BindSharedTexture() = 0;
  virtual void Slot109_UnBindSharedTexture() = 0;
  virtual void Slot110_GetSharedTextureID() = 0;
  virtual void Slot111_GetSenderCount() = 0;
  virtual void Slot112_GetSender() = 0;
  virtual void Slot113_FindSenderName() = 0;
  virtual void Slot114_GetSenderInfo() = 0;
  virtual void Slot115_GetActiveSender() = 0;
  virtual void Slot116_SetActiveSender() = 0;
  virtual void Slot117_GetBufferMode() = 0;
  virtual void Slot118_SetBufferMode() = 0;
  virtual void Slot119_GetBuffers() = 0;
  virtual void Slot120_SetBuffers() = 0;
  virtual void Slot121_GetMaxSenders() = 0;
  virtual void Slot122_SetMaxSenders() = 0;
  virtual void Slot123_CreateSender() = 0;
  virtual void Slot124_UpdateSender() = 0;
  virtual void Slot125_CreateReceiver() = 0;
  virtual void Slot126_CheckReceiver() = 0;
  virtual void Slot127_GetDX9() = 0;
  virtual void Slot128_SetDX9() = 0;
  virtual void Slot129_GetMemoryShareMode() = 0;
  virtual void Slot130_SetMemoryShareMode() = 0;
  virtual void Slot131_GetCPUmode() = 0;
  virtual void Slot132_SetCPUmode() = 0;
  virtual void Slot133_GetShareMode() = 0;
  virtual void Slot134_SetShareMode() = 0;
  virtual void Slot135_GetAutoShare() = 0;
  virtual void Slot136_SetAutoShare() = 0;
  virtual void Slot137_SetCPUshare() = 0;
  virtual void Slot138_IsGLDXready() = 0;
  virtual void Slot139_GetNumAdapters() = 0;
  virtual void Slot140_GetAdapterName() = 0;
  virtual void Slot141_AdapterName() = 0;
  virtual void Slot142_GetAdapter() = 0;
  virtual void Slot143_GetAdapterInfo() = 0;
  virtual void Slot144_GetAdapterInfo_2() = 0;
  virtual void Slot145_GetPerformancePreference() = 0;  // NTDDI_WIN10_RS4
  virtual void Slot146_SetPerformancePreference() = 0;  // NTDDI_WIN10_RS4
  virtual void Slot147_GetPreferredAdapterName() = 0;  // NTDDI_WIN10_RS4
  virtual void Slot148_SetPreferredAdapter() = 0;  // NTDDI_WIN10_RS4
  virtual void Slot149_IsPreferenceAvailable() = 0;  // NTDDI_WIN10_RS4
  virtual void Slot150_IsApplicationPath() = 0;  // NTDDI_WIN10_RS4
  virtual bool CreateOpenGL(void* window) = 0;  // slot 151
  virtual bool CloseOpenGL() = 0;  // slot 152
  virtual void Slot153_InitTexture() = 0;
  virtual void Slot154_CopyTexture() = 0;
  virtual void Slot155_ReadTextureData() = 0;
  virtual void Slot156_ClearAlpha() = 0;
  virtual void Slot157_FlipBuffer() = 0;
  virtual void Slot158_FlipBuffer_2() = 0;
  virtual void Slot159_GetDX11format() = 0;
  virtual void Slot160_SetDX11format() = 0;
  virtual void Slot161_DX11format() = 0;
  virtual void Slot162_GLDXformat() = 0;
  virtual void Slot163_GLformat() = 0;
  virtual void Slot164_GLformatName() = 0;
  virtual void Slot165_OpenDirectX() = 0;
  virtual void Slot166_CloseDirectX() = 0;
  virtual void Slot167_OpenDirectX11() = 0;
  virtual void Slot168_CloseDirectX11() = 0;
  virtual void Slot169_GetDX11Device() = 0;
  virtual void Slot170_GetDX11Context() = 0;
  virtual void Release() = 0;  // slot 171
};

// Documented C export: extern "C" SPOUTAPI SPOUTHANDLE WINAPI GetSpout(VOID);
// SPOUTHANDLE is a typedef for SPOUTLIBRARY*, i.e. a plain __stdcall
// function taking no arguments and returning a pointer we reinterpret as
// our local SpoutLibraryAbi*.
using GetSpoutFn = void*(WINAPI*)();

// ---------------------------------------------------------------------------
// Discovery: bounded, deduplicated, never trusts the legacy current-directory
// search order.
// ---------------------------------------------------------------------------

// CLI arguments arrive as UTF-8 (the shell/harness convention this repo
// otherwise uses); Win32 path APIs need UTF-16, so this converts properly
// instead of the naive byte-per-wchar_t widening that would corrupt any
// non-ASCII path.
auto widen_utf8(std::string_view value, std::wstring& out) noexcept -> bool {
  if (value.empty() || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
  if (required <= 0) {
    return false;
  }
  try {
    out.resize(static_cast<std::size_t>(required));
  } catch (const std::exception&) {
    return false;
  }
  const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), out.data(), required);
  if (written != required) {
    out.clear();
    return false;
  }
  return true;
}

// Trims a full module path down to its containing directory (in place,
// keeping the trailing backslash) so it can be joined with "SpoutLibrary.dll".
auto directory_of_running_executable(std::wstring& path) noexcept -> bool {
  std::array<wchar_t, MAX_PATH> buffer{};
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                          static_cast<DWORD>(buffer.size()));
  // GetModuleFileNameW does not null-terminate on truncation (length ==
  // buffer.size()) prior to Windows 10 1607; treat that as failure rather
  // than risk operating on a silently-truncated path.
  if (length == 0 || length >= buffer.size()) {
    return false;
  }
  std::wstring_view view(buffer.data(), length);
  const std::size_t separator = view.find_last_of(L"\\/");
  if (separator == std::wstring_view::npos) {
    return false;
  }
  path.assign(view.substr(0, separator + 1));
  path.append(L"SpoutLibrary.dll");
  return true;
}

// Adds a candidate path to the bounded, deduplicated search list. Silently
// drops candidates once the (small, fixed) capacity is reached rather than
// growing -- an unbounded discovery list is itself an attack surface.
void add_candidate(std::array<std::wstring, kMaximumDiscoveryPaths>& paths,
                   std::size_t& count,
                   std::wstring value) noexcept {
  if (value.empty() || count >= paths.size()) {
    return;
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (paths[index] == value) {
      return;
    }
  }
  try {
    paths[count] = std::move(value);
    ++count;
  } catch (const std::exception&) {
    // std::wstring assignment can throw bad_alloc; treat as "candidate not
    // added" rather than let it escape this noexcept-adjacent discovery path.
  }
}

// Loads a candidate DLL without ever falling back to the legacy DLL search
// order, which includes the process's current working directory. Loading a
// module executes its code (DllMain, static initializers), and the current
// directory is attacker-influenced (e.g. "open with" from an arbitrary
// folder, a dropped file, a shell working directory inherited from an
// untrusted launcher) -- so LOAD_LIBRARY_SEARCH_DEFAULT_DIRS is mandatory,
// and for an absolute path we add LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR so the
// DLL's own directory (the installer's pinned copy, or an explicit
// --spout-library path) is trusted too. We deliberately do not add any
// user-writable directory to the search set.
auto load_candidate(const std::wstring& path) noexcept -> HMODULE {
  const bool is_absolute = path.find_first_of(L"\\/") != std::wstring::npos;
  DWORD flags = LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
  if (is_absolute) {
    flags |= LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR;
  }
  return LoadLibraryExW(path.c_str(), nullptr, flags);
}

}  // namespace

struct SpoutFramePublisher::Impl {
  struct SenderEntry {
    bool occupied = false;
    std::size_t sender_id_length = 0;
    std::array<char, kMaximumSenderIdBytes> sender_id{};
    SpoutLibraryAbi* handle = nullptr;
    // Owned repack buffer, used only when the incoming row_stride does not
    // match the tightly-packed width SendImage requires. Sized on demand,
    // never shrunk (so a later frame at the same or smaller size is a
    // no-op), and counted against allocated_bytes.
    std::unique_ptr<std::byte[]> repack_buffer;
    std::size_t repack_capacity_bytes = 0;

    [[nodiscard]] auto id_view() const noexcept -> std::string_view {
      return {sender_id.data(), sender_id_length};
    }
  };

  explicit Impl(SpoutFramePublisher::Options options)
      : allocation_budget_bytes(options.allocation_budget_bytes) {
    if (options.allocation_budget_bytes > SpoutFramePublisher::kProductAllocationBudgetBytes) {
      configuration_valid = false;
      return;
    }
    discover_and_initialize(options.library_path);
  }

  ~Impl() { teardown(); }

  Impl(const Impl&) = delete;
  auto operator=(const Impl&) -> Impl& = delete;

  HMODULE module = nullptr;
  GetSpoutFn get_spout = nullptr;
  SpoutLibraryAbi* primary = nullptr;
  DWORD constructing_thread_id = 0;
  bool configuration_valid = true;
  bool gl_context_created = false;
  std::size_t allocation_budget_bytes = 0;
  std::size_t allocated_bytes = 0;
  std::optional<ProviderFailure> latched_failure;
  std::array<SenderEntry, SpoutFramePublisher::kMaximumSenderEntries> senders{};

  [[nodiscard]] auto available() const noexcept -> bool {
    return configuration_valid && module != nullptr && primary != nullptr && gl_context_created;
  }

  // The GL context CreateOpenGL() establishes is thread-affine (it is
  // wglMakeCurrent'd on whichever thread called CreateOpenGL). SendImage
  // relies on that context being current on the calling thread. The daemon
  // publishes from a single libuv loop thread in production, so this check
  // never trips there -- but if it ever did (a bug, a future refactor that
  // moves publish() onto a worker thread), calling into Spout from the
  // wrong thread would silently corrupt GPU state rather than fail. Turning
  // that into an honest, explicit failure is worth the GetCurrentThreadId()
  // call on every entry point.
  [[nodiscard]] auto on_constructing_thread() const noexcept -> bool {
    return GetCurrentThreadId() == constructing_thread_id;
  }

  void latch_failure(ProviderFailureKind kind, std::uint32_t native_status = 0,
                     std::int64_t native_error_code = 0) noexcept {
    if (!latched_failure.has_value()) {
      latched_failure = ProviderFailure{
          .kind = kind, .native_status = native_status, .native_error_code = native_error_code};
    }
  }

  [[nodiscard]] auto find_sender(std::string_view sender_id) noexcept -> SenderEntry* {
    for (SenderEntry& entry : senders) {
      if (entry.occupied && entry.sender_id_length == sender_id.size() &&
          entry.id_view() == sender_id) {
        return &entry;
      }
    }
    return nullptr;
  }

  // Releases one sender's Spout instance (ReleaseSender then Release, per
  // the design contract) and its repack buffer's share of the allocation
  // budget, then clears the slot.
  void release_and_clear(SenderEntry& entry) noexcept {
    if (!entry.occupied) {
      return;
    }
    if (entry.handle != nullptr) {
      entry.handle->ReleaseSender(0U);
      entry.handle->Release();
      entry.handle = nullptr;
    }
    allocated_bytes -= entry.repack_capacity_bytes;
    entry = SenderEntry{};
  }

  // Grows (never shrinks) entry's repack buffer to at least needed_bytes,
  // honoring the shared allocation budget across all senders. Mirrors
  // MetalFramePublisher::ensure_ring's budget bookkeeping, minus the ring
  // (Spout's SendImage is synchronous, so one buffer per sender suffices).
  [[nodiscard]] auto ensure_repack_buffer(SenderEntry& entry, std::size_t needed_bytes) noexcept
      -> bool {
    if (entry.repack_capacity_bytes >= needed_bytes) {
      return true;
    }
    const auto replacement_total = allocation::replacement_total_if_peak_fits(
        allocated_bytes, entry.repack_capacity_bytes, needed_bytes,
        allocation_budget_bytes);
    if (!replacement_total.has_value()) {
      return false;
    }
    std::unique_ptr<std::byte[]> replacement;
    try {
      replacement = std::make_unique<std::byte[]>(needed_bytes);
    } catch (const std::exception&) {
      return false;
    }
    entry.repack_buffer = std::move(replacement);
    entry.repack_capacity_bytes = needed_bytes;
    allocated_bytes = *replacement_total;
    return true;
  }

  void discover_and_initialize(std::string_view explicit_library_path) noexcept {
    std::array<std::wstring, kMaximumDiscoveryPaths> candidates{};
    std::size_t candidate_count = 0;

    // (a) explicit --spout-library path.
    if (!explicit_library_path.empty()) {
      std::wstring widened;
      if (widen_utf8(explicit_library_path, widened)) {
        add_candidate(candidates, candidate_count, std::move(widened));
      }
    }

    // (b) the directory containing the running executable -- where the
    // installer places its pinned, redistributed copy (see
    // docs/dependencies/spout.md).
    std::wstring exe_relative_path;
    if (directory_of_running_executable(exe_relative_path)) {
      add_candidate(candidates, candidate_count, std::move(exe_relative_path));
    }

    // (c) the bare name, letting the loader apply its safe (non-legacy)
    // default search directories. Deliberately no per-user or per-process
    // writable directory is ever added here.
    add_candidate(candidates, candidate_count, L"SpoutLibrary.dll");

    // Two checks, in order of what they can prove.
    //
    // First, the vtable's extent. Every entry of a real vtable points at code
    // inside the module that defines it. A DLL built WITHOUT the
    // NTDDI_WIN10_RS4 block has a 166-entry vtable, so the entry this build
    // calls Release (index 171) would lie past its end and would not point
    // into the module's image. Checking that directly is the only way to test
    // the one build-configuration assumption this mirror makes, because the
    // two layouts are byte-identical over the early slots and no behavioural
    // probe can tell them apart.
    //
    // Second, behaviour on the early, unconditional slots (0-7), which catches
    // a module that is not SPOUTLIBRARY at all: a fresh instance has no
    // sender, so IsInitialized() must be false and GetName() must read back
    // whatever SetSenderName() was given.
    const auto probe_abi = [](SpoutLibraryAbi* candidate,
                              HMODULE candidate_module) noexcept -> bool {
      const auto* image = reinterpret_cast<const unsigned char*>(candidate_module);
      const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
      if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
      const auto* nt =
          reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
      if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
      const unsigned char* image_end = image + nt->OptionalHeader.SizeOfImage;

      // Reading one entry past a short vtable stays well inside the module's
      // mapped data section, so this read is safe even when the answer is
      // "that entry is not a function pointer".
      const auto* const* vtable =
          *reinterpret_cast<const void* const* const*>(candidate);
      const auto in_image = [&](const void* address) noexcept {
        const auto* byte = static_cast<const unsigned char*>(address);
        return byte >= image && byte < image_end;
      };
      if (!in_image(vtable) || !in_image(vtable[0]) ||
          !in_image(vtable[kReleaseSlotIndex])) {
        return false;
      }

      // A fresh SPOUTLIBRARY instance has no sender, so this must be false.
      // It is one real call through the vtable returning a known value, which
      // is what distinguishes a genuine module from something that merely has
      // a plausible-looking export.
      //
      // Deliberately nothing more than this. See the overload warning on the
      // mirror above: the obvious next check -- SetSenderName() then GetName()
      // to see the name round-trip -- cannot be written, because GetName is
      // overloaded and its slot therefore cannot be relied on.
      return !candidate->IsInitialized();
    };

    for (std::size_t index = 0; index < candidate_count; ++index) {
      HMODULE candidate_module = load_candidate(candidates[index]);
      if (candidate_module == nullptr) {
        continue;
      }
      auto* candidate_get_spout =
          // Through void*: GetProcAddress returns FARPROC, and casting one
          // function-pointer type straight to another of a different signature
          // is what -Wcast-function-type flags. Bridging via void* is the
          // documented way to do this, and matches ndi_publisher.cpp.
          reinterpret_cast<GetSpoutFn>(
              reinterpret_cast<void*>(GetProcAddress(candidate_module, "GetSpout")));
      if (candidate_get_spout == nullptr) {
        FreeLibrary(candidate_module);
        continue;
      }
      void* raw_handle = candidate_get_spout();
      if (raw_handle == nullptr) {
        FreeLibrary(candidate_module);
        continue;
      }
      auto* candidate_primary = static_cast<SpoutLibraryAbi*>(raw_handle);
      if (!probe_abi(candidate_primary, candidate_module)) {
        // The vtable is not the one this build mirrors, so no slot on it can
        // be trusted -- including Release(). Leaking one object and leaving
        // the module mapped is the only safe exit: calling a wrong slot to
        // clean up would be exactly the corruption the probe just prevented.
        continue;
      }
      // CreateOpenGL() must run on the thread that will later call
      // SendImage; that is this constructing thread by contract (see
      // on_constructing_thread()).
      if (!candidate_primary->CreateOpenGL(nullptr)) {
        candidate_primary->Release();
        FreeLibrary(candidate_module);
        continue;
      }
      module = candidate_module;
      get_spout = candidate_get_spout;
      primary = candidate_primary;
      constructing_thread_id = GetCurrentThreadId();
      gl_context_created = true;
      return;
    }
    // No candidate worked: absence of the DLL, a failed ABI probe, or a
    // failed CreateOpenGL is never an error (see class-level docs on
    // available()) -- we simply leave module/primary null.
  }

  void teardown() noexcept {
    // Guarded at every step so a partially constructed Impl (e.g.
    // configuration_valid == false, discovery never ran) tears down safely.
    //
    // ReleaseSender, CloseOpenGL, and Release all run against the OpenGL
    // context CreateOpenGL made current on the constructing thread, so they
    // are only safe on that same thread. The daemon constructs and destroys
    // this publisher on its single loop thread, so the guard below never
    // fires in practice; if it ever did, leaking the senders and the module
    // is the right answer, because tearing a thread-affine GL context down
    // from the wrong thread corrupts state rather than failing cleanly.
    const bool can_touch_gl = !gl_context_created || on_constructing_thread();
    if (can_touch_gl) {
      for (std::size_t index = senders.size(); index > 0; --index) {
        release_and_clear(senders[index - 1U]);
      }
      if (primary != nullptr) {
        if (gl_context_created) {
          primary->CloseOpenGL();
        }
        primary->Release();
      }
    }
    primary = nullptr;
    // The module stays mapped when it could not be torn down cleanly: a
    // FreeLibrary here would unload code the leaked objects still point at.
    if (module != nullptr && can_touch_gl) {
      FreeLibrary(module);
    }
    module = nullptr;
    gl_context_created = false;
  }
};

SpoutFramePublisher::SpoutFramePublisher() : SpoutFramePublisher(Options{}) {}

SpoutFramePublisher::SpoutFramePublisher(Options options)
    : impl_(std::make_unique<Impl>(options)) {}

SpoutFramePublisher::~SpoutFramePublisher() = default;

auto SpoutFramePublisher::available() const noexcept -> bool {
  return impl_ != nullptr && impl_->available();
}

auto SpoutFramePublisher::open_sender(std::string_view sender_id,
                                      std::string_view name) noexcept -> bool {
  // Shape/content validation runs unconditionally, before the availability
  // check: it is cheap, and -- unlike almost everything else in this file
  // -- it is exercisable in a CI environment that never has real Spout
  // installed (see spout_publisher_test.cpp). Ordering it first does not
  // change any observable result (both paths return false), only which
  // guard a given rejection is attributable to.
  if (!valid_sender_id_bytes(sender_id) || !valid_sender_name_bytes(name)) {
    return false;
  }
  if (impl_ == nullptr || !impl_->available() || !impl_->on_constructing_thread() ||
      impl_->find_sender(sender_id) != nullptr) {
    return false;
  }

  Impl::SenderEntry* entry = nullptr;
  for (Impl::SenderEntry& candidate : impl_->senders) {
    if (!candidate.occupied) {
      entry = &candidate;
      break;
    }
  }
  if (entry == nullptr) {
    return false;
  }

  // One SPOUTHANDLE per sender: Spout binds a sender name to an instance,
  // so each concurrently-open sender needs its own handle from GetSpout().
  // Only the shared GL context (created once, above) is reused.
  void* raw_handle = impl_->get_spout();
  if (raw_handle == nullptr) {
    impl_->latch_failure(ProviderFailureKind::SpoutInitializationFailed);
    return false;
  }
  auto* handle = static_cast<SpoutLibraryAbi*>(raw_handle);

  // SetSenderName needs a NUL-terminated owned copy; name is only borrowed
  // for the duration of this call per the FramePublisher contract.
  std::array<char, kMaximumSenderNameBytes + 1U> name_buffer{};
  std::memcpy(name_buffer.data(), name.data(), name.size());
  name_buffer[name.size()] = '\0';
  handle->SetSenderName(name_buffer.data());

  entry->occupied = true;
  entry->sender_id_length = sender_id.size();
  std::memcpy(entry->sender_id.data(), sender_id.data(), sender_id.size());
  entry->handle = handle;
  return true;
}

void SpoutFramePublisher::close_sender(std::string_view sender_id) noexcept {
  if (impl_ == nullptr || !impl_->available() || !impl_->on_constructing_thread()) {
    return;
  }
  Impl::SenderEntry* entry = impl_->find_sender(sender_id);
  if (entry != nullptr) {
    impl_->release_and_clear(*entry);
  }
}

auto SpoutFramePublisher::publish(std::string_view sender_id,
                                  const protocol::FrameView& frame) noexcept -> PublishResult {
  // Same ordering rationale as open_sender: frame-shape validation runs
  // unconditionally so it is exercisable without a live Spout install.
  if (!view_is_valid(frame)) {
    return PublishResult::Failed;
  }
  if (impl_ == nullptr || !impl_->available() || !impl_->on_constructing_thread()) {
    return PublishResult::Failed;
  }
  Impl::SenderEntry* entry = impl_->find_sender(sender_id);
  if (entry == nullptr || entry->handle == nullptr) {
    return PublishResult::Failed;
  }

  const std::size_t packed_row_bytes = static_cast<std::size_t>(frame.width) * 4U;
  const unsigned char* pixels = nullptr;
  if (static_cast<std::size_t>(frame.row_stride) == packed_row_bytes) {
    // Fast path: the payload is already tightly packed. SendImage is
    // synchronous (Spout uploads and returns before this call returns), and
    // the FramePublisher contract only guarantees frame.payload for the
    // duration of this call -- both of which are exactly satisfied by
    // passing the borrowed pointer straight through, so no copy is needed.
    pixels = reinterpret_cast<const unsigned char*>(frame.payload.data());
  } else {
    std::size_t needed_bytes = 0;
    if (!checked_multiply(packed_row_bytes, static_cast<std::size_t>(frame.height),
                          needed_bytes) ||
        !impl_->ensure_repack_buffer(*entry, needed_bytes)) {
      return PublishResult::Failed;
    }
    auto* destination = entry->repack_buffer.get();
    const std::byte* source = frame.payload.data();
    for (std::uint32_t row = 0; row < frame.height; ++row) {
      std::memcpy(destination + static_cast<std::size_t>(row) * packed_row_bytes,
                 source + static_cast<std::size_t>(row) * frame.row_stride, packed_row_bytes);
    }
    pixels = reinterpret_cast<const unsigned char*>(destination);
  }

  // bInvert asks Spout to flip the buffer while copying it into the shared
  // DirectX texture. Sync's protocol guarantees row 0 is the topmost row, and
  // a DXGI texture's row 0 is likewise its top row, so the rows already line
  // up and no flip is wanted. This is also SendImage's own default in
  // SpoutLibrary.h (`bool bInvert = false`), unlike SendTexture, which
  // defaults to true because an OpenGL texture's origin is at the bottom.
  // VERIFY: orientation is the one property no unit test can confirm; check a
  // real receiver once, and if the image is upside down this is the line.
  constexpr bool kInvertTopDownInput = false;
  const bool sent = entry->handle->SendImage(
      pixels, frame.width, frame.height, kGlRgbaFormat, kInvertTopDownInput);
  if (!sent) {
    impl_->latch_failure(ProviderFailureKind::SpoutSendFailed);
    return PublishResult::Failed;
  }
  return PublishResult::Accepted;
}

auto SpoutFramePublisher::poll_failure(std::uint64_t now_ms) noexcept
    -> std::optional<ProviderFailure> {
  (void)now_ms;
  if (impl_ == nullptr) {
    return std::nullopt;
  }
  return impl_->latched_failure;
}

}  // namespace noisefactor::sync
