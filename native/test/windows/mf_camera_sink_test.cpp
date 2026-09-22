#include "test_harness.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <sync/camera/frame_ring.hpp>
#include <sync/platform/camera_identity.hpp>
#include <sync/platform/mf_camera_sink.hpp>

namespace {

using noisefactor::sync::camera::CameraSinkFrame;
using noisefactor::sync::camera::CameraSinkSubmit;
using noisefactor::sync::camera::CameraSinkUnavailableReason;
using noisefactor::sync::camera::CameraSinkWrite;
using noisefactor::sync::camera::camera_clock_us;
using noisefactor::sync::camera::FrameRingHeader;
using noisefactor::sync::camera::FrameRingReader;
using noisefactor::sync::camera::FrameRingWriter;
using noisefactor::sync::camera::frame_ring_bytes;
using noisefactor::sync::camera::kBytesPerPixel;
using noisefactor::sync::camera::kCanvas;
using noisefactor::sync::camera::kFrameRingDemandTimeoutUs;
using noisefactor::sync::camera::kFrameRingSlotBytes;
using noisefactor::sync::camera::MfCameraSink;
using noisefactor::sync::camera::windows_supports_virtual_cameras;

constexpr std::size_t kStride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;

// Local rather than Global. Creating a Global object needs
// SeCreateGlobalPrivilege, which only a session 0 service has -- the very
// asymmetry that makes the media source, not syncd, own the real section. The
// sink cannot tell the two namespaces apart, so a Local one exercises it
// exactly the same way.
constexpr wchar_t kTestSection[] = L"Local\\SyncCameraTest.frames";

[[nodiscard]] auto test_options() -> MfCameraSink::Options {
  return {.section = kTestSection, .create_virtual_camera = false, .enable_shm = false};
}

[[nodiscard]] auto now_us() -> std::uint64_t { return camera_clock_us(); }

// Stands in for the media source, which is the half that creates the section
// and stamps demand on it whenever a consumer asks for a frame.
struct FakeSource {
  HANDLE section = nullptr;
  void* view = nullptr;

  FakeSource() {
    section = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                   static_cast<DWORD>(frame_ring_bytes()), kTestSection);
    if (section != nullptr) {
      view = ::MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, frame_ring_bytes());
      // The real SectionOwner stamps the ring at creation, so demand can be
      // recorded before any frame exists. Standing in for it means doing the
      // same.
      if (view != nullptr) {
        (void)FrameRingWriter(
            std::span<std::byte>(static_cast<std::byte*>(view), frame_ring_bytes()));
      }
    }
  }

  ~FakeSource() {
    if (view != nullptr) ::UnmapViewOfFile(view);
    if (section != nullptr) ::CloseHandle(section);
  }

  FakeSource(const FakeSource&) = delete;
  auto operator=(const FakeSource&) -> FakeSource& = delete;

  [[nodiscard]] auto mapping() const -> std::span<const std::byte> {
    return {static_cast<const std::byte*>(view), frame_ring_bytes()};
  }

  // What the media source does on every RequestSample, or what tests use
  // to simulate stale demand timestamps without waiting for real time to elapse.
  void demand(std::uint64_t at_us) const {
    if (view != nullptr) {
      auto* header = static_cast<noisefactor::sync::camera::FrameRingHeader*>(view);
      header->last_demand_us.store(at_us, std::memory_order_relaxed);
    }
  }
};

[[nodiscard]] auto canvas_filled(std::uint8_t value) -> std::vector<std::byte> {
  return std::vector<std::byte>(kFrameRingSlotBytes, static_cast<std::byte>(value));
}

[[nodiscard]] auto submission(const std::vector<std::byte>& bgra, std::uint64_t presentation)
    -> CameraSinkFrame {
  return {
      .width = kCanvas.width,
      .height = kCanvas.height,
      .row_stride = kStride,
      .bgra = bgra,
      .presentation_time_us = presentation,
  };
}

// The build number as the registry records it. Not subject to the
// compatibility shim that rewrites what GetVersionEx and VerifyVersionInfo
// report, so it is an independent answer to compare against.
[[nodiscard]] auto registry_build_number() -> std::uint32_t {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
    return 0;
  }
  wchar_t value[32]{};
  DWORD bytes = sizeof(value);
  const LSTATUS status = ::RegQueryValueExW(key, L"CurrentBuildNumber", nullptr, nullptr,
                                            reinterpret_cast<LPBYTE>(value), &bytes);
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS) return 0;
  return static_cast<std::uint32_t>(::_wtoi(value));
}

