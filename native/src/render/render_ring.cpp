#include <sync/render/render_ring.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace noisefactor::sync::render {

namespace {

constexpr int kAcquireAttempts = 8;
constexpr std::uint64_t kBytesPerPixel = 4;

[[nodiscard]] constexpr auto pack_newest(std::uint64_t frame, std::uint32_t slot) noexcept
    -> std::uint64_t {
  return (frame << 2U) | slot;
}
[[nodiscard]] constexpr auto newest_frame_of(std::uint64_t packed) noexcept -> std::uint64_t {
  return packed >> 2U;
}
[[nodiscard]] constexpr auto newest_slot_of(std::uint64_t packed) noexcept -> std::uint32_t {
  return static_cast<std::uint32_t>(packed & 3U);
}

[[nodiscard]] auto geometry_ok(const RenderRingGeometry& geometry) noexcept -> bool {
  if (geometry.width == 0 || geometry.height == 0) return false;
  if (geometry.width > kRenderRingMaxDimension || geometry.height > kRenderRingMaxDimension) {
    return false;
  }
  if (geometry.color_space != kRenderColorSpaceSrgb &&
      geometry.color_space != kRenderColorSpaceDisplayP3) {
    return false;
  }
  return geometry.alpha_mode == kRenderAlphaOpaque ||
         geometry.alpha_mode == kRenderAlphaStraight ||
         geometry.alpha_mode == kRenderAlphaPremultiplied;
}

// Everything the reader trusts about a mapping is checked here, against the
// mapping's own length, so a truncated or hostile section cannot steer a copy
// outside it.
[[nodiscard]] auto header_ok(const RenderRingHeader& header, std::size_t mapping_bytes) noexcept
    -> bool {
  if (header.magic != kRenderRingMagic || header.version != kRenderRingVersion) return false;
  if (header.header_bytes != render_ring_payload_offset()) return false;
  if (header.slot_count != kRenderRingSlots) return false;
  if (header.pixel_format != kRenderPixelFormatRgba8) return false;
  const RenderRingGeometry geometry{.width = header.width,
                                    .height = header.height,
                                    .color_space = header.color_space,
                                    .alpha_mode = header.alpha_mode};
  if (!geometry_ok(geometry)) return false;
  if (header.row_stride != static_cast<std::uint64_t>(header.width) * kBytesPerPixel) return false;
  if (header.slot_bytes != static_cast<std::uint64_t>(header.row_stride) * header.height) {
    return false;
  }
  const auto expected = render_ring_bytes(geometry);
  return expected.has_value() && *expected <= mapping_bytes;
}

}  // namespace

auto render_ring_bytes(const RenderRingGeometry& geometry) noexcept
    -> std::optional<std::size_t> {
  if (!geometry_ok(geometry)) return std::nullopt;
  const std::uint64_t slot = static_cast<std::uint64_t>(geometry.width) * kBytesPerPixel *
                             geometry.height;
  return render_ring_payload_offset() + static_cast<std::size_t>(slot) * kRenderRingSlots;
}

