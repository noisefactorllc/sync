#include <sync/platform/ndi_publisher.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// ============================================================================
// Runtime boundary
// ============================================================================
// Sync never links the NDI SDK and never vendors its headers: the NDI SDK
// licence forbids redistributing the runtime, and the SDK's own header would
// pull in a build-time dependency this project does not own. Instead we
// locate a user-installed runtime at process start and resolve the single
// documented dynamic-load entry point, `NDIlib_v5_load`, exactly the way the
// vendor's own dynamic-load header (Processing.NDI.DynamicLoad.h) does it.
//
// ABI evidence: the vendor's current dynamic-load header defines every
// versioned name (NDIlib_v2 .. NDIlib_v6) as a typedef alias of the SAME
// growing struct — fields are appended at the end as the API evolves and are
// never reordered or removed. That means a struct containing only the
// documented v1.5..v3 prefix (through `send_send_video_async_v2`) is a
// layout-compatible PREFIX of whatever the installed runtime's real struct
// is, regardless of which SDK version is actually installed, as long as the
// fields we declare match the public header's field order exactly. This
// file was written after fetching and cross-checking that field order
// against the vendor's MIT-licensed `Processing.NDI.DynamicLoad.h`,
// `Processing.NDI.structs.h`, and `Processing.NDI.Send.h` (which are
// licensed for exactly this kind of reproduction; note the MIT grant there
// covers those files only, not the SDK/runtime itself) — see
// docs/dependencies/ndi.md for the field-order table a reviewer can use to
// re-audit this against the header directly. It is still a hand-reproduced
// ABI mirror rather than the header itself, so every field below is called
// out with // VERIFY where a mistake would be silent and dangerous.
namespace noisefactor::sync {
namespace {

// ---------------------------------------------------------------------------
// Dynamic module loading (platform-specific, kept to two small helpers)
// ---------------------------------------------------------------------------
// Loading a module executes its code: static initializers on POSIX, DllMain
// on Windows. Every directory we search must therefore already be one this
// process trusts before we start, not one an unrelated writable location
// could plant a file into. We search only: an explicit path the operator
// passed on the command line, the vendor's own documented discovery
// environment variables (which the NDI installer itself populates), and
// finally the bare library name, which defers to the OS loader's own
// default search rules. This mirrors the reasoning in syphon_consumer.mm
// for Syphon.framework discovery on macOS.

#if defined(_WIN32)
using native_module_handle = HMODULE;
constexpr native_module_handle kNullModule = nullptr;

[[nodiscard]] auto widen_utf8(const std::string& utf8, std::wstring& out) noexcept -> bool {
  if (utf8.empty()) {
    out.clear();
    return true;
  }
  if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  const int source_length = static_cast<int>(utf8.size());
  const int required =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), source_length, nullptr, 0);
  if (required <= 0) {
    return false;
  }
  out.resize(static_cast<std::size_t>(required));
  const int written =
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), source_length, out.data(), required);
  return written == required;
}

[[nodiscard]] auto load_module(const std::string& path, bool has_directory) noexcept -> native_module_handle {
  std::wstring wide_path;
  if (!widen_utf8(path, wide_path)) {
    return kNullModule;
  }
  // A path with a directory component needs the newer search flags so the
  // DLL's own dependent DLLs (also shipped by the vendor, alongside it) are
  // found next to it rather than only along the process's default search
  // path; a bare library name should fall through to the OS loader's
  // ordinary default search instead.
  const DWORD flags = has_directory
                          ? (LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)
                          : 0;
  return ::LoadLibraryExW(wide_path.c_str(), nullptr, flags);
}

void unload_module(native_module_handle handle) noexcept {
  if (handle != kNullModule) {
    ::FreeLibrary(handle);
  }
}

[[nodiscard]] auto resolve_symbol(native_module_handle handle, const char* name) noexcept -> void* {
  if (handle == kNullModule) {
    return nullptr;
  }
  // GetProcAddress returns FARPROC; converting a code pointer through a
  // data-pointer-shaped void* is the documented way to bridge it to our
  // typed loader signature below, matching how every dynamic-load consumer
  // of this ABI does it.
  return reinterpret_cast<void*>(::GetProcAddress(handle, name));
}

[[nodiscard]] auto platform_library_filename() noexcept -> std::string_view {
  return "Processing.NDI.Lib.x64.dll";
}

constexpr char kPathSeparator = '\\';