SYNC_TEST(the_build_check_is_not_fooled_by_the_compatibility_shim) {
  const std::uint32_t build = registry_build_number();
  SYNC_REQUIRE(build != 0);
  // VerifyVersionInfo would answer "older than 22000" on every unmanifested
  // binary, Windows 11 included, and silently disable the camera everywhere.
  SYNC_REQUIRE(windows_supports_virtual_cameras() == (build >= 22000));
}

SYNC_TEST(a_sink_with_no_consumer_is_available_but_has_no_capacity) {
  // No FakeSource in scope, so no consumer has activated the source and there
  // is no section. The camera still exists as a device, so the provider is
  // available; it simply has nowhere to put a frame this instant.
  const MfCameraSink sink(test_options());
  SYNC_REQUIRE(sink.available());
  SYNC_REQUIRE(!sink.has_capacity());
}

SYNC_TEST(a_section_with_no_recent_demand_still_has_no_capacity) {
  const FakeSource source;
  SYNC_REQUIRE(source.view != nullptr);
  const MfCameraSink sink(test_options());
  SYNC_REQUIRE(sink.available());
  // The section exists but nothing has asked for a frame. This is the state
  // after every consumer closes the camera, and it must not read as demand.
  SYNC_REQUIRE(!sink.has_capacity());
}

SYNC_TEST(a_sink_gains_capacity_when_a_consumer_asks_for_a_frame) {
  const FakeSource source;
  MfCameraSink sink(test_options());
  SYNC_REQUIRE(!sink.has_capacity());
  source.demand(now_us());
  SYNC_REQUIRE(sink.has_capacity());
  SYNC_REQUIRE(sink.submit(submission(canvas_filled(0x2B), 42)) == CameraSinkSubmit::Accepted);
}

SYNC_TEST(a_sink_loses_capacity_once_demand_goes_stale) {
  const FakeSource source;
  MfCameraSink sink(test_options());
  source.demand(now_us());
  SYNC_REQUIRE(sink.has_capacity());

  // A consumer that closed the camera stops asking. Without this the writer
  // latches on the first consumer and the publisher goes on fitting 1080p
  // frames into a ring nothing reads, for the rest of the daemon's life.
  // Clamped: steady_clock counts from boot, so on a machine up for less than
  // a couple of seconds this subtraction would wrap and read as fresh demand.
  const std::uint64_t now = now_us();
  const std::uint64_t stale = now > (kFrameRingDemandTimeoutUs + 1'000'000)
                                  ? now - kFrameRingDemandTimeoutUs - 1'000'000
                                  : 1;
  source.demand(stale);
  SYNC_REQUIRE(!sink.has_capacity());
  SYNC_REQUIRE(sink.submit(submission(canvas_filled(1), 1)) == CameraSinkSubmit::Backpressured);
}

SYNC_TEST(a_submitted_frame_lands_in_the_ring) {
  const FakeSource source;
  MfCameraSink sink(test_options());
  source.demand(now_us());
  const auto frame = canvas_filled(0x3C);
  SYNC_REQUIRE(sink.submit(submission(frame, 9999)) == CameraSinkSubmit::Accepted);

  const FrameRingReader reader(source.mapping());
  SYNC_REQUIRE(reader.valid());
  SYNC_REQUIRE(reader.newest_sequence() == 1);
  std::vector<std::byte> out(kFrameRingSlotBytes);
  std::uint64_t presentation = 0;
  SYNC_REQUIRE(reader.read(out, kStride, presentation));
  SYNC_REQUIRE(presentation == 9999);
  SYNC_REQUIRE(static_cast<std::uint8_t>(out[0]) == 0x3C);
}

SYNC_TEST(successive_frames_advance_the_ring) {
  const FakeSource source;
  MfCameraSink sink(test_options());
  source.demand(now_us());
  for (std::uint8_t value = 1; value <= 4; ++value) {
    SYNC_REQUIRE(sink.submit(submission(canvas_filled(value), value)) ==
                 CameraSinkSubmit::Accepted);
  }
  const FrameRingReader reader(source.mapping());
  SYNC_REQUIRE(reader.newest_sequence() == 4);
  std::vector<std::byte> out(kFrameRingSlotBytes);
  std::uint64_t presentation = 0;
  SYNC_REQUIRE(reader.read(out, kStride, presentation));
  SYNC_REQUIRE(static_cast<std::uint8_t>(out[0]) == 4);
}