auto render_clock_us() noexcept -> std::uint64_t {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// ------------------------------------------------------------------ writer

RenderRingWriter::RenderRingWriter(std::span<std::byte> mapping,
                                   const RenderRingGeometry& geometry,
                                   std::uint32_t writer_pid) noexcept {
  const auto bytes = render_ring_bytes(geometry);
  if (!bytes.has_value() || mapping.data() == nullptr || mapping.size() < *bytes) return;
  auto* header = reinterpret_cast<RenderRingHeader*>(mapping.data());
  header->version = kRenderRingVersion;
  header->header_bytes = static_cast<std::uint32_t>(render_ring_payload_offset());
  header->slot_count = kRenderRingSlots;
  header->width = geometry.width;
  header->height = geometry.height;
  header->row_stride = geometry.width * static_cast<std::uint32_t>(kBytesPerPixel);
  header->slot_bytes = header->row_stride * geometry.height;
  header->pixel_format = kRenderPixelFormatRgba8;
  header->color_space = geometry.color_space;
  header->alpha_mode = geometry.alpha_mode;
  header->reserved0 = 0;
  header->writer_pid = writer_pid;
  header->newest.store(0, std::memory_order_relaxed);
  header->reader_lease.store(0, std::memory_order_relaxed);
  header->writer_state.store(kRenderWriterRunning, std::memory_order_relaxed);
  header->writer_heartbeat_us.store(render_clock_us(), std::memory_order_relaxed);
  header->reader_heartbeat_us.store(0, std::memory_order_relaxed);
  for (auto& reserved : header->reserved1) reserved = 0;
  for (auto& slot : header->slot) {
    slot.sequence.store(0, std::memory_order_relaxed);
    slot.presentation_time_us = 0;
    slot.reserved[0] = slot.reserved[1] = 0;
  }
  // The magic goes last behind a release fence: a reader that sees it must
  // also see every field it is about to validate.
  std::atomic_thread_fence(std::memory_order_release);
  header->magic = kRenderRingMagic;
  header_ = header;
  payload_ = mapping.data() + render_ring_payload_offset();
}

auto RenderRingWriter::valid() const noexcept -> bool { return header_ != nullptr; }

auto RenderRingWriter::row_stride() const noexcept -> std::size_t {
  return header_ == nullptr ? 0 : header_->row_stride;
}

auto RenderRingWriter::write_with(Fill fill, void* context,
                                  std::uint64_t presentation_time_us) noexcept -> bool {
  if (header_ == nullptr || fill == nullptr) return false;
  // Single writer: nobody else moves newest, so a relaxed read of our own
  // last store is exact.
  const std::uint64_t packed = header_->newest.load(std::memory_order_relaxed);
  const std::uint64_t frame = newest_frame_of(packed) + 1;
  const bool has_newest = packed != 0;
  const std::uint32_t newest_slot = newest_slot_of(packed);

  // Pick a slot that is neither the newest (the reader's next lease) nor
  // leased. Marking the slot odd BEFORE reading the lease, and the reader
  // storing its lease BEFORE reading the sequence, both sequentially
  // consistent, is the handshake: at least one side sees the other. Either the
  // writer sees the lease and backs out, or the reader sees the odd sequence
  // and retries. Three slots always leave one free: newest, leased, and this.
  std::uint32_t chosen = kRenderRingSlots;
  for (std::uint32_t step = 1; step <= kRenderRingSlots; ++step) {
    const std::uint32_t index = (newest_slot + step) % kRenderRingSlots;
    if (has_newest && index == newest_slot) continue;
    RenderRingSlot& slot = header_->slot[index];
    const std::uint64_t previous = slot.sequence.load(std::memory_order_relaxed);
    slot.sequence.store(frame * 2 - 1, std::memory_order_seq_cst);
    if (header_->reader_lease.load(std::memory_order_seq_cst) == index + 1) {
      slot.sequence.store(previous, std::memory_order_release);
      continue;
    }
    chosen = index;
    break;
  }
  if (chosen == kRenderRingSlots) return false;

  RenderRingSlot& slot = header_->slot[chosen];
  std::span<std::byte> destination(
      payload_ + static_cast<std::size_t>(chosen) * header_->slot_bytes, header_->slot_bytes);
  if (!fill(context, destination, header_->row_stride)) {
    // The payload may be half written, so the slot must not read as holding
    // any complete frame. Zero is "never written", which no reader accepts.
    slot.sequence.store(0, std::memory_order_release);
    return false;
  }
  slot.presentation_time_us = presentation_time_us;
  slot.sequence.store(frame * 2, std::memory_order_release);
  header_->newest.store(pack_newest(frame, chosen), std::memory_order_release);
  header_->writer_heartbeat_us.store(render_clock_us(), std::memory_order_release);
  return true;
}

auto RenderRingWriter::write(std::span<const std::byte> source, std::size_t source_stride,
                             std::uint64_t presentation_time_us) noexcept -> bool {
  if (header_ == nullptr) return false;
  const std::size_t row = header_->row_stride;
  if (source_stride < row) return false;
  if (source.size() < source_stride * (header_->height - 1) + row) return false;

  struct Copy {
    std::span<const std::byte> source;
    std::size_t source_stride;
    std::uint32_t height;
  } copy{source, source_stride, header_->height};

  const auto fill = [](void* context, std::span<std::byte> destination,
                       std::size_t stride) noexcept -> bool {
    const auto& c = *static_cast<const Copy*>(context);
    if (c.source_stride == stride) {
      std::memcpy(destination.data(), c.source.data(), stride * c.height);
      return true;
    }
    for (std::uint32_t y = 0; y < c.height; ++y) {
      std::memcpy(destination.data() + y * stride, c.source.data() + y * c.source_stride,
                  stride);
    }
    return true;
  };
  return write_with(fill, &copy, presentation_time_us);
}

void RenderRingWriter::heartbeat(std::uint64_t now_us) noexcept {
  if (header_ == nullptr) return;
  header_->writer_heartbeat_us.store(now_us, std::memory_order_release);
}

void RenderRingWriter::close() noexcept {
  if (header_ == nullptr) return;
  header_->writer_state.store(kRenderWriterClosed, std::memory_order_release);
}

auto RenderRingWriter::reader_alive(std::uint64_t now_us,
                                    std::uint64_t timeout_us) const noexcept -> bool {
  if (header_ == nullptr) return false;
  const std::uint64_t last = header_->reader_heartbeat_us.load(std::memory_order_acquire);
  if (last == 0) return false;
  return now_us < last ? (last - now_us) <= timeout_us : (now_us - last) <= timeout_us;
}

// ------------------------------------------------------------------ lease

RenderFrameLease::RenderFrameLease(RenderFrameLease&& other) noexcept
    : header_(std::exchange(other.header_, nullptr)),
      info_(other.info_),
      payload_(std::exchange(other.payload_, {})) {}

auto RenderFrameLease::operator=(RenderFrameLease&& other) noexcept -> RenderFrameLease& {
  if (this != &other) {
    release();
    header_ = std::exchange(other.header_, nullptr);
    info_ = other.info_;
    payload_ = std::exchange(other.payload_, {});
  }
  return *this;
}

RenderFrameLease::~RenderFrameLease() { release(); }

void RenderFrameLease::release() noexcept {
  if (header_ == nullptr) return;
  header_->reader_lease.store(0, std::memory_order_release);
  header_ = nullptr;
  payload_ = {};
}

// ------------------------------------------------------------------ reader

RenderRingReader::RenderRingReader(std::span<std::byte> mapping) noexcept {
  if (mapping.data() == nullptr || mapping.size() < sizeof(RenderRingHeader)) return;
  auto* header = reinterpret_cast<RenderRingHeader*>(mapping.data());
  if (header->magic != kRenderRingMagic) return;
  std::atomic_thread_fence(std::memory_order_acquire);
  if (!header_ok(*header, mapping.size())) return;
  header_ = header;
  payload_ = mapping.data() + render_ring_payload_offset();
}

auto RenderRingReader::valid() const noexcept -> bool { return header_ != nullptr; }

auto RenderRingReader::newest_frame() const noexcept -> std::uint64_t {
  return header_ == nullptr ? 0
                            : newest_frame_of(header_->newest.load(std::memory_order_acquire));
}

auto RenderRingReader::writer_closed() const noexcept -> bool {
  return header_ == nullptr ||
         header_->writer_state.load(std::memory_order_acquire) == kRenderWriterClosed;
}

auto RenderRingReader::writer_alive(std::uint64_t now_us,
                                    std::uint64_t timeout_us) const noexcept -> bool {
  if (header_ == nullptr || writer_closed()) return false;
  const std::uint64_t last = header_->writer_heartbeat_us.load(std::memory_order_acquire);
  return now_us < last ? (last - now_us) <= timeout_us : (now_us - last) <= timeout_us;
}

void RenderRingReader::heartbeat(std::uint64_t now_us) noexcept {
  if (header_ == nullptr) return;
  header_->reader_heartbeat_us.store(now_us, std::memory_order_release);
}

auto RenderRingReader::geometry() const noexcept -> RenderRingGeometry {
  if (header_ == nullptr) return {};
  return {.width = header_->width,
          .height = header_->height,
          .color_space = header_->color_space,
          .alpha_mode = header_->alpha_mode};
}

auto RenderRingReader::acquire() noexcept -> RenderFrameLease {
  RenderFrameLease lease;
  if (header_ == nullptr) return lease;
  for (int attempt = 0; attempt < kAcquireAttempts; ++attempt) {
    const std::uint64_t packed = header_->newest.load(std::memory_order_acquire);
    if (packed == 0) return lease;
    const std::uint64_t frame = newest_frame_of(packed);
    const std::uint32_t index = newest_slot_of(packed);
    if (index >= kRenderRingSlots) return lease;
    header_->reader_lease.store(index + 1, std::memory_order_seq_cst);
    const RenderRingSlot& slot = header_->slot[index];
    if (slot.sequence.load(std::memory_order_seq_cst) != frame * 2) {
      // Rewritten since newest was read (the writer lapped us before the
      // lease landed). Drop the lease and look at the new newest.
      header_->reader_lease.store(0, std::memory_order_release);
      continue;
    }
    lease.header_ = header_;
    lease.info_ = {.frame = frame,
                   .presentation_time_us = slot.presentation_time_us,
                   .width = header_->width,
                   .height = header_->height,
                   .row_stride = header_->row_stride,
                   .color_space = header_->color_space,
                   .alpha_mode = header_->alpha_mode};
    lease.payload_ = std::span<const std::byte>(
        payload_ + static_cast<std::size_t>(index) * header_->slot_bytes, header_->slot_bytes);
    return lease;
  }
  return lease;
}

// ------------------------------------------------------------------ section

RenderRingSection::RenderRingSection(RenderRingSection&& other) noexcept
    : view_(std::exchange(other.view_, nullptr)),
      size_(std::exchange(other.size_, 0)),
      name_(std::move(other.name_)),
      owner_(std::exchange(other.owner_, false))
#if defined(_WIN32)
      ,
      handle_(std::exchange(other.handle_, nullptr))
#else
      ,
      device_(std::exchange(other.device_, 0)),
      inode_(std::exchange(other.inode_, 0))
#endif
{
}

auto RenderRingSection::operator=(RenderRingSection&& other) noexcept -> RenderRingSection& {
  if (this != &other) {
    reset();
    view_ = std::exchange(other.view_, nullptr);
    size_ = std::exchange(other.size_, 0);
    name_ = std::move(other.name_);
    owner_ = std::exchange(other.owner_, false);
#if defined(_WIN32)
    handle_ = std::exchange(other.handle_, nullptr);
#else
    device_ = std::exchange(other.device_, 0);
    inode_ = std::exchange(other.inode_, 0);
#endif
  }
  return *this;
}

RenderRingSection::~RenderRingSection() { reset(); }

auto RenderRingSection::bytes() noexcept -> std::span<std::byte> {
  return {static_cast<std::byte*>(view_), size_};
}

#if defined(_WIN32)

namespace {

[[nodiscard]] auto widen(const std::string& text) -> std::wstring {
  if (text.empty()) return {};
  const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

}  // namespace

// Windows sections are named objects in the session's Local namespace with the
// creator's default DACL, which grants the creating user and SYSTEM only. The
// section disappears with its last handle, so there is nothing to unlink.
auto RenderRingSection::create(const std::string& name, std::size_t bytes, std::string& error)
    -> std::optional<RenderRingSection> {
  const std::wstring wide = widen("Local\\" + name);
  if (wide.empty()) {
    error = "invalid section name";
    return std::nullopt;
  }
  const auto size64 = static_cast<std::uint64_t>(bytes);
  HANDLE handle = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                       static_cast<DWORD>(size64 >> 32U),
                                       static_cast<DWORD>(size64 & 0xFFFFFFFFU), wide.c_str());
  if (handle == nullptr) {
    error = "CreateFileMappingW failed: " + std::to_string(::GetLastError());
    return std::nullopt;
  }
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    // Another live writer owns this name. Sharing it would interleave two
    // writers in one ring, which the seqlock does not survive.
    ::CloseHandle(handle);
    error = "section already exists";
    return std::nullopt;
  }
  void* view = ::MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
  if (view == nullptr) {
    error = "MapViewOfFile failed: " + std::to_string(::GetLastError());
    ::CloseHandle(handle);
    return std::nullopt;
  }
  RenderRingSection section;
  section.view_ = view;
  section.size_ = bytes;
  section.name_ = name;
  section.owner_ = true;
  section.handle_ = handle;
  return section;
}