[[nodiscard]] auto path_has_directory(std::string_view path) noexcept -> bool {
  return path.find('\\') != std::string_view::npos || path.find('/') != std::string_view::npos;
}

#else
using native_module_handle = void*;
constexpr native_module_handle kNullModule = nullptr;

[[nodiscard]] auto load_module(const std::string& path, bool /*has_directory*/) noexcept
    -> native_module_handle {
  return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}

void unload_module(native_module_handle handle) noexcept {
  if (handle != kNullModule) {
    ::dlclose(handle);
  }
}

[[nodiscard]] auto resolve_symbol(native_module_handle handle, const char* name) noexcept -> void* {
  return handle == kNullModule ? nullptr : ::dlsym(handle, name);
}

#if defined(__APPLE__)
[[nodiscard]] auto platform_library_filename() noexcept -> std::string_view {
  return "libndi.dylib";
}
#else
[[nodiscard]] auto platform_library_filename() noexcept -> std::string_view {
  return "libndi.so.5";
}
#endif

constexpr char kPathSeparator = '/';

[[nodiscard]] auto path_has_directory(std::string_view path) noexcept -> bool {
  return path.find('/') != std::string_view::npos;
}

#endif

[[nodiscard]] auto read_env_var(const char* name, std::string& out) noexcept -> bool {
#if defined(_WIN32)
  char* buffer = nullptr;
  std::size_t length = 0;
  // _dupenv_s is the CRT-safe replacement for getenv on Windows: it never
  // triggers the deprecation warning getenv does there, and it reports
  // allocation failure instead of relying on a shared static buffer.
  if (::_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr) {
    if (buffer != nullptr) {
      std::free(buffer);
    }
    return false;
  }
  const bool has_value = length > 0;
  if (has_value) {
    out.assign(buffer, length - 1);  // length counts the trailing NUL.
  }
  std::free(buffer);
  return has_value && !out.empty();
#else
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return false;
  }
  out.assign(value);
  return true;
#endif
}

// ---------------------------------------------------------------------------
// Minimal, hand-reproduced prefix of the NDIlib_v5 dynamic-load ABI.
// ---------------------------------------------------------------------------
// Field order below matches Processing.NDI.DynamicLoad.h exactly, field for
// field, from `initialize` through `send_send_video_async_v2` (52 fields).
// Every field we do not call is declared as an opaque `void*` sized and
// named after the SDK field it stands in for; this assumes function
// pointers and object pointers are the same width, true for every ABI Sync
// targets (Win64, macOS, Linux, all LP64/LLP64) but not guaranteed by the
// C++ standard in general — // VERIFY if Sync ever targets an ABI where
// that assumption does not hold.
//
// Reviewer audit table (index : SDK field name : role):
//   1 initialize                          real  (ABI probe)
//   2 destroy                             real  (shutdown)
//   3 version                             real  (typed to match the task spec; currently unused)
//   4 is_supported_CPU                    real  (ABI probe)
//   5 find_create                         reserved
//   6 find_create_v2                      reserved
//   7 find_destroy                        reserved
//   8 find_get_sources                    reserved
//   9 send_create                         real  (open_sender)
//  10 send_destroy                        real  (close_sender / dtor)
//  11 send_send_video                     reserved
//  12 send_send_video_async               reserved
//  13 send_send_audio                     reserved
//  14 send_send_metadata                  reserved
//  15 send_capture                        reserved
//  16 send_free_metadata                  reserved
//  17 send_get_tally                      reserved
//  18 send_get_no_connections             reserved
//  19 send_clear_connection_metadata      reserved
//  20 send_add_connection_metadata        reserved
//  21 send_set_failover                   reserved
//  22 recv_create_v2                      reserved
//  23 recv_create                         reserved
//  24 recv_destroy                        reserved
//  25 recv_capture                        reserved
//  26 recv_free_video                     reserved
//  27 recv_free_audio                     reserved
//  28 recv_free_metadata                  reserved
//  29 recv_send_metadata                  reserved
//  30 recv_set_tally                      reserved
//  31 recv_get_performance                reserved
//  32 recv_get_queue                      reserved
//  33 recv_clear_connection_metadata      reserved
//  34 recv_add_connection_metadata        reserved
//  35 recv_get_no_connections             reserved
//  36 routing_create                      reserved
//  37 routing_destroy                     reserved
//  38 routing_change                      reserved
//  39 routing_clear                       reserved
//  40 util_send_send_audio_interleaved_16s reserved
//  41 util_audio_to_interleaved_16s       reserved
//  42 util_audio_from_interleaved_16s     reserved
//  43 find_wait_for_sources               reserved
//  44 find_get_current_sources            reserved
//  45 util_audio_to_interleaved_32f       reserved
//  46 util_audio_from_interleaved_32f     reserved
//  47 util_send_send_audio_interleaved_32f reserved
//  48 recv_free_video_v2                  reserved
//  49 recv_free_audio_v2                  reserved
//  50 recv_capture_v2                     reserved
//  51 send_send_video_v2                  real  (synchronous flush before destroy)
//  52 send_send_video_async_v2            real  (per-frame publish)