SYNC_TEST(a_frame_that_is_not_the_canvas_fails_rather_than_corrupting_the_ring) {
  const FakeSource source;
  MfCameraSink sink(test_options());
  source.demand(now_us());
  CameraSinkFrame wrong_size = submission(canvas_filled(0x11), 1);
  wrong_size.height = kCanvas.height / 2;
  SYNC_REQUIRE(sink.submit(wrong_size) == CameraSinkSubmit::Failed);

  const FrameRingReader reader(source.mapping());
  SYNC_REQUIRE(reader.newest_sequence() == 0);
}

SYNC_TEST(submitting_with_no_consumer_is_backpressure_not_failure) {
  MfCameraSink sink(test_options());
  // Nobody is watching, so the frame goes nowhere -- but that is the camera
  // being idle, not the camera being broken, and Failed would be reported to
  // the user as an error.
  SYNC_REQUIRE(sink.submit(submission(canvas_filled(1), 1)) == CameraSinkSubmit::Backpressured);
}

SYNC_TEST(a_sink_picks_up_a_consumer_that_arrives_after_it_started) {
  MfCameraSink sink(test_options());
  SYNC_REQUIRE(!sink.has_capacity());
  // A consumer can open and close the camera many times over one run of the
  // daemon, so the section is retried per frame rather than once at
  // construction the way the CoreMediaIO sink discovers its device.
  const FakeSource source;
  SYNC_REQUIRE(source.view != nullptr);
  source.demand(now_us());
  SYNC_REQUIRE(sink.has_capacity());
  SYNC_REQUIRE(sink.submit(submission(canvas_filled(0x2B), 42)) == CameraSinkSubmit::Accepted);

  const FrameRingReader reader(source.mapping());
  SYNC_REQUIRE(reader.newest_sequence() == 1);
}

[[nodiscard]] auto test_temp_path() -> std::wstring {
  static std::atomic<std::uint32_t> counter{0};
  const auto id = counter.fetch_add(1, std::memory_order_relaxed);
  wchar_t temp[MAX_PATH];
  const DWORD len = ::GetTempPathW(MAX_PATH, temp);
  if (len > 0 && len < MAX_PATH) {
    return std::wstring(temp, len) + L"SyncCameraSinkTest_" +
           std::to_wstring(::GetCurrentProcessId()) + L"_" + std::to_wstring(id) + L".frames";
  }
  return L"SyncCameraSinkTest_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
         std::to_wstring(id) + L".frames";
}

struct ShmMapping {
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE map = nullptr;
  void* view = nullptr;

  explicit ShmMapping(const std::wstring& path) {
    file = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      map = ::CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
      if (map != nullptr) {
        view = ::MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, frame_ring_bytes());
      }
    }
  }

  ~ShmMapping() {
    if (view != nullptr) ::UnmapViewOfFile(view);
    if (map != nullptr) ::CloseHandle(map);
    if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);
  }

  ShmMapping(const ShmMapping&) = delete;
  auto operator=(const ShmMapping&) -> ShmMapping& = delete;

  [[nodiscard]] auto span() const noexcept -> std::span<const std::byte> {
    if (view == nullptr) return {};
    return {static_cast<const std::byte*>(view), frame_ring_bytes()};
  }

  [[nodiscard]] auto header() noexcept -> FrameRingHeader* {
    return static_cast<FrameRingHeader*>(view);
  }
};