auto RenderRingSection::open(const std::string& name, std::string& error)
    -> std::optional<RenderRingSection> {
  const std::wstring wide = widen("Local\\" + name);
  if (wide.empty()) {
    error = "invalid section name";
    return std::nullopt;
  }
  HANDLE handle = ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide.c_str());
  if (handle == nullptr) {
    error = "OpenFileMappingW failed: " + std::to_string(::GetLastError());
    return std::nullopt;
  }
  void* view = ::MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
  if (view == nullptr) {
    error = "MapViewOfFile failed: " + std::to_string(::GetLastError());
    ::CloseHandle(handle);
    return std::nullopt;
  }
  MEMORY_BASIC_INFORMATION region{};
  if (::VirtualQuery(view, &region, sizeof(region)) == 0) {
    error = "VirtualQuery failed: " + std::to_string(::GetLastError());
    ::UnmapViewOfFile(view);
    ::CloseHandle(handle);
    return std::nullopt;
  }
  RenderRingSection section;
  section.view_ = view;
  // Page-rounded, so it can exceed what the writer asked for. The reader
  // validates the header against it and never reads past the geometry.
  section.size_ = region.RegionSize;
  section.name_ = name;
  section.owner_ = false;
  section.handle_ = handle;
  return section;
}

void RenderRingSection::unlink() noexcept {}