struct ndi_send_instance_opaque;
using ndi_send_instance_t = ndi_send_instance_opaque*;

// Mirrors NDIlib_send_create_t (Processing.NDI.Send.h) field for field.
struct ndi_send_create {
  const char* p_ndi_name;
  const char* p_groups;
  bool clock_video;
  bool clock_audio;
};

// Mirrors NDIlib_video_frame_v2_t (Processing.NDI.structs.h) field for
// field. The SDK's `line_stride_in_bytes` / `data_size_in_bytes` union
// collapses to a single int here since we only ever use the uncompressed
// (line-stride) meaning for RGBA.
struct ndi_video_frame_v2 {
  int xres;
  int yres;
  std::int32_t four_cc;
  int frame_rate_n;
  int frame_rate_d;
  float picture_aspect_ratio;
  std::int32_t frame_format_type;
  std::int64_t timecode;
  std::uint8_t* p_data;
  int line_stride_in_bytes;
  const char* p_metadata;
  std::int64_t timestamp;
};

// NDI_LIB_FOURCC('R','G','B','A'): least-significant byte first, so 'R'
// occupies bits 0-7. // VERIFY against NDIlib_FourCC_video_type_e in
// Processing.NDI.structs.h if the vendor ever changes this packing.
constexpr std::int32_t kNdiFourCcRgba =
    static_cast<std::int32_t>(static_cast<std::uint32_t>('R') | (static_cast<std::uint32_t>('G') << 8) |
                               (static_cast<std::uint32_t>('B') << 16) |
                               (static_cast<std::uint32_t>('A') << 24));

// NDIlib_frame_format_type_e::NDIlib_frame_format_type_progressive.
constexpr std::int32_t kNdiFrameFormatProgressive = 1;

// The SDK's own NDIlib_video_frame_v2_t C++ default constructor defaults to
// 30000/1001 (~29.97 fps); protocol::FrameView carries no frame-rate field,
// so we reuse the vendor's own default rather than inventing one.
constexpr int kDefaultFrameRateNumerator = 30000;
constexpr int kDefaultFrameRateDenominator = 1001;

struct ndi_v5 {
  bool (*initialize)(void);
  void (*destroy)(void);
  const char* (*version)(void);
  bool (*is_supported_CPU)(void);
  void* reserved_find_create;
  void* reserved_find_create_v2;
  void* reserved_find_destroy;
  void* reserved_find_get_sources;
  ndi_send_instance_t (*send_create)(const ndi_send_create* p_create_settings);
  void (*send_destroy)(ndi_send_instance_t p_instance);
  void* reserved_send_send_video;
  void* reserved_send_send_video_async;
  void* reserved_send_send_audio;
  void* reserved_send_send_metadata;
  void* reserved_send_capture;
  void* reserved_send_free_metadata;
  void* reserved_send_get_tally;
  void* reserved_send_get_no_connections;
  void* reserved_send_clear_connection_metadata;
  void* reserved_send_add_connection_metadata;
  void* reserved_send_set_failover;
  void* reserved_recv_create_v2;
  void* reserved_recv_create;
  void* reserved_recv_destroy;
  void* reserved_recv_capture;
  void* reserved_recv_free_video;
  void* reserved_recv_free_audio;
  void* reserved_recv_free_metadata;
  void* reserved_recv_send_metadata;
  void* reserved_recv_set_tally;
  void* reserved_recv_get_performance;
  void* reserved_recv_get_queue;
  void* reserved_recv_clear_connection_metadata;
  void* reserved_recv_add_connection_metadata;
  void* reserved_recv_get_no_connections;
  void* reserved_routing_create;
  void* reserved_routing_destroy;
  void* reserved_routing_change;
  void* reserved_routing_clear;
  void* reserved_util_send_send_audio_interleaved_16s;
  void* reserved_util_audio_to_interleaved_16s;
  void* reserved_util_audio_from_interleaved_16s;
  void* reserved_find_wait_for_sources;
  void* reserved_find_get_current_sources;
  void* reserved_util_audio_to_interleaved_32f;
  void* reserved_util_audio_from_interleaved_32f;
  void* reserved_util_send_send_audio_interleaved_32f;
  void* reserved_recv_free_video_v2;
  void* reserved_recv_free_audio_v2;
  void* reserved_recv_capture_v2;
  void (*send_send_video_v2)(ndi_send_instance_t p_instance, const ndi_video_frame_v2* p_video_data);
  void (*send_send_video_async_v2)(ndi_send_instance_t p_instance, const ndi_video_frame_v2* p_video_data);
};

