#include "test_harness.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
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
  wchar_t temp[MAX_PATH];
  const DWORD len = ::GetTempPathW(MAX_PATH, temp);
  if (len > 0 && len < MAX_PATH) {
    return std::wstring(temp, len) + L"SyncCameraSinkTest_" +
           std::to_wstring(::GetCurrentProcessId()) + L".frames";
  }
  return L"SyncCameraSinkTest.frames";
}

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

}  // namespace