void RenderRingSection::reset() noexcept {
  if (view_ != nullptr) ::UnmapViewOfFile(view_);
  if (handle_ != nullptr) ::CloseHandle(static_cast<HANDLE>(handle_));
  view_ = nullptr;
  handle_ = nullptr;
  size_ = 0;
  owner_ = false;
}

auto default_render_ring_name() -> std::string { return "SyncRender.frames"; }

#else

auto RenderRingSection::create(const std::string& name, std::size_t bytes, std::string& error)
    -> std::optional<RenderRingSection> {
  // A stale ring from a crashed writer is replaced, never adopted: its
  // geometry and pid belong to a process that no longer exists. The new file
  // is a new inode, so a reader still mapped to the old one keeps a coherent
  // (if silent) view until it re-attaches.
  if (::unlink(name.c_str()) != 0 && errno != ENOENT) {
    error = "unlink failed: " + std::string(std::strerror(errno));
    return std::nullopt;
  }
  const int fd =
      ::open(name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    error = "open failed: " + std::string(std::strerror(errno));
    return std::nullopt;
  }
  if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    error = "ftruncate failed: " + std::string(std::strerror(errno));
    ::close(fd);
    ::unlink(name.c_str());
    return std::nullopt;
  }
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    error = "fstat failed: " + std::string(std::strerror(errno));
    ::close(fd);
    ::unlink(name.c_str());
    return std::nullopt;
  }
  void* view = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (view == MAP_FAILED) {
    error = "mmap failed: " + std::string(std::strerror(errno));
    ::unlink(name.c_str());
    return std::nullopt;
  }
  RenderRingSection section;
  section.view_ = view;
  section.size_ = bytes;
  section.name_ = name;
  section.owner_ = true;
  section.device_ = static_cast<std::uint64_t>(st.st_dev);
  section.inode_ = static_cast<std::uint64_t>(st.st_ino);
  return section;
}