using ndi_v5_load_fn = const ndi_v5* (*)(void);

// ---------------------------------------------------------------------------
// Bounded, deduplicated discovery path list
// ---------------------------------------------------------------------------
constexpr std::size_t kMaximumDiscoveryPaths = 4;

[[nodiscard]] auto join_runtime_dir(std::string_view dir) -> std::string {
  if (dir.empty()) {
    return std::string(platform_library_filename());
  }
  std::string path(dir);
  if (path.back() != '/' && path.back() != '\\') {
    path.push_back(kPathSeparator);
  }
  path += platform_library_filename();
  return path;
}

void add_candidate(std::array<std::string, kMaximumDiscoveryPaths>& paths, std::size_t& count,
                   std::string candidate) noexcept {
  if (candidate.empty() || count >= paths.size()) {
    return;
  }
  for (std::size_t index = 0; index < count; ++index) {
    if (paths[index] == candidate) {
      return;
    }
  }
  try {
    paths[count] = std::move(candidate);
  } catch (const std::exception&) {
    return;
  }
  ++count;
}

// ---------------------------------------------------------------------------
// Checked arithmetic (mirrors metal_frame_publisher.mm's helpers)
// ---------------------------------------------------------------------------
[[nodiscard]] auto checked_add(std::size_t left, std::size_t right, std::size_t& result) noexcept -> bool {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    return false;
  }
  result = left + right;
  return true;
}

[[nodiscard]] auto checked_multiply(std::size_t left, std::size_t right, std::size_t& result) noexcept
    -> bool {
  if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
    return false;
  }
  result = left * right;
  return true;
}

[[nodiscard]] auto checked_timecode(std::uint64_t presentation_time_us, std::int64_t& out) noexcept
    -> bool {
  constexpr std::uint64_t kHundredNsPerMicrosecond = 10;
  if (presentation_time_us > std::numeric_limits<std::uint64_t>::max() / kHundredNsPerMicrosecond) {
    return false;
  }
  const std::uint64_t scaled = presentation_time_us * kHundredNsPerMicrosecond;
  if (scaled > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  out = static_cast<std::int64_t>(scaled);
  return true;
}

// ---------------------------------------------------------------------------
// Frame and identifier validation (mirrors metal_frame_publisher.mm's
// view_is_valid shape)
// ---------------------------------------------------------------------------
constexpr std::uint32_t kMaximumDimension = 4096;
constexpr std::uint32_t kMaximumPayloadBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumSenderIdBytes = 128;
constexpr std::size_t kMaximumSenderNameBytes = 64;

[[nodiscard]] auto frame_view_is_valid(const protocol::FrameView& frame) noexcept -> bool {
  if (frame.version != 1 || frame.header_bytes != 64 || frame.flags != 1 || !frame.top_down ||
      frame.pixel_format != 1 || (frame.color_space != 1 && frame.color_space != 2) ||
      (frame.alpha_mode != 1 && frame.alpha_mode != 2 && frame.alpha_mode != 3) || frame.width == 0 ||
      frame.height == 0 || frame.width > kMaximumDimension || frame.height > kMaximumDimension) {
    return false;
  }

  std::size_t packed_row_bytes = 0;
  if (!checked_multiply(static_cast<std::size_t>(frame.width), 4U, packed_row_bytes) ||
      static_cast<std::size_t>(frame.row_stride) < packed_row_bytes) {
    return false;
  }
  std::size_t expected_payload_bytes = 0;
  if (!checked_multiply(static_cast<std::size_t>(frame.row_stride), static_cast<std::size_t>(frame.height),
                        expected_payload_bytes) ||
      expected_payload_bytes > kMaximumPayloadBytes ||
      expected_payload_bytes != static_cast<std::size_t>(frame.payload_bytes) ||
      expected_payload_bytes != frame.payload.size()) {
    return false;
  }
  return true;
}

[[nodiscard]] auto sender_id_is_valid(std::string_view sender_id) noexcept -> bool {
  return !sender_id.empty() && sender_id.size() <= kMaximumSenderIdBytes;
}

[[nodiscard]] auto sender_name_is_valid(std::string_view name) noexcept -> bool {
  if (name.empty() || name.size() > kMaximumSenderNameBytes) {
    return false;
  }
  for (const char character : name) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte == 0x7f) {
      return false;
    }
  }
  return true;
}

}  // namespace