SYNC_TEST(a_sink_creates_and_maps_windows_shm_ring_file) {
  const std::wstring shm_file = test_temp_path();
  MfCameraSink::Options opts;
  opts.section = kTestSection;
  opts.create_virtual_camera = false;
  opts.shm_path = shm_file;
  opts.enable_shm = true;

  {
    MfCameraSink sink(opts);
    SYNC_REQUIRE(sink.available());
    SYNC_REQUIRE(!sink.has_capacity());

    const HANDLE hFile = ::CreateFileW(shm_file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    SYNC_REQUIRE(hFile != INVALID_HANDLE_VALUE);

    const HANDLE hMap = ::CreateFileMappingW(hFile, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    SYNC_REQUIRE(hMap != nullptr);

    void* pView = ::MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, frame_ring_bytes());
    SYNC_REQUIRE(pView != nullptr);

    FrameRingReader reader(
        std::span<const std::byte>(static_cast<const std::byte*>(pView), frame_ring_bytes()));
    SYNC_REQUIRE(reader.valid());
    SYNC_REQUIRE(reader.newest_sequence() == 0);

    reader.record_demand(now_us());
    SYNC_REQUIRE(sink.has_capacity());

    SYNC_REQUIRE(sink.submit(submission(canvas_filled(0x42), 8888)) == CameraSinkSubmit::Accepted);
    SYNC_REQUIRE(reader.newest_sequence() == 1);

    std::vector<std::byte> out(kFrameRingSlotBytes);
    std::uint64_t pres = 0;
    SYNC_REQUIRE(reader.read(out, kStride, pres));
    SYNC_REQUIRE(pres == 8888);
    SYNC_REQUIRE(static_cast<std::uint8_t>(out[0]) == 0x42);

    ::UnmapViewOfFile(pView);
    ::CloseHandle(hMap);
    ::CloseHandle(hFile);
  }
  SYNC_REQUIRE(::GetFileAttributesW(shm_file.c_str()) == INVALID_FILE_ATTRIBUTES);
}

SYNC_TEST(a_sink_supports_zero_copy_direct_writer_on_shm_ring) {
  const std::wstring shm_file = test_temp_path();
  MfCameraSink::Options opts;
  opts.section = kTestSection;
  opts.create_virtual_camera = false;
  opts.shm_path = shm_file;
  opts.enable_shm = true;

  {
    MfCameraSink sink(opts);
    const HANDLE hFile = ::CreateFileW(shm_file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    SYNC_REQUIRE(hFile != INVALID_HANDLE_VALUE);
    const HANDLE hMap = ::CreateFileMappingW(hFile, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    void* pView = ::MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, frame_ring_bytes());
    FrameRingReader reader(
        std::span<const std::byte>(static_cast<const std::byte*>(pView), frame_ring_bytes()));
    reader.record_demand(now_us());

    struct TestWriterContext {
      std::uint8_t fill_val = 0x7E;
      bool called = false;
    } ctx;

    const auto write_cb = [](void* context, std::span<std::byte> dest,
                             std::size_t stride) noexcept -> bool {
      (void)stride;
      auto* c = static_cast<TestWriterContext*>(context);
      c->called = true;
      std::fill(dest.begin(), dest.end(), static_cast<std::byte>(c->fill_val));
      return true;
    };

    const auto res = sink.submit_written(write_cb, &ctx, 12345);
    SYNC_REQUIRE(res == CameraSinkWrite::Accepted);
    SYNC_REQUIRE(ctx.called);
    SYNC_REQUIRE(reader.newest_sequence() == 1);

    std::vector<std::byte> out(kFrameRingSlotBytes);
    std::uint64_t pres = 0;
    SYNC_REQUIRE(reader.read(out, kStride, pres));
    SYNC_REQUIRE(pres == 12345);
    SYNC_REQUIRE(static_cast<std::uint8_t>(out[0]) == 0x7E);

    ::UnmapViewOfFile(pView);
    ::CloseHandle(hMap);
    ::CloseHandle(hFile);
  }
}

SYNC_TEST(a_sink_services_both_shm_and_virtual_camera_consumers) {
  const std::wstring shm_file = test_temp_path();
  MfCameraSink::Options opts;
  opts.section = kTestSection;
  opts.create_virtual_camera = false;
  opts.shm_path = shm_file;
  opts.enable_shm = true;

  const FakeSource vcam_source;
  MfCameraSink sink(opts);

  const HANDLE hFile = ::CreateFileW(shm_file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  SYNC_REQUIRE(hFile != INVALID_HANDLE_VALUE);
  const HANDLE hMap = ::CreateFileMappingW(hFile, nullptr, PAGE_READWRITE, 0, 0, nullptr);
  void* pView = ::MapViewOfFile(hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, frame_ring_bytes());
  FrameRingReader shm_reader(
      std::span<const std::byte>(static_cast<const std::byte*>(pView), frame_ring_bytes()));

  const FrameRingReader vcam_reader(vcam_source.mapping());

  // Both demand
  shm_reader.record_demand(now_us());
  vcam_source.demand(now_us());

  SYNC_REQUIRE(sink.has_capacity());

  SYNC_REQUIRE(sink.submit(submission(canvas_filled(0x33), 777)) == CameraSinkSubmit::Accepted);

  SYNC_REQUIRE(shm_reader.newest_sequence() == 1);
  SYNC_REQUIRE(vcam_reader.newest_sequence() == 1);

  std::vector<std::byte> shm_out(kFrameRingSlotBytes);
  std::vector<std::byte> vcam_out(kFrameRingSlotBytes);
  std::uint64_t p1 = 0, p2 = 0;
  SYNC_REQUIRE(shm_reader.read(shm_out, kStride, p1));
  SYNC_REQUIRE(vcam_reader.read(vcam_out, kStride, p2));
  SYNC_REQUIRE(p1 == 777);
  SYNC_REQUIRE(p2 == 777);
  SYNC_REQUIRE(static_cast<std::uint8_t>(shm_out[0]) == 0x33);
  SYNC_REQUIRE(static_cast<std::uint8_t>(vcam_out[0]) == 0x33);

  ::UnmapViewOfFile(pView);
  ::CloseHandle(hMap);
  ::CloseHandle(hFile);
}

SYNC_TEST(a_sink_survives_asymmetric_vcam_disconnect_while_shm_consumer_remains_active) {
  const std::wstring shm_file = test_temp_path();
  MfCameraSink::Options opts;
  opts.section = kTestSection;
  opts.create_virtual_camera = false;
  opts.shm_path = shm_file;
  opts.enable_shm = true;

  const FakeSource vcam_source;
  MfCameraSink sink(opts);

  ShmMapping shm(shm_file);
  SYNC_REQUIRE(shm.view != nullptr);
  FrameRingReader shm_reader(shm.span());
  const FrameRingReader vcam_reader(vcam_source.mapping());

  // Both demand frames initially
  shm_reader.record_demand(now_us());
  vcam_source.demand(now_us());
  SYNC_REQUIRE(sink.has_capacity());

  // First frame is accepted and delivered to both
  SYNC_REQUIRE(sink.submit(submission(canvas_filled(0x11), 101)) == CameraSinkSubmit::Accepted);
  SYNC_REQUIRE(shm_reader.newest_sequence() == 1);
  SYNC_REQUIRE(vcam_reader.newest_sequence() == 1);

  // VCAM consumer disconnects / demand goes stale (> 500ms in the past)
  vcam_source.demand(now_us() - kFrameRingDemandTimeoutUs - 1000);

  // SHM consumer remains active and demands frames
  shm_reader.record_demand(now_us());
  SYNC_REQUIRE(sink.has_capacity());

  // Sink must continue accepting frames for SHM consumer without backpressure or stalling
  SYNC_REQUIRE(sink.submit(submission(canvas_filled(0x22), 102)) == CameraSinkSubmit::Accepted);

  // SHM consumer receives frame 2 cleanly
  SYNC_REQUIRE(shm_reader.newest_sequence() == 2);
  std::vector<std::byte> shm_out(kFrameRingSlotBytes);
  std::uint64_t pts = 0;
  SYNC_REQUIRE(shm_reader.read(shm_out, kStride, pts));
  SYNC_REQUIRE(pts == 102);
  SYNC_REQUIRE(static_cast<std::uint8_t>(shm_out[0]) == 0x22);

  // VCAM ring was untouched (remains at sequence 1)
  SYNC_REQUIRE(vcam_reader.newest_sequence() == 1);

  // VCAM consumer reconnects
  vcam_source.demand(now_us());
  SYNC_REQUIRE(sink.has_capacity());
  SYNC_REQUIRE(sink.submit(submission(canvas_filled(0x33), 103)) == CameraSinkSubmit::Accepted);

  // Both now receive sequence 3 (SHM) and sequence 2 (VCAM)
  SYNC_REQUIRE(shm_reader.newest_sequence() == 3);
  SYNC_REQUIRE(vcam_reader.newest_sequence() == 2);
  SYNC_REQUIRE(vcam_reader.read(shm_out, kStride, pts));
  SYNC_REQUIRE(pts == 103);
  SYNC_REQUIRE(static_cast<std::uint8_t>(shm_out[0]) == 0x33);
}

SYNC_TEST(a_sink_survives_asymmetric_shm_disconnect_while_vcam_consumer_remains_active) {
  const std::wstring shm_file = test_temp_path();
  MfCameraSink::Options opts;
  opts.section = kTestSection;
  opts.create_virtual_camera = false;
  opts.shm_path = shm_file;
  opts.enable_shm = true;

  const FakeSource vcam_source;
  MfCameraSink sink(opts);

  ShmMapping shm(shm_file);
  SYNC_REQUIRE(shm.view != nullptr);
  FrameRingReader shm_reader(shm.span());
  const FrameRingReader vcam_reader(vcam_source.mapping());

  // Both demand frames initially
  shm_reader.record_demand(now_us());
  vcam_source.demand(now_us());
  SYNC_REQUIRE(sink.has_capacity());

  SYNC_REQUIRE(sink.submit(submission(canvas_filled(0xAA), 201)) == CameraSinkSubmit::Accepted);
  SYNC_REQUIRE(shm_reader.newest_sequence() == 1);
  SYNC_REQUIRE(vcam_reader.newest_sequence() == 1);

  // SHM consumer disconnects / demand goes stale
  shm.header()->last_demand_us.store(now_us() - kFrameRingDemandTimeoutUs - 1000,
                                     std::memory_order_release);

  // VCAM consumer remains active
  vcam_source.demand(now_us());
  SYNC_REQUIRE(sink.has_capacity());

  // Sink must continue accepting frames for VCAM consumer without backpressure
  SYNC_REQUIRE(sink.submit(submission(canvas_filled(0xBB), 202)) == CameraSinkSubmit::Accepted);

  // VCAM receives frame 2 cleanly
  SYNC_REQUIRE(vcam_reader.newest_sequence() == 2);
  std::vector<std::byte> vcam_out(kFrameRingSlotBytes);
  std::uint64_t pts = 0;
  SYNC_REQUIRE(vcam_reader.read(vcam_out, kStride, pts));
  SYNC_REQUIRE(pts == 202);
  SYNC_REQUIRE(static_cast<std::uint8_t>(vcam_out[0]) == 0xBB);

  // SHM ring was untouched (remains at sequence 1)
  SYNC_REQUIRE(shm_reader.newest_sequence() == 1);

  // Both disconnect -> sink enters backpressure
  vcam_source.demand(now_us() - kFrameRingDemandTimeoutUs - 1000);
  SYNC_REQUIRE(!sink.has_capacity());
  SYNC_REQUIRE(sink.submit(submission(canvas_filled(0xCC), 203)) ==
               CameraSinkSubmit::Backpressured);
}

SYNC_TEST(asymmetric_zero_copy_submit_written_survives_consumer_disconnect_and_reconnect) {
  const std::wstring shm_file = test_temp_path();
  MfCameraSink::Options opts;
  opts.section = kTestSection;
  opts.create_virtual_camera = false;
  opts.shm_path = shm_file;
  opts.enable_shm = true;

  const FakeSource vcam_source;
  MfCameraSink sink(opts);

  ShmMapping shm(shm_file);
  SYNC_REQUIRE(shm.view != nullptr);
  FrameRingReader shm_reader(shm.span());
  const FrameRingReader vcam_reader(vcam_source.mapping());

  const auto direct_writer = [](void* ctx, std::span<std::byte> dest,
                                std::size_t /*stride*/) noexcept -> bool {
    const auto val = *static_cast<std::uint8_t*>(ctx);
    std::fill(dest.begin(), dest.end(), static_cast<std::byte>(val));
    return true;
  };

  // Both demand initially
  shm_reader.record_demand(now_us());
  vcam_source.demand(now_us());

  std::uint8_t b1 = 0x51;
  SYNC_REQUIRE(sink.submit_written(direct_writer, &b1, 301) == CameraSinkWrite::Accepted);
  SYNC_REQUIRE(shm_reader.newest_sequence() == 1);
  SYNC_REQUIRE(vcam_reader.newest_sequence() == 1);

  // VCAM disconnects -> direct writer writes solely to SHM
  vcam_source.demand(now_us() - kFrameRingDemandTimeoutUs - 1000);
  shm_reader.record_demand(now_us());

  std::uint8_t b2 = 0x52;
  SYNC_REQUIRE(sink.submit_written(direct_writer, &b2, 302) == CameraSinkWrite::Accepted);
  SYNC_REQUIRE(shm_reader.newest_sequence() == 2);
  SYNC_REQUIRE(vcam_reader.newest_sequence() == 1);

  // SHM disconnects and VCAM reconnects -> direct writer writes solely to VCAM
  shm.header()->last_demand_us.store(now_us() - kFrameRingDemandTimeoutUs - 1000,
                                     std::memory_order_release);
  vcam_source.demand(now_us());

  std::uint8_t b3 = 0x53;
  SYNC_REQUIRE(sink.submit_written(direct_writer, &b3, 303) == CameraSinkWrite::Accepted);
  SYNC_REQUIRE(shm_reader.newest_sequence() == 2);
  SYNC_REQUIRE(vcam_reader.newest_sequence() == 2);

  // Both disconnect -> backpressured
  vcam_source.demand(now_us() - kFrameRingDemandTimeoutUs - 1000);
  std::uint8_t b4 = 0x54;
  SYNC_REQUIRE(sink.submit_written(direct_writer, &b4, 304) == CameraSinkWrite::Backpressured);
}

SYNC_TEST(concurrent_multi_reader_shm_and_virtual_camera_seqlock_consistency) {
  const std::wstring shm_file = test_temp_path();
  MfCameraSink::Options opts;
  opts.section = kTestSection;
  opts.create_virtual_camera = false;
  opts.shm_path = shm_file;
  opts.enable_shm = true;

  const FakeSource vcam_source;
  MfCameraSink sink(opts);

  ShmMapping shm(shm_file);
  SYNC_REQUIRE(shm.view != nullptr);

  constexpr int kNumFrames = 40;
  std::atomic<bool> stop{false};
  std::atomic<int> vcam_reads{0};
  std::atomic<int> shm_reads_1{0};
  std::atomic<int> shm_reads_2{0};
  std::atomic<int> corruptions{0};

  auto reader_loop = [&](std::span<const std::byte> mapping, std::atomic<int>& read_counter) {
    FrameRingReader reader(mapping);
    std::vector<std::byte> buf(kFrameRingSlotBytes);
    while (!stop.load(std::memory_order_relaxed)) {
      reader.record_demand(now_us());
      std::uint64_t pts = 0;
      if (reader.read(buf, kStride, pts)) {
        if (pts > 0) {
          const auto expected_byte = static_cast<std::uint8_t>(pts & 0xFF);
          if (static_cast<std::uint8_t>(buf[0]) != expected_byte ||
              static_cast<std::uint8_t>(buf[buf.size() / 2]) != expected_byte ||
              static_cast<std::uint8_t>(buf[buf.size() - 1]) != expected_byte) {
            corruptions.fetch_add(1, std::memory_order_relaxed);
          } else {
            read_counter.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
      std::this_thread::yield();
    }
  };

  std::thread t_vcam(reader_loop, vcam_source.mapping(), std::ref(vcam_reads));
  std::thread t_shm1(reader_loop, shm.span(), std::ref(shm_reads_1));
  std::thread t_shm2(reader_loop, shm.span(), std::ref(shm_reads_2));

  for (int f = 1; f <= kNumFrames; ++f) {
    const auto frame = canvas_filled(static_cast<std::uint8_t>(f & 0xFF));
    const auto sub = submission(frame, static_cast<std::uint64_t>(f));
    while (sink.submit(sub) != CameraSinkSubmit::Accepted) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::this_thread::sleep_for(std::chrono::microseconds(300));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  stop.store(true, std::memory_order_release);

  t_vcam.join();
  t_shm1.join();
  t_shm2.join();

  SYNC_REQUIRE(corruptions.load(std::memory_order_acquire) == 0);
  SYNC_REQUIRE(vcam_reads.load(std::memory_order_acquire) > 0);
  SYNC_REQUIRE(shm_reads_1.load(std::memory_order_acquire) > 0);
  SYNC_REQUIRE(shm_reads_2.load(std::memory_order_acquire) > 0);
}

}  // namespace