auto RenderRingSection::open(const std::string& name, std::string& error)
    -> std::optional<RenderRingSection> {
  const int fd = ::open(name.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    error = "open failed: " + std::string(std::strerror(errno));
    return std::nullopt;
  }
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    error = "fstat failed: " + std::string(std::strerror(errno));
    ::close(fd);
    return std::nullopt;
  }
  // The ring hands syncd pixels it will publish under the user's name. Only a
  // file this user owns and nobody else can write is trusted to do that.
  if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 077) != 0) {
    error = "section is not a private file owned by this user";
    ::close(fd);
    return std::nullopt;
  }
  if (st.st_size < static_cast<off_t>(render_ring_payload_offset())) {
    error = "section is too small";
    ::close(fd);
    return std::nullopt;
  }
  const auto bytes = static_cast<std::size_t>(st.st_size);
  void* view = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (view == MAP_FAILED) {
    error = "mmap failed: " + std::string(std::strerror(errno));
    return std::nullopt;
  }
  RenderRingSection section;
  section.view_ = view;
  section.size_ = bytes;
  section.name_ = name;
  section.owner_ = false;
  return section;
}

void RenderRingSection::unlink() noexcept {
  if (!owner_ || name_.empty()) return;
  struct stat st{};
  if (::lstat(name_.c_str(), &st) != 0) return;
  if (static_cast<std::uint64_t>(st.st_dev) != device_ ||
      static_cast<std::uint64_t>(st.st_ino) != inode_) {
    return;  // a newer writer owns the name now
  }
  ::unlink(name_.c_str());
}

void RenderRingSection::reset() noexcept {
  if (view_ != nullptr) ::munmap(view_, size_);
  if (owner_) unlink();
  view_ = nullptr;
  size_ = 0;
  owner_ = false;
}

auto default_render_ring_name() -> std::string {
  // $TMPDIR is per-user on macOS; elsewhere it usually is not, which the
  // owner-only mode and the uid check on open cover.
  const char* tmp = std::getenv("TMPDIR");
  std::string dir = (tmp != nullptr && *tmp != '\0') ? tmp : "/tmp";
  if (dir.back() != '/') dir.push_back('/');
  return dir + "SyncRender-" + std::to_string(::geteuid()) + ".frames";
}

#endif

}  // namespace noisefactor::sync::render