struct NdiFramePublisher::Impl {
  struct FrameBuffer {
    std::vector<std::byte> storage;
    std::size_t accounted_bytes = 0;
  };

  struct SenderEntry {
    bool occupied = false;
    std::size_t sender_id_length = 0;
    std::array<char, kMaximumSenderIdBytes> sender_id{};
    ndi_send_instance_t handle = nullptr;
    std::array<FrameBuffer, 2> buffers{};
    std::size_t next_buffer_index = 0;

    [[nodiscard]] auto id_view() const noexcept -> std::string_view {
      return {sender_id.data(), sender_id_length};
    }
  };

  explicit Impl(Options options) : allocation_budget_bytes(options.allocation_budget_bytes) {
    if (options.allocation_budget_bytes > kProductAllocationBudgetBytes) {
      configuration_valid = false;
      return;
    }
    discover(options.runtime_path);
  }

  bool configuration_valid = true;
  native_module_handle module = kNullModule;
  const ndi_v5* api = nullptr;
  bool initialized = false;
  std::size_t allocation_budget_bytes = kProductAllocationBudgetBytes;
  std::size_t allocated_bytes = 0;
  std::optional<ProviderFailure> latched_failure;
  std::array<SenderEntry, NdiFramePublisher::kMaximumSenderEntries> senders{};

  void discover(std::string_view runtime_path) noexcept {
    try {
      std::array<std::string, kMaximumDiscoveryPaths> candidates{};
      std::size_t candidate_count = 0;

      add_candidate(candidates, candidate_count, join_runtime_dir(runtime_path));

      std::string env_value;
      if (read_env_var("NDI_RUNTIME_DIR_V6", env_value)) {
        add_candidate(candidates, candidate_count, join_runtime_dir(env_value));
      }
      if (read_env_var("NDI_RUNTIME_DIR_V5", env_value)) {
        add_candidate(candidates, candidate_count, join_runtime_dir(env_value));
      }
      // Bare library name: defers entirely to the OS loader's own default
      // search rules, adding no path of our own.
      add_candidate(candidates, candidate_count, std::string(platform_library_filename()));

      for (std::size_t index = 0; index < candidate_count; ++index) {
        const std::string& candidate = candidates[index];
        native_module_handle handle = load_module(candidate, path_has_directory(candidate));
        if (handle == kNullModule) {
          continue;
        }
        auto* loader = reinterpret_cast<ndi_v5_load_fn>(resolve_symbol(handle, "NDIlib_v5_load"));
        const ndi_v5* candidate_api = loader != nullptr ? loader() : nullptr;
        if (candidate_api == nullptr || candidate_api->initialize == nullptr ||
            candidate_api->is_supported_CPU == nullptr || candidate_api->send_create == nullptr ||
            candidate_api->send_destroy == nullptr || candidate_api->send_send_video_v2 == nullptr ||
            candidate_api->send_send_video_async_v2 == nullptr) {
          unload_module(handle);
          continue;
        }
        // ABI probe: an unsupported CPU or a runtime that refuses to
        // initialize is never an error, it just means this provider is not
        // offered. Order matches the vendor's documented recommendation:
        // check CPU support before calling initialize().
        if (!candidate_api->is_supported_CPU() || !candidate_api->initialize()) {
          unload_module(handle);
          continue;
        }
        module = handle;
        api = candidate_api;
        initialized = true;
        return;
      }
    } catch (const std::exception&) {
      module = kNullModule;
      api = nullptr;
      initialized = false;
    }
  }

  void latch_failure(ProviderFailureKind kind, std::uint32_t status, std::int64_t code) noexcept {
    if (!latched_failure.has_value()) {
      latched_failure = ProviderFailure{.kind = kind, .native_status = status, .native_error_code = code};
    }
  }

  [[nodiscard]] auto find_sender(std::string_view sender_id) noexcept -> SenderEntry* {
    for (SenderEntry& entry : senders) {
      if (entry.occupied && entry.sender_id_length == sender_id.size() && entry.id_view() == sender_id) {
        return &entry;
      }
    }
    return nullptr;
  }

  // Unlike MetalFramePublisher's ring, a buffer here only ever grows: once
  // large enough for some frame size it stays that size rather than being
  // replaced on every resolution change. CPU buffers, unlike GPU textures,
  // do not need to match the current resolution exactly, so this trades a
  // little peak memory for a much simpler, still budget-safe policy.
  [[nodiscard]] auto ensure_buffer_capacity(FrameBuffer& buffer, std::size_t required_bytes) noexcept
      -> bool {
    if (buffer.storage.size() >= required_bytes) {
      return true;
    }
    const std::size_t growth = required_bytes - buffer.storage.size();
    std::size_t projected_bytes = 0;
    if (!checked_add(allocated_bytes, growth, projected_bytes) ||
        projected_bytes > allocation_budget_bytes) {
      return false;
    }
    try {
      buffer.storage.resize(required_bytes);
    } catch (const std::exception&) {
      return false;
    }
    allocated_bytes = projected_bytes;
    buffer.accounted_bytes += growth;
    return true;
  }

  void destroy_sender(SenderEntry& entry) noexcept {
    if (!entry.occupied) {
      return;
    }
    if (api != nullptr && entry.handle != nullptr) {
      // The async send ABI gives us no completion signal of its own; the
      // documented way to force one is a synchronous send call, which
      // blocks until any frame handed to the previous async call has been
      // fully consumed. Passing nullptr here is a flush with no new frame
      // to submit, done before we release the buffers that call may still
      // have been reading and before destroying the sender itself.
      if (api->send_send_video_v2 != nullptr) {
        api->send_send_video_v2(entry.handle, nullptr);
      }
      if (api->send_destroy != nullptr) {
        api->send_destroy(entry.handle);
      }
    }
    for (FrameBuffer& buffer : entry.buffers) {
      allocated_bytes -= buffer.accounted_bytes;
    }
    entry = SenderEntry{};
  }
};

NdiFramePublisher::NdiFramePublisher() : NdiFramePublisher(Options{}) {}

NdiFramePublisher::NdiFramePublisher(Options options) {
  try {
    impl_ = std::make_unique<Impl>(options);
  } catch (const std::exception&) {
    impl_.reset();
  }
}

NdiFramePublisher::~NdiFramePublisher() {
  if (impl_ == nullptr) {
    return;
  }
  for (std::size_t index = impl_->senders.size(); index > 0; --index) {
    impl_->destroy_sender(impl_->senders[index - 1U]);
  }
  if (impl_->initialized && impl_->api != nullptr && impl_->api->destroy != nullptr) {
    impl_->api->destroy();
  }
  unload_module(impl_->module);
  impl_->module = kNullModule;
  impl_->api = nullptr;
  impl_->initialized = false;
}

auto NdiFramePublisher::available() const noexcept -> bool {
  return impl_ != nullptr && impl_->configuration_valid && impl_->module != kNullModule &&
         impl_->api != nullptr && impl_->initialized;
}

auto NdiFramePublisher::open_sender(std::string_view sender_id, std::string_view name) noexcept -> bool {
  // Input-shape validation is a pure function of the arguments and does not
  // touch impl_, so it runs (and is exercised by tests) whether or not a
  // runtime was ever discovered; the availability check below is the first
  // one that dereferences impl_, and available() itself guards on impl_ !=
  // nullptr.
  if (!sender_id_is_valid(sender_id) || !sender_name_is_valid(name)) {
    return false;
  }
  if (!available()) {
    return false;
  }
  if (impl_->find_sender(sender_id) != nullptr) {
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

  std::array<char, kMaximumSenderNameBytes + 1> name_buffer{};
  std::memcpy(name_buffer.data(), name.data(), name.size());
  name_buffer[name.size()] = '\0';

  ndi_send_create create_settings{};
  create_settings.p_ndi_name = name_buffer.data();
  create_settings.p_groups = nullptr;
  // Sync pushes frames as they arrive off the wire rather than at a fixed
  // rate; NDI's clocking would otherwise block send calls to pace them to a
  // frame rate we do not actually have.
  create_settings.clock_video = false;
  create_settings.clock_audio = false;

  ndi_send_instance_t handle = impl_->api->send_create(&create_settings);
  if (handle == nullptr) {
    impl_->latch_failure(ProviderFailureKind::NdiInitializationFailed, 0, 0);
    return false;
  }

  entry->occupied = true;
  entry->sender_id_length = sender_id.size();
  std::memcpy(entry->sender_id.data(), sender_id.data(), sender_id.size());
  entry->handle = handle;
  entry->next_buffer_index = 0;
  return true;
}

void NdiFramePublisher::close_sender(std::string_view sender_id) noexcept {
  if (impl_ == nullptr) {
    return;
  }
  Impl::SenderEntry* entry = impl_->find_sender(sender_id);
  if (entry != nullptr) {
    impl_->destroy_sender(*entry);
  }
}

auto NdiFramePublisher::publish(std::string_view sender_id, const protocol::FrameView& frame) noexcept
    -> PublishResult {
  // Frame-shape validation is a pure function of frame and does not touch
  // impl_, so it runs (and is exercised by tests) whether or not a runtime
  // was ever discovered; available() is the first check below that
  // dereferences impl_, and it itself guards on impl_ != nullptr.
  if (!frame_view_is_valid(frame)) {
    return PublishResult::Failed;
  }
  if (!available() || impl_->latched_failure.has_value()) {
    return PublishResult::Failed;
  }
  Impl::SenderEntry* entry = impl_->find_sender(sender_id);
  if (entry == nullptr || entry->handle == nullptr) {
    return PublishResult::Failed;
  }

  const std::size_t packed_row_bytes = static_cast<std::size_t>(frame.width) * 4U;
  std::size_t required_bytes = 0;
  if (!checked_multiply(packed_row_bytes, static_cast<std::size_t>(frame.height), required_bytes)) {
    return PublishResult::Failed;
  }

  Impl::FrameBuffer& buffer = entry->buffers[entry->next_buffer_index];
  if (!impl_->ensure_buffer_capacity(buffer, required_bytes)) {
    impl_->latch_failure(ProviderFailureKind::NdiSendFailed, 0, 0);
    return PublishResult::Failed;
  }

  std::byte* destination = buffer.storage.data();
  if (static_cast<std::size_t>(frame.row_stride) == packed_row_bytes) {
    std::memcpy(destination, frame.payload.data(), frame.payload.size());
  } else {
    for (std::uint32_t row = 0; row < frame.height; ++row) {
      std::memcpy(destination + static_cast<std::size_t>(row) * packed_row_bytes,
                  frame.payload.data() + static_cast<std::size_t>(row) * frame.row_stride, packed_row_bytes);
    }
  }

  std::int64_t timecode = 0;
  if (!checked_timecode(frame.presentation_time_us, timecode)) {
    return PublishResult::Failed;
  }

  ndi_video_frame_v2 descriptor{};
  descriptor.xres = static_cast<int>(frame.width);
  descriptor.yres = static_cast<int>(frame.height);
  descriptor.four_cc = kNdiFourCcRgba;
  descriptor.frame_rate_n = kDefaultFrameRateNumerator;
  descriptor.frame_rate_d = kDefaultFrameRateDenominator;
  descriptor.picture_aspect_ratio = 0.0f;  // 0 means square pixels, per the vendor header.
  descriptor.frame_format_type = kNdiFrameFormatProgressive;
  descriptor.timecode = timecode;
  descriptor.p_data = reinterpret_cast<std::uint8_t*>(destination);
  descriptor.line_stride_in_bytes = static_cast<int>(packed_row_bytes);
  descriptor.p_metadata = nullptr;
  descriptor.timestamp = 0;  // Receive-only field; irrelevant when sending.

  impl_->api->send_send_video_async_v2(entry->handle, &descriptor);
  entry->next_buffer_index = (entry->next_buffer_index + 1U) % entry->buffers.size();
  return PublishResult::Accepted;
}

auto NdiFramePublisher::poll_failure(std::uint64_t now_ms) noexcept -> std::optional<ProviderFailure> {
  (void)now_ms;
  if (impl_ == nullptr) {
    return std::nullopt;
  }
  return impl_->latched_failure;
}

}  // namespace noisefactor::sync
