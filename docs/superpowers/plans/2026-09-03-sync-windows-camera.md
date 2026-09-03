# Sync Windows Camera Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the `camera` provider on Windows as a Media Foundation virtual camera, so a Noisedeck output published through Sync appears as "Sync" in any Windows app's camera picker.

**Architecture:** A COM media source DLL (`SyncCamera.dll`) is loaded by the Windows Frame Server in session 0. It creates a shared-memory ring, and syncd — unelevated, in the user's session — opens that ring and writes fitted BGRA frames into it. The media source converts per negotiated media type and hands `IMFSample`s to the Frame Server. The existing `CameraSink` seam, `CameraFramePublisher`, and `CameraRelayPolicy` are reused unchanged.

**Tech Stack:** C++20, CMake 3.21+, MSVC (x64), Media Foundation (`mfplat`, `mfsensorgroup`), Direct2D/DirectWrite, Inno Setup, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-03-sync-windows-camera-design.md`

## Global Constraints

- **Platform floor for the camera:** Windows build **22000**. Below it the provider reports unavailable with a reason; `Sync.iss` keeps `MinVersion=10.0`.
- **Canvas:** fixed **1920×1080**, `kBytesPerPixel = 4`, `kMaximumFramesPerSecond = 60`, from `native/include/sync/platform/camera_identity.hpp`. Do not parameterize.
- **Device name:** `kDeviceName = "Sync Camera"`. Media Foundation appends "Windows Virtual Camera" to the friendly name automatically — pass `"Sync"` as the friendly name so the picker reads "Sync (Windows Virtual Camera)". Do not pass `kDeviceName` verbatim or it reads "Sync Camera Windows Virtual Camera".
- **Pixel formats:** advertise **NV12 first, RGB32 second**. Both ship in the first release.
- **Ring payload:** always top-down BGRA, opaque alpha. Conversion happens in the media source, never in syncd.
- **Elevation:** required only for the HKLM CLSID write. `MFCreateVirtualCamera` is called unelevated by syncd with `MFVirtualCameraAccess_CurrentUser`.
- **CI gate:** the provider does not ship until its end-to-end path runs green in CI. Manual verification on one desk is not sufficient.
- **No temporary artifacts.** `~/platform/CLAUDE.md` bans "for now" hacks. The capability probe in Task 2 is therefore a permanent CI job, not a throwaway workflow.
- **Git:** commit in place on the current branch. No branches, no PRs, no worktrees, no force-push.
- **`kMaximumProviders = 4`** and `cli::kMaximumPublishers = 4` are already at capacity with syphon/spout/ndi/camera. Do not add a fifth provider.

---

### Task 1: Toolchain and a green baseline

Nothing can be verified without a compiler, and `~/platform/CLAUDE.md` bans shipping unverified code. Establish the baseline before touching any source.

**Files:**
- Create: none
- Modify: none

**Interfaces:**
- Consumes: nothing
- Produces: a `build/` tree and a known-green `ctest` run that every later task compares against

- [ ] **Step 1: Install the MSVC build tools and the Windows SDK**

Media Foundation's `mfvirtualcamera.h` is absent from mingw-w64 headers, so MSVC is required — the MinGW path documented in `README.md` cannot build this feature.

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --accept-source-agreements --accept-package-agreements --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --includeRecommended"
```

This needs an interactive UAC approval. If the session cannot elevate, stop and report — every subsequent task depends on it.

- [ ] **Step 2: Install vcpkg and the two dependencies**

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg.exe install libuv:x64-windows openssl:x64-windows
```

- [ ] **Step 3: Configure and build the tree as it stands**

```powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

Expected: builds clean. If it does not, fix the build before proceeding — a red baseline makes every later failure ambiguous.

- [ ] **Step 4: Run the existing suite and record the result**

```powershell
ctest --test-dir build --build-config Release --output-on-failure
```

Expected: all tests pass. Record the count; later tasks must never reduce it.

- [ ] **Step 5: Confirm the machine can host Tier 2**

```powershell
[System.Environment]::OSVersion.Version.Build   # must be >= 22000
Get-Service FrameServer,FrameServerMonitor
Test-Path "$env:SystemRoot\System32\mfsensorgroup.dll"
```

Expected: build 26200, both services present (Stopped/Manual is correct — they start on demand), DLL present.

- [ ] **Step 6: Commit**

No source changed. Nothing to commit; proceed.

---

### Task 2: CI capability probe

Settles §6.3 of the spec: can hosted runners run Tier 1's in-process media source test and Tier 2's real virtual camera test? This job is permanent — it documents runner capability and catches a hosted-image regression later.

**Files:**
- Create: `native/test/windows/camera_capability_probe.cpp`
- Modify: `CMakeLists.txt`, `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: nothing
- Produces: `sync_camera_capability_probe.exe`, exit code 0 when the platform can host a virtual camera, 2 when Media Foundation is present but virtual cameras are not, 3 when Media Foundation itself is absent. Task 14 keys its runner choice off this.

- [ ] **Step 1: Write the probe**

Create `native/test/windows/camera_capability_probe.cpp`:

```cpp
// Reports what the running Windows image can host. Exit codes are the
// contract: 0 = virtual cameras work here, 2 = Media Foundation works but
// virtual cameras do not, 3 = Media Foundation is absent. CI reads these to
// decide whether Tier 2 can run on a hosted runner.

#include <windows.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>

#include <cstdio>

int main() {
  OSVERSIONINFOEXW version{};
  version.dwOSVersionInfoSize = sizeof(version);
  ULONGLONG mask = 0;
  VER_SET_CONDITION(mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);
  version.dwBuildNumber = 22000;
  const bool build_ok = ::VerifyVersionInfoW(&version, VER_BUILDNUMBER, mask) != FALSE;
  std::printf("build_22000_or_later=%d\n", build_ok ? 1 : 0);

  if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
    std::printf("mfplat=0\nverdict=no_media_foundation\n");
    return 3;
  }
  std::printf("mfplat=1\n");

  IMFVirtualCamera* camera = nullptr;
  const HRESULT hr = ::MFCreateVirtualCamera(
      MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
      MFVirtualCameraAccess_CurrentUser, L"Sync Capability Probe",
      L"{00000000-0000-0000-0000-000000000000}", nullptr, 0, &camera);
  std::printf("mfcreatevirtualcamera_hr=0x%08lX\n", static_cast<unsigned long>(hr));

  // A bogus CLSID cannot resolve to a real source, so success is not expected.
  // What distinguishes a capable image is *how* it fails: a capable image
  // rejects the CLSID, an incapable one rejects the call itself.
  const bool capable = (hr != E_NOTIMPL) && (hr != HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
  if (camera != nullptr) {
    camera->Remove();
    camera->Release();
  }
  ::MFShutdown();
  std::printf("verdict=%s\n", capable ? "virtual_cameras_supported" : "no_virtual_cameras");
  return capable ? 0 : 2;
}
```

- [ ] **Step 2: Add the target to CMake**

In `CMakeLists.txt`, inside the `elseif(WIN32)` branch (after the `sync_spout_publisher_tests` block, around line 690):

```cmake
  add_executable(sync_camera_capability_probe
    native/test/windows/camera_capability_probe.cpp
  )
  target_compile_features(sync_camera_capability_probe PRIVATE cxx_std_20)
  target_link_libraries(sync_camera_capability_probe PRIVATE mfplat mfsensorgroup)
```

Deliberately not an `add_test`: it reports capability rather than asserting it, and a hosted runner that cannot host virtual cameras is a fact to record, not a test failure.

- [ ] **Step 3: Build and run it locally**

```powershell
cmake --build build --config Release --target sync_camera_capability_probe
.\build\Release\sync_camera_capability_probe.exe; $LASTEXITCODE
```

Expected on LARGEBOI: `build_22000_or_later=1`, `mfplat=1`, `verdict=virtual_cameras_supported`, exit 0.

- [ ] **Step 4: Add the probe to CI**

In `.github/workflows/ci.yml`, add a step to the existing `windows` job after its build step:

```yaml
      - name: Report camera capability of this runner image
        shell: pwsh
        run: |
          cmake --build build --config $env:BUILD_TYPE --target sync_camera_capability_probe
          & ".\build\$env:BUILD_TYPE\sync_camera_capability_probe.exe"
          "runner_camera_verdict_exit=$LASTEXITCODE" | Tee-Object -Append $env:GITHUB_STEP_SUMMARY
          # Capability is reported, never asserted: this step must not fail the job.
          exit 0
```

- [ ] **Step 5: Commit and read the hosted verdict**

```bash
git add native/test/windows/camera_capability_probe.cpp CMakeLists.txt .github/workflows/ci.yml
git commit -m "test(camera): report Windows runner virtual-camera capability"
git push
```

Then read the run's step summary. Record the verdict — Task 14 branches on it:
- exit 0 → Tier 2 runs on `windows-latest`; no runner changes needed.
- exit 2 or 3 → Tier 2 needs a self-hosted Windows 11 runner (Task 14, Step 6).

---

### Task 3: Move the portable camera core out of the macOS tree

`camera_publisher.cpp` and `relay_policy.cpp` are already portable C++ that happen to live under `native/src/platform/macos/`. Windows needs both unchanged. Moving them is a prerequisite for every later task and changes no behavior.

**Files:**
- Create: `native/src/camera/camera_publisher.cpp` (moved), `native/src/camera/relay_policy.cpp` (moved), `native/test/camera/camera_publisher_test.cpp` (moved), `native/test/camera/relay_policy_test.cpp` (moved)
- Delete: `native/src/platform/macos/camera_publisher.cpp`, `native/src/platform/macos/camera_extension/relay_policy.cpp`, `native/test/macos/camera_publisher_test.cpp`, `native/test/macos/camera_relay_policy_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `CameraSink` (`native/include/sync/platform/camera_sink.hpp`), `CameraFitScratch` and `fit_camera_frame` (`camera_frame_fitter.hpp`)
- Produces: CMake targets `sync_camera_publisher` and `sync_camera_relay_policy`, both available on Windows and macOS

- [ ] **Step 1: Move the files with git so history follows**

```bash
mkdir -p native/src/camera native/test/camera
git mv native/src/platform/macos/camera_publisher.cpp native/src/camera/camera_publisher.cpp
git mv native/src/platform/macos/camera_extension/relay_policy.cpp native/src/camera/relay_policy.cpp
git mv native/test/macos/camera_publisher_test.cpp native/test/camera/camera_publisher_test.cpp
git mv native/test/macos/camera_relay_policy_test.cpp native/test/camera/relay_policy_test.cpp
```

- [ ] **Step 2: Hoist the two targets out of `if(APPLE)`**

In `CMakeLists.txt`, delete the `sync_camera_publisher` block (lines ~466-471) and the `sync_camera_relay_policy` block (lines ~527-531) from the `if(APPLE)` branch, and add both *above* the `if(APPLE)` line, next to `sync_camera_activation` (line ~170):

```cmake
add_library(sync_camera_relay_policy
  native/src/camera/relay_policy.cpp
)
target_compile_features(sync_camera_relay_policy PUBLIC cxx_std_20)
target_include_directories(sync_camera_relay_policy PUBLIC native/include)

add_library(sync_camera_publisher
  native/src/camera/camera_publisher.cpp
)
target_compile_features(sync_camera_publisher PUBLIC cxx_std_20)
target_include_directories(sync_camera_publisher PUBLIC native/include)
target_link_libraries(sync_camera_publisher PUBLIC sync_protocol sync_camera_fitter)
```

`sync_camera_fitter` is still Apple-only at this point; Task 4 makes it portable. Until then this block must stay inside `if(APPLE)` — move `sync_camera_relay_policy` out now and leave `sync_camera_publisher` where it is, moving it in Task 4 once its dependency is portable.

- [ ] **Step 3: Update the macOS test target's source paths**

In the `sync_camera_tests` target (line ~473), change the two moved paths:

```cmake
  add_executable(sync_camera_tests
    native/test/macos/camera_frame_fitter_test.mm
    native/test/macos/camera_idle_card_test.mm
    native/test/camera/camera_publisher_test.cpp
    native/test/camera/relay_policy_test.cpp
    native/test/test_main.cpp
  )
```

- [ ] **Step 4: Add a Windows test target for the relay policy**

In the `elseif(WIN32)` branch:

```cmake
  add_executable(sync_camera_relay_policy_tests
    native/test/camera/relay_policy_test.cpp
    native/test/test_main.cpp
  )
  target_compile_features(sync_camera_relay_policy_tests PRIVATE cxx_std_20)
  target_include_directories(sync_camera_relay_policy_tests PRIVATE native/test)
  target_link_libraries(sync_camera_relay_policy_tests PRIVATE sync_camera_relay_policy)
  add_test(NAME sync_camera_relay_policy_tests COMMAND sync_camera_relay_policy_tests)
```

- [ ] **Step 5: Build and run**

```powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

Expected: the baseline count from Task 1 plus `sync_camera_relay_policy_tests`, all passing.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor(camera): move the portable camera core out of the macOS tree"
```

---

### Task 4: A portable frame fitter

`fit_camera_frame` is vImage-only today and its header hard-errors off Apple. Windows needs the same contract without Accelerate.

**Files:**
- Create: `native/src/camera/frame_fitter.cpp`, `native/test/camera/frame_fitter_test.cpp`
- Modify: `native/include/sync/platform/camera_frame_fitter.hpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `protocol::FrameView`, `CameraCanvas`, `CameraPlacement`, `CameraFitScratch`
- Produces: the same `compute_camera_placement` and `fit_camera_frame` signatures the header already declares, now available on every platform

- [ ] **Step 1: Drop the platform guard from the header**

In `native/include/sync/platform/camera_frame_fitter.hpp`, delete these three lines from the top:

```cpp
#if !defined(__APPLE__)
#error "camera_frame_fitter.hpp is available only on Apple platforms"
#endif
```

The `CameraFitScratch` comment mentioning "vImage's temporary buffer" is now wrong for the portable path. Change that member's comment to:

```cpp
  // Scratch for the scale step. Sized by the implementation; unused when the
  // frame already matches the canvas.
  std::vector<std::byte> scale_temp;
```

- [ ] **Step 2: Write the failing test**

Create `native/test/camera/frame_fitter_test.cpp`:

```cpp
#include "test_harness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <sync/platform/camera_frame_fitter.hpp>
#include <sync/platform/camera_identity.hpp>

namespace {

using noisefactor::sync::camera::CameraFitScratch;
using noisefactor::sync::camera::CameraPlacement;
using noisefactor::sync::camera::compute_camera_placement;
using noisefactor::sync::camera::fit_camera_frame;
using noisefactor::sync::camera::kBytesPerPixel;
using noisefactor::sync::camera::kCanvas;
using noisefactor::sync::protocol::FrameView;

constexpr std::uint16_t kPixelFormatRgba8 = 1;
constexpr std::uint16_t kAlphaOpaque = 1;
constexpr std::uint16_t kAlphaStraight = 2;

[[nodiscard]] auto solid_rgba(std::uint32_t width, std::uint32_t height, std::uint8_t r,
                              std::uint8_t g, std::uint8_t b, std::uint8_t a)
    -> std::vector<std::byte> {
  std::vector<std::byte> pixels(static_cast<std::size_t>(width) * height * kBytesPerPixel);
  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i + 0] = static_cast<std::byte>(r);
    pixels[i + 1] = static_cast<std::byte>(g);
    pixels[i + 2] = static_cast<std::byte>(b);
    pixels[i + 3] = static_cast<std::byte>(a);
  }
  return pixels;
}

[[nodiscard]] auto frame_over(const std::vector<std::byte>& payload, std::uint32_t width,
                              std::uint32_t height, std::uint16_t alpha_mode) -> FrameView {
  return {
      .version = 1,
      .header_bytes = 64,
      .flags = 1,
      .pixel_format = kPixelFormatRgba8,
      .color_space = 1,
      .alpha_mode = alpha_mode,
      .width = width,
      .height = height,
      .row_stride = width * kBytesPerPixel,
      .payload_bytes = static_cast<std::uint32_t>(payload.size()),
      .sequence = 1,
      .presentation_time_us = 0,
      .top_down = true,
      .payload = payload,
  };
}

SYNC_TEST(placement_letterboxes_a_wide_source) {
  const auto placement = compute_camera_placement(3840, 1080, kCanvas);
  SYNC_REQUIRE(placement.has_value());
  SYNC_REQUIRE(placement->width == 1920);
  SYNC_REQUIRE(placement->height == 540);
  SYNC_REQUIRE(placement->x == 0);
  SYNC_REQUIRE(placement->y == 270);
}

SYNC_TEST(placement_pillarboxes_a_tall_source) {
  const auto placement = compute_camera_placement(1080, 1920, kCanvas);
  SYNC_REQUIRE(placement.has_value());
  SYNC_REQUIRE(placement->height == 1080);
  SYNC_REQUIRE(placement->width == 607);
  SYNC_REQUIRE(placement->y == 0);
  SYNC_REQUIRE(placement->x == 656);
}

SYNC_TEST(placement_rejects_a_zero_dimension) {
  SYNC_REQUIRE(!compute_camera_placement(0, 1080, kCanvas).has_value());
  SYNC_REQUIRE(!compute_camera_placement(1920, 0, kCanvas).has_value());
}

SYNC_TEST(fit_permutes_rgba_to_bgra_and_forces_opaque) {
  // Pure red, RGBA, already opaque.
  const auto payload = solid_rgba(1920, 1080, 255, 0, 0, 255);
  const auto frame = frame_over(payload, 1920, 1080, kAlphaOpaque);
  const std::size_t stride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;
  std::vector<std::byte> canvas(stride * kCanvas.height);
  CameraFitScratch scratch;
  SYNC_REQUIRE(fit_camera_frame(frame, canvas, stride, kCanvas, scratch));
  // BGRA: blue 0, green 0, red 255, alpha 255.
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[0]) == 0);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[1]) == 0);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[2]) == 255);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[3]) == 255);
}

SYNC_TEST(fit_premultiplies_straight_alpha_over_black) {
  // White at 50% straight alpha premultiplies to mid grey, then opaque.
  const auto payload = solid_rgba(1920, 1080, 255, 255, 255, 128);
  const auto frame = frame_over(payload, 1920, 1080, kAlphaStraight);
  const std::size_t stride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;
  std::vector<std::byte> canvas(stride * kCanvas.height);
  CameraFitScratch scratch;
  SYNC_REQUIRE(fit_camera_frame(frame, canvas, stride, kCanvas, scratch));
  const auto blue = static_cast<std::uint8_t>(canvas[0]);
  SYNC_REQUIRE(blue >= 127 && blue <= 129);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[3]) == 255);
}

SYNC_TEST(fit_paints_bars_black_around_a_letterboxed_source) {
  const auto payload = solid_rgba(3840, 1080, 255, 255, 255, 255);
  const auto frame = frame_over(payload, 3840, 1080, kAlphaOpaque);
  const std::size_t stride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;
  std::vector<std::byte> canvas(stride * kCanvas.height, static_cast<std::byte>(0xAB));
  CameraFitScratch scratch;
  SYNC_REQUIRE(fit_camera_frame(frame, canvas, stride, kCanvas, scratch));
  // Top-left is inside the top bar: opaque black.
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[0]) == 0);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[1]) == 0);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[2]) == 0);
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[3]) == 255);
  // Centre is inside the placement: white.
  const std::size_t centre = (kCanvas.height / 2) * stride + (kCanvas.width / 2) * 4;
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[centre]) == 255);
}

SYNC_TEST(fit_rejects_a_bottom_up_frame) {
  const auto payload = solid_rgba(1920, 1080, 255, 0, 0, 255);
  auto frame = frame_over(payload, 1920, 1080, kAlphaOpaque);
  frame.top_down = false;
  const std::size_t stride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;
  std::vector<std::byte> canvas(stride * kCanvas.height);
  CameraFitScratch scratch;
  SYNC_REQUIRE(!fit_camera_frame(frame, canvas, stride, kCanvas, scratch));
}

SYNC_TEST(fit_rejects_a_canvas_buffer_that_is_too_small) {
  const auto payload = solid_rgba(1920, 1080, 255, 0, 0, 255);
  const auto frame = frame_over(payload, 1920, 1080, kAlphaOpaque);
  const std::size_t stride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;
  std::vector<std::byte> canvas(stride * (kCanvas.height - 1));
  CameraFitScratch scratch;
  SYNC_REQUIRE(!fit_camera_frame(frame, canvas, stride, kCanvas, scratch));
}

}  // namespace
```

- [ ] **Step 3: Run it and watch it fail**

```powershell
cmake --build build --config Release --target sync_camera_fitter_tests
```

Expected: FAIL — the target does not exist yet and `camera_frame_fitter.hpp` has no non-Apple implementation.

- [ ] **Step 4: Write the portable implementation**

Create `native/src/camera/frame_fitter.cpp`. This file is compiled only when not Apple; macOS keeps the vImage path.

```cpp
#include <sync/platform/camera_frame_fitter.hpp>

#include <algorithm>
#include <cstring>

namespace noisefactor::sync::camera {

namespace {

constexpr std::uint16_t kPixelFormatRgba8 = 1;
constexpr std::uint16_t kAlphaStraight = 2;

[[nodiscard]] auto frame_is_fittable(const protocol::FrameView& frame) noexcept -> bool {
  if (frame.pixel_format != kPixelFormatRgba8 || !frame.top_down) return false;
  if (frame.width == 0 || frame.height == 0) return false;
  const std::uint64_t packed = static_cast<std::uint64_t>(frame.width) * kBytesPerPixel;
  if (frame.row_stride < packed) return false;
  const std::uint64_t needed = static_cast<std::uint64_t>(frame.row_stride) * frame.height;
  return frame.payload.size() >= needed;
}

// 255-scaled premultiply that rounds like vImage: (value * alpha + 127) / 255.
[[nodiscard]] constexpr auto premultiply(std::uint8_t value, std::uint8_t alpha) noexcept
    -> std::uint8_t {
  const std::uint32_t scaled = static_cast<std::uint32_t>(value) * alpha + 127U;
  return static_cast<std::uint8_t>((scaled + (scaled >> 8)) >> 8);
}

void fill_black(std::span<std::byte> canvas_bytes, std::size_t canvas_stride, std::uint32_t x,
                std::uint32_t y, std::uint32_t width, std::uint32_t height) noexcept {
  if (width == 0 || height == 0) return;
  for (std::uint32_t row = 0; row < height; ++row) {
    std::byte* out = canvas_bytes.data() + static_cast<std::size_t>(y + row) * canvas_stride +
                     static_cast<std::size_t>(x) * kBytesPerPixel;
    for (std::uint32_t column = 0; column < width; ++column) {
      out[0] = std::byte{0};
      out[1] = std::byte{0};
      out[2] = std::byte{0};
      out[3] = std::byte{255};
      out += kBytesPerPixel;
    }
  }
}

// Nearest-neighbour scale from the source into the placement, permuting RGBA
// to BGRA, premultiplying straight alpha over black, and forcing opaque. One
// pass, no intermediate buffer: the destination is written exactly once.
void scale_permute_into(const protocol::FrameView& frame, std::span<std::byte> canvas_bytes,
                        std::size_t canvas_stride, const CameraPlacement& placement) noexcept {
  const bool straight = frame.alpha_mode == kAlphaStraight;
  for (std::uint32_t row = 0; row < placement.height; ++row) {
    // Sample the centre of the destination texel, not its corner: corner
    // sampling biases the whole image up and left by half a source pixel.
    const std::uint64_t source_row =
        ((static_cast<std::uint64_t>(row) * 2 + 1) * frame.height) / (placement.height * 2);
    const std::byte* in_row =
        frame.payload.data() + std::min<std::uint64_t>(source_row, frame.height - 1) *
                                   frame.row_stride;
    std::byte* out = canvas_bytes.data() +
                     static_cast<std::size_t>(placement.y + row) * canvas_stride +
                     static_cast<std::size_t>(placement.x) * kBytesPerPixel;
    for (std::uint32_t column = 0; column < placement.width; ++column) {
      const std::uint64_t source_column =
          ((static_cast<std::uint64_t>(column) * 2 + 1) * frame.width) / (placement.width * 2);
      const std::byte* in =
          in_row + std::min<std::uint64_t>(source_column, frame.width - 1) * kBytesPerPixel;
      const auto r = static_cast<std::uint8_t>(in[0]);
      const auto g = static_cast<std::uint8_t>(in[1]);
      const auto b = static_cast<std::uint8_t>(in[2]);
      const auto a = static_cast<std::uint8_t>(in[3]);
      out[0] = static_cast<std::byte>(straight ? premultiply(b, a) : b);
      out[1] = static_cast<std::byte>(straight ? premultiply(g, a) : g);
      out[2] = static_cast<std::byte>(straight ? premultiply(r, a) : r);
      out[3] = std::byte{255};
      out += kBytesPerPixel;
    }
  }
}

}  // namespace

auto compute_camera_placement(std::uint32_t source_width, std::uint32_t source_height,
                              CameraCanvas canvas) noexcept -> std::optional<CameraPlacement> {
  if (source_width == 0 || source_height == 0 || canvas.width == 0 || canvas.height == 0) {
    return std::nullopt;
  }
  const std::uint64_t lhs = static_cast<std::uint64_t>(source_width) * canvas.height;
  const std::uint64_t rhs = static_cast<std::uint64_t>(canvas.width) * source_height;
  CameraPlacement placement{};
  if (lhs >= rhs) {
    placement.width = canvas.width;
    placement.height = static_cast<std::uint32_t>(std::max<std::uint64_t>(
        1, (static_cast<std::uint64_t>(canvas.width) * source_height) / source_width));
  } else {
    placement.height = canvas.height;
    placement.width = static_cast<std::uint32_t>(std::max<std::uint64_t>(
        1, (static_cast<std::uint64_t>(canvas.height) * source_width) / source_height));
  }
  placement.x = (canvas.width - placement.width) / 2;
  placement.y = (canvas.height - placement.height) / 2;
  return placement;
}

auto fit_camera_frame(const protocol::FrameView& frame, std::span<std::byte> canvas_bytes,
                      std::size_t canvas_stride, CameraCanvas canvas,
                      CameraFitScratch& scratch) noexcept -> bool {
  (void)scratch;  // The one-pass path needs no working memory.
  if (!frame_is_fittable(frame)) return false;
  if (canvas_stride < static_cast<std::size_t>(canvas.width) * kBytesPerPixel) return false;
  if (canvas_bytes.size() < canvas_stride * canvas.height) return false;
  const auto placement = compute_camera_placement(frame.width, frame.height, canvas);
  if (!placement.has_value()) return false;
  const std::uint32_t bottom = placement->y + placement->height;
  const std::uint32_t right = placement->x + placement->width;
  fill_black(canvas_bytes, canvas_stride, 0, 0, canvas.width, placement->y);
  fill_black(canvas_bytes, canvas_stride, 0, bottom, canvas.width, canvas.height - bottom);
  fill_black(canvas_bytes, canvas_stride, 0, placement->y, placement->x, placement->height);
  fill_black(canvas_bytes, canvas_stride, right, placement->y, canvas.width - right,
             placement->height);
  scale_permute_into(frame, canvas_bytes, canvas_stride, *placement);
  return true;
}

auto fit_camera_frame(const protocol::FrameView& frame, std::span<std::byte> canvas_bytes,
                      std::size_t canvas_stride, CameraCanvas canvas) noexcept -> bool {
  CameraFitScratch scratch;
  return fit_camera_frame(frame, canvas_bytes, canvas_stride, canvas, scratch);
}

}  // namespace noisefactor::sync::camera
```

- [ ] **Step 5: Wire the target and move `sync_camera_publisher` out of `if(APPLE)`**

In `CMakeLists.txt`, above the `if(APPLE)` line, add a portable fitter that picks its implementation per platform, and move the publisher block from Task 3 Step 2 here:

```cmake
if(APPLE)
  set(SYNC_CAMERA_FITTER_SOURCE native/src/platform/macos/camera_frame_fitter.mm)
else()
  set(SYNC_CAMERA_FITTER_SOURCE native/src/camera/frame_fitter.cpp)
endif()
add_library(sync_camera_fitter ${SYNC_CAMERA_FITTER_SOURCE})
target_compile_features(sync_camera_fitter PUBLIC cxx_std_20)
target_include_directories(sync_camera_fitter PUBLIC native/include)
target_link_libraries(sync_camera_fitter PUBLIC sync_protocol)
if(APPLE)
  set_target_properties(sync_camera_fitter PROPERTIES OBJCXX_STANDARD 20)
  target_link_libraries(sync_camera_fitter PRIVATE ${ACCELERATE_FRAMEWORK})
endif()
```

Delete the old Apple-only `sync_camera_fitter` block (lines ~437-451) and move the `sync_camera_publisher` block out of `if(APPLE)` to sit directly beneath this one.

Then add the Windows test target inside `elseif(WIN32)`:

```cmake
  add_executable(sync_camera_fitter_tests
    native/test/camera/frame_fitter_test.cpp
    native/test/camera/camera_publisher_test.cpp
    native/test/test_main.cpp
  )
  target_compile_features(sync_camera_fitter_tests PRIVATE cxx_std_20)
  target_include_directories(sync_camera_fitter_tests PRIVATE native/test)
  target_link_libraries(sync_camera_fitter_tests PRIVATE sync_camera_publisher sync_camera_fitter)
  add_test(NAME sync_camera_fitter_tests COMMAND sync_camera_fitter_tests)
```

- [ ] **Step 6: Run the tests**

```powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure -R sync_camera
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat(camera): fit frames to the canvas without Accelerate"
```

---

### Task 5: BGRA to NV12 conversion

The camera advertises NV12 first, so this conversion is on the hot path for most consumers. It lives in a portable library specifically so CI covers it with no camera present.

**Files:**
- Create: `native/include/sync/camera/nv12.hpp`, `native/src/camera/nv12.cpp`, `native/test/camera/nv12_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing
- Produces: `noisefactor::sync::camera::bgra_to_nv12(std::span<const std::byte> bgra, std::size_t bgra_stride, std::uint32_t width, std::uint32_t height, std::span<std::byte> nv12, std::size_t y_stride) -> bool` and `nv12_size_bytes(std::uint32_t width, std::uint32_t height, std::size_t y_stride) -> std::size_t`. Task 8 calls both.

- [ ] **Step 1: Write the header**

Create `native/include/sync/camera/nv12.hpp`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace noisefactor::sync::camera {

// Bytes an NV12 image occupies: a full-size Y plane followed by a half-height
// interleaved UV plane, both at y_stride.
[[nodiscard]] auto nv12_size_bytes(std::uint32_t width, std::uint32_t height,
                                   std::size_t y_stride) noexcept -> std::size_t;

// Converts top-down opaque BGRA to NV12 (BT.601, studio range), the pairing
// Media Foundation's capture pipeline assumes for a 1080p camera. Chroma is
// box-averaged over each 2x2 block. Odd dimensions clamp to the last row and
// column rather than reading past the image.
//
// Returns false when either dimension is zero, when a stride is too small for
// its width, or when a buffer is too small for its plane.
[[nodiscard]] auto bgra_to_nv12(std::span<const std::byte> bgra, std::size_t bgra_stride,
                                std::uint32_t width, std::uint32_t height,
                                std::span<std::byte> nv12, std::size_t y_stride) noexcept -> bool;

}  // namespace noisefactor::sync::camera
```

- [ ] **Step 2: Write the failing test**

Create `native/test/camera/nv12_test.cpp`:

```cpp
#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <sync/camera/nv12.hpp>

namespace {

using noisefactor::sync::camera::bgra_to_nv12;
using noisefactor::sync::camera::nv12_size_bytes;

[[nodiscard]] auto solid_bgra(std::uint32_t width, std::uint32_t height, std::uint8_t b,
                              std::uint8_t g, std::uint8_t r) -> std::vector<std::byte> {
  std::vector<std::byte> pixels(static_cast<std::size_t>(width) * height * 4);
  for (std::size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i + 0] = static_cast<std::byte>(b);
    pixels[i + 1] = static_cast<std::byte>(g);
    pixels[i + 2] = static_cast<std::byte>(r);
    pixels[i + 3] = static_cast<std::byte>(255);
  }
  return pixels;
}

SYNC_TEST(nv12_size_is_one_and_a_half_planes) {
  SYNC_REQUIRE(nv12_size_bytes(1920, 1080, 1920) == 1920 * 1080 * 3 / 2);
  // An odd height still needs a full chroma row for the trailing luma row.
  SYNC_REQUIRE(nv12_size_bytes(4, 3, 4) == 4 * 3 + 4 * 2);
}

SYNC_TEST(black_converts_to_studio_range_floor) {
  const auto bgra = solid_bgra(4, 4, 0, 0, 0);
  std::vector<std::byte> nv12(nv12_size_bytes(4, 4, 4));
  SYNC_REQUIRE(bgra_to_nv12(bgra, 16, 4, 4, nv12, 4));
  SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[0]) == 16);
  SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[16]) == 128);   // U
  SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[17]) == 128);   // V
}

SYNC_TEST(white_converts_to_studio_range_ceiling) {
  const auto bgra = solid_bgra(4, 4, 255, 255, 255);
  std::vector<std::byte> nv12(nv12_size_bytes(4, 4, 4));
  SYNC_REQUIRE(bgra_to_nv12(bgra, 16, 4, 4, nv12, 4));
  SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[0]) == 235);
  SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[16]) == 128);
  SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[17]) == 128);
}

SYNC_TEST(pure_red_lands_on_known_chroma) {
  const auto bgra = solid_bgra(2, 2, 0, 0, 255);
  std::vector<std::byte> nv12(nv12_size_bytes(2, 2, 2));
  SYNC_REQUIRE(bgra_to_nv12(bgra, 8, 2, 2, nv12, 2));
  // BT.601 studio red: Y 81, U 90, V 240.
  const auto y = static_cast<std::uint8_t>(nv12[0]);
  const auto u = static_cast<std::uint8_t>(nv12[4]);
  const auto v = static_cast<std::uint8_t>(nv12[5]);
  SYNC_REQUIRE(y >= 80 && y <= 82);
  SYNC_REQUIRE(u >= 89 && u <= 91);
  SYNC_REQUIRE(v >= 239 && v <= 241);
}

SYNC_TEST(odd_dimensions_clamp_rather_than_read_past_the_image) {
  const auto bgra = solid_bgra(3, 3, 0, 0, 255);
  std::vector<std::byte> nv12(nv12_size_bytes(3, 3, 3));
  SYNC_REQUIRE(bgra_to_nv12(bgra, 12, 3, 3, nv12, 3));
  // Every chroma sample sees only red, so the edge block matches the interior.
  SYNC_REQUIRE(static_cast<std::uint8_t>(nv12[9]) == static_cast<std::uint8_t>(nv12[11]));
}

SYNC_TEST(rejects_undersized_buffers_and_strides) {
  const auto bgra = solid_bgra(4, 4, 0, 0, 0);
  std::vector<std::byte> nv12(nv12_size_bytes(4, 4, 4));
  SYNC_REQUIRE(!bgra_to_nv12(bgra, 8, 4, 4, nv12, 4));       // bgra_stride too small
  SYNC_REQUIRE(!bgra_to_nv12(bgra, 16, 4, 4, nv12, 2));      // y_stride too small
  SYNC_REQUIRE(!bgra_to_nv12(bgra, 16, 0, 4, nv12, 4));      // zero width
  std::vector<std::byte> tiny(4);
  SYNC_REQUIRE(!bgra_to_nv12(bgra, 16, 4, 4, tiny, 4));      // output too small
}

}  // namespace
```

- [ ] **Step 3: Run it and watch it fail**

```powershell
cmake --build build --config Release --target sync_camera_nv12_tests
```

Expected: FAIL — target does not exist.

- [ ] **Step 4: Implement the conversion**

Create `native/src/camera/nv12.cpp`:

```cpp
#include <sync/camera/nv12.hpp>

#include <algorithm>

namespace noisefactor::sync::camera {

namespace {

constexpr std::size_t kBgraBytesPerPixel = 4;

// BT.601 studio range, fixed point in 16.16. Y in [16,235], chroma in [16,240]
// centred on 128 -- the pairing Media Foundation assumes for an SD-origin
// camera format, and what consumers render correctly without a colour hint.
constexpr std::int32_t kYr = 16829, kYg = 33039, kYb = 6416, kYOffset = 16 << 16;
constexpr std::int32_t kUr = -9714, kUg = -19071, kUb = 28784, kChromaOffset = 128 << 16;
constexpr std::int32_t kVr = 28784, kVg = -24103, kVb = -4681;

[[nodiscard]] constexpr auto clamp_byte(std::int32_t fixed) noexcept -> std::uint8_t {
  const std::int32_t value = (fixed + (1 << 15)) >> 16;
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

}  // namespace

auto nv12_size_bytes(std::uint32_t width, std::uint32_t height, std::size_t y_stride) noexcept
    -> std::size_t {
  (void)width;
  const std::size_t chroma_rows = (static_cast<std::size_t>(height) + 1) / 2;
  return y_stride * height + y_stride * chroma_rows;
}

auto bgra_to_nv12(std::span<const std::byte> bgra, std::size_t bgra_stride, std::uint32_t width,
                  std::uint32_t height, std::span<std::byte> nv12, std::size_t y_stride) noexcept
    -> bool {
  if (width == 0 || height == 0) return false;
  if (bgra_stride < static_cast<std::size_t>(width) * kBgraBytesPerPixel) return false;
  if (y_stride < width) return false;
  if (bgra.size() < bgra_stride * height) return false;
  if (nv12.size() < nv12_size_bytes(width, height, y_stride)) return false;

  std::byte* const luma = nv12.data();
  std::byte* const chroma = nv12.data() + y_stride * height;

  for (std::uint32_t row = 0; row < height; ++row) {
    const std::byte* in = bgra.data() + static_cast<std::size_t>(row) * bgra_stride;
    std::byte* out = luma + static_cast<std::size_t>(row) * y_stride;
    for (std::uint32_t column = 0; column < width; ++column) {
      const auto b = static_cast<std::int32_t>(static_cast<std::uint8_t>(in[0]));
      const auto g = static_cast<std::int32_t>(static_cast<std::uint8_t>(in[1]));
      const auto r = static_cast<std::int32_t>(static_cast<std::uint8_t>(in[2]));
      out[column] = static_cast<std::byte>(clamp_byte(kYr * r + kYg * g + kYb * b + kYOffset));
      in += kBgraBytesPerPixel;
    }
  }

  for (std::uint32_t row = 0; row < height; row += 2) {
    std::byte* out = chroma + static_cast<std::size_t>(row / 2) * y_stride;
    for (std::uint32_t column = 0; column < width; column += 2) {
      // Box-average the 2x2 block, clamping at the right and bottom edges so
      // an odd dimension samples the last row or column twice instead of
      // reading past the image.
      std::int32_t sum_r = 0, sum_g = 0, sum_b = 0;
      for (std::uint32_t dy = 0; dy < 2; ++dy) {
        const std::uint32_t sample_row = std::min(row + dy, height - 1);
        const std::byte* in_row = bgra.data() + static_cast<std::size_t>(sample_row) * bgra_stride;
        for (std::uint32_t dx = 0; dx < 2; ++dx) {
          const std::uint32_t sample_column = std::min(column + dx, width - 1);
          const std::byte* in = in_row + static_cast<std::size_t>(sample_column) * kBgraBytesPerPixel;
          sum_b += static_cast<std::int32_t>(static_cast<std::uint8_t>(in[0]));
          sum_g += static_cast<std::int32_t>(static_cast<std::uint8_t>(in[1]));
          sum_r += static_cast<std::int32_t>(static_cast<std::uint8_t>(in[2]));
        }
      }
      const std::int32_t r = sum_r / 4, g = sum_g / 4, b = sum_b / 4;
      out[column + 0] = static_cast<std::byte>(clamp_byte(kUr * r + kUg * g + kUb * b + kChromaOffset));
      out[column + 1] = static_cast<std::byte>(clamp_byte(kVr * r + kVg * g + kVb * b + kChromaOffset));
    }
  }
  return true;
}

}  // namespace noisefactor::sync::camera
```

- [ ] **Step 5: Wire the target**

Above `if(APPLE)` in `CMakeLists.txt`:

```cmake
add_library(sync_camera_nv12
  native/src/camera/nv12.cpp
)
target_compile_features(sync_camera_nv12 PUBLIC cxx_std_20)
target_include_directories(sync_camera_nv12 PUBLIC native/include)
```

And a test target that builds on both platforms — add it above `if(APPLE)` too, since NV12 has no platform dependency:

```cmake
add_executable(sync_camera_nv12_tests
  native/test/camera/nv12_test.cpp
  native/test/test_main.cpp
)
target_compile_features(sync_camera_nv12_tests PRIVATE cxx_std_20)
target_include_directories(sync_camera_nv12_tests PRIVATE native/test)
target_link_libraries(sync_camera_nv12_tests PRIVATE sync_camera_nv12)
add_test(NAME sync_camera_nv12_tests COMMAND sync_camera_nv12_tests)
```

- [ ] **Step 6: Run the tests**

```powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure -R nv12
```

Expected: PASS. If `pure_red_lands_on_known_chroma` fails, the coefficients are wrong — do not widen the tolerance to make it pass.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat(camera): convert the canvas to NV12 for Media Foundation consumers"
```

---

### Task 6: The shared frame ring

The transport CoreMediaIO provides for free on macOS. Layout and naming live in a portable header so both sides agree by construction and CI tests the logic with no camera present.

**Files:**
- Create: `native/include/sync/camera/frame_ring.hpp`, `native/src/camera/frame_ring.cpp`, `native/test/camera/frame_ring_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `kCanvas`, `kBytesPerPixel`
- Produces: `FrameRingHeader`, `kFrameRingSlots`, `frame_ring_bytes()`, `section_name(std::wstring_view sid)`, `frame_event_name(std::wstring_view sid)`, `FrameRingWriter`, `FrameRingReader`. Task 8 uses the reader, Task 10 the writer.

- [ ] **Step 1: Write the header**

Create `native/include/sync/camera/frame_ring.hpp`:

```cpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <sync/platform/camera_identity.hpp>

namespace noisefactor::sync::camera {

// Three slots: one being written, one being read, one spare. Matches the
// queue depth CmioCameraSink uses on macOS so both platforms drop frames at
// the same point under load.
inline constexpr std::uint32_t kFrameRingSlots = 3;
inline constexpr std::uint32_t kFrameRingMagic = 0x53594E43;  // 'SYNC'
inline constexpr std::uint32_t kFrameRingVersion = 1;

// One slot's payload: the full canvas as top-down BGRA.
inline constexpr std::size_t kFrameRingSlotBytes =
    static_cast<std::size_t>(kCanvas.width) * kCanvas.height * kBytesPerPixel;

// Written by the media source at creation, read by both sides. Sequence
// numbers are the synchronisation: a writer bumps `sequence` to an odd value
// before writing a slot and to the next even value after, so a reader that
// sees an odd or changed sequence knows it read a torn frame and retries.
struct FrameRingSlot {
  std::atomic<std::uint64_t> sequence;
  std::uint64_t presentation_time_us;
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t row_stride;
  std::uint32_t reserved;
};

struct FrameRingHeader {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint32_t slots;
  std::uint32_t slot_bytes;
  std::atomic<std::uint64_t> newest;  // monotonic count of published frames
  FrameRingSlot slot[kFrameRingSlots];
};

[[nodiscard]] constexpr auto frame_ring_bytes() noexcept -> std::size_t {
  return sizeof(FrameRingHeader) + kFrameRingSlotBytes * kFrameRingSlots;
}

// Kernel object names. Both live in the Global namespace because the media
// source runs in session 0 and syncd in the user's session. Deliberately not
// keyed by user: the media source cannot be told which user to pair with
// without an administrator-only API, so it grants INTERACTIVE instead and
// whoever is logged in feeds the camera.
[[nodiscard]] auto section_name() -> std::wstring;
[[nodiscard]] auto frame_event_name() -> std::wstring;

// Writes frames into a mapped ring. Does not own the mapping.
class FrameRingWriter {
 public:
  explicit FrameRingWriter(std::span<std::byte> mapping) noexcept;
  [[nodiscard]] auto valid() const noexcept -> bool;
  // True when a slot is free for writing. Always true for a three-slot ring
  // with a single writer, but the reader's in-flight count can make it false.
  [[nodiscard]] auto has_capacity() const noexcept -> bool;
  // Copies `bgra` into the next slot and publishes it. False when the mapping
  // is invalid or the payload does not match the canvas.
  [[nodiscard]] auto write(std::span<const std::byte> bgra, std::size_t row_stride,
                           std::uint64_t presentation_time_us) noexcept -> bool;

 private:
  FrameRingHeader* header_ = nullptr;
  std::byte* payload_ = nullptr;
};

// Reads the newest complete frame. Does not own the mapping.
class FrameRingReader {
 public:
  explicit FrameRingReader(std::span<const std::byte> mapping) noexcept;
  [[nodiscard]] auto valid() const noexcept -> bool;
  // Monotonic publish count, so a caller can tell a new frame from a repeat.
  [[nodiscard]] auto newest_sequence() const noexcept -> std::uint64_t;
  // Copies the newest complete frame into `out`. False when nothing has been
  // published, when `out` is too small, or when the frame tore under a
  // concurrent write and did not settle within a bounded number of retries.
  [[nodiscard]] auto read(std::span<std::byte> out, std::size_t out_stride,
                          std::uint64_t& presentation_time_us) const noexcept -> bool;

 private:
  const FrameRingHeader* header_ = nullptr;
  const std::byte* payload_ = nullptr;
};

}  // namespace noisefactor::sync::camera
```

- [ ] **Step 2: Write the failing test**

Create `native/test/camera/frame_ring_test.cpp`:

```cpp
#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <sync/camera/frame_ring.hpp>

namespace {

using noisefactor::sync::camera::FrameRingHeader;
using noisefactor::sync::camera::FrameRingReader;
using noisefactor::sync::camera::FrameRingWriter;
using noisefactor::sync::camera::frame_event_name;
using noisefactor::sync::camera::frame_ring_bytes;
using noisefactor::sync::camera::kCanvas;
using noisefactor::sync::camera::kFrameRingSlotBytes;
using noisefactor::sync::camera::section_name;

constexpr std::size_t kStride = static_cast<std::size_t>(kCanvas.width) * 4;

[[nodiscard]] auto canvas_filled(std::uint8_t value) -> std::vector<std::byte> {
  return std::vector<std::byte>(kFrameRingSlotBytes, static_cast<std::byte>(value));
}

SYNC_TEST(names_are_global_and_distinct) {
  const std::wstring section = section_name();
  SYNC_REQUIRE(section.rfind(L"Global\\", 0) == 0);
  const std::wstring event = frame_event_name();
  SYNC_REQUIRE(event.rfind(L"Global\\", 0) == 0);
  // The section and the event are separate kernel objects; sharing a name
  // would make CreateEventW fail against the existing section.
  SYNC_REQUIRE(event != section);
}

SYNC_TEST(a_fresh_ring_has_nothing_to_read) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  SYNC_REQUIRE(writer.valid());
  const FrameRingReader reader(mapping);
  SYNC_REQUIRE(reader.valid());
  SYNC_REQUIRE(reader.newest_sequence() == 0);
  std::vector<std::byte> out(kFrameRingSlotBytes);
  std::uint64_t presentation = 0;
  SYNC_REQUIRE(!reader.read(out, kStride, presentation));
}

SYNC_TEST(a_written_frame_reads_back_intact) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  const auto frame = canvas_filled(0x5A);
  SYNC_REQUIRE(writer.write(frame, kStride, 1234));
  const FrameRingReader reader(mapping);
  SYNC_REQUIRE(reader.newest_sequence() == 1);
  std::vector<std::byte> out(kFrameRingSlotBytes);
  std::uint64_t presentation = 0;
  SYNC_REQUIRE(reader.read(out, kStride, presentation));
  SYNC_REQUIRE(presentation == 1234);
  SYNC_REQUIRE(std::memcmp(out.data(), frame.data(), frame.size()) == 0);
}

SYNC_TEST(the_reader_always_sees_the_newest_frame) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  for (std::uint8_t value = 1; value <= 5; ++value) {
    SYNC_REQUIRE(writer.write(canvas_filled(value), kStride, value));
  }
  const FrameRingReader reader(mapping);
  SYNC_REQUIRE(reader.newest_sequence() == 5);
  std::vector<std::byte> out(kFrameRingSlotBytes);
  std::uint64_t presentation = 0;
  SYNC_REQUIRE(reader.read(out, kStride, presentation));
  SYNC_REQUIRE(presentation == 5);
  SYNC_REQUIRE(static_cast<std::uint8_t>(out[0]) == 5);
}

SYNC_TEST(a_reader_rejects_a_torn_slot) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  SYNC_REQUIRE(writer.write(canvas_filled(7), kStride, 7));
  // Forge a write in progress: an odd sequence on the newest slot.
  auto* header = reinterpret_cast<FrameRingHeader*>(mapping.data());
  header->slot[0].sequence.store(1, std::memory_order_release);
  const FrameRingReader reader(mapping);
  std::vector<std::byte> out(kFrameRingSlotBytes);
  std::uint64_t presentation = 0;
  SYNC_REQUIRE(!reader.read(out, kStride, presentation));
}

SYNC_TEST(a_reader_rejects_a_foreign_or_undersized_mapping) {
  std::vector<std::byte> too_small(64);
  const FrameRingReader small(too_small);
  SYNC_REQUIRE(!small.valid());
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  auto* header = reinterpret_cast<FrameRingHeader*>(mapping.data());
  header->magic = 0xDEADBEEF;
  const FrameRingReader foreign(mapping);
  SYNC_REQUIRE(!foreign.valid());
}

SYNC_TEST(the_writer_rejects_a_payload_that_is_not_the_canvas) {
  std::vector<std::byte> mapping(frame_ring_bytes());
  FrameRingWriter writer(mapping);
  std::vector<std::byte> short_frame(kFrameRingSlotBytes - 4);
  SYNC_REQUIRE(!writer.write(short_frame, kStride, 1));
  SYNC_REQUIRE(!writer.write(canvas_filled(1), kStride - 4, 1));
}

}  // namespace
```

- [ ] **Step 3: Run it and watch it fail**

```powershell
cmake --build build --config Release --target sync_camera_frame_ring_tests
```

Expected: FAIL — target does not exist.

- [ ] **Step 4: Implement the ring**

Create `native/src/camera/frame_ring.cpp`:

```cpp
#include <sync/camera/frame_ring.hpp>

#include <cstring>

namespace noisefactor::sync::camera {

namespace {

constexpr int kTornReadRetries = 4;

[[nodiscard]] auto mapping_is_ring(const void* data, std::size_t bytes) noexcept -> bool {
  if (data == nullptr || bytes < frame_ring_bytes()) return false;
  const auto* header = static_cast<const FrameRingHeader*>(data);
  return header->magic == kFrameRingMagic && header->version == kFrameRingVersion &&
         header->slots == kFrameRingSlots && header->slot_bytes == kFrameRingSlotBytes;
}

}  // namespace

auto section_name() -> std::wstring { return L"Global\\SyncCamera.frames"; }

auto frame_event_name() -> std::wstring { return L"Global\\SyncCamera.frame"; }

FrameRingWriter::FrameRingWriter(std::span<std::byte> mapping) noexcept {
  if (mapping.data() == nullptr || mapping.size() < frame_ring_bytes()) return;
  auto* header = reinterpret_cast<FrameRingHeader*>(mapping.data());
  // A writer initializes the ring when it finds it uninitialized, and adopts
  // it when the media source already stamped it. Either way the stamp is what
  // makes a reader trust the mapping.
  if (header->magic != kFrameRingMagic) {
    header->magic = kFrameRingMagic;
    header->version = kFrameRingVersion;
    header->slots = kFrameRingSlots;
    header->slot_bytes = static_cast<std::uint32_t>(kFrameRingSlotBytes);
    header->newest.store(0, std::memory_order_release);
    for (auto& slot : header->slot) {
      slot.sequence.store(0, std::memory_order_relaxed);
    }
  }
  if (!mapping_is_ring(mapping.data(), mapping.size())) return;
  header_ = header;
  payload_ = mapping.data() + sizeof(FrameRingHeader);
}

auto FrameRingWriter::valid() const noexcept -> bool { return header_ != nullptr; }

auto FrameRingWriter::has_capacity() const noexcept -> bool { return header_ != nullptr; }

auto FrameRingWriter::write(std::span<const std::byte> bgra, std::size_t row_stride,
                            std::uint64_t presentation_time_us) noexcept -> bool {
  if (header_ == nullptr) return false;
  if (row_stride != static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel) return false;
  if (bgra.size() < kFrameRingSlotBytes) return false;

  const std::uint64_t next = header_->newest.load(std::memory_order_acquire) + 1;
  const std::uint32_t index = static_cast<std::uint32_t>(next % kFrameRingSlots);
  FrameRingSlot& slot = header_->slot[index];

  // Odd while writing, even when settled: a reader that sees an odd sequence,
  // or a different one before and after, knows it read a torn frame.
  slot.sequence.store(next * 2 - 1, std::memory_order_release);
  std::memcpy(payload_ + static_cast<std::size_t>(index) * kFrameRingSlotBytes, bgra.data(),
              kFrameRingSlotBytes);
  slot.presentation_time_us = presentation_time_us;
  slot.width = kCanvas.width;
  slot.height = kCanvas.height;
  slot.row_stride = static_cast<std::uint32_t>(row_stride);
  slot.sequence.store(next * 2, std::memory_order_release);
  header_->newest.store(next, std::memory_order_release);
  return true;
}

FrameRingReader::FrameRingReader(std::span<const std::byte> mapping) noexcept {
  if (!mapping_is_ring(mapping.data(), mapping.size())) return;
  header_ = reinterpret_cast<const FrameRingHeader*>(mapping.data());
  payload_ = mapping.data() + sizeof(FrameRingHeader);
}

auto FrameRingReader::valid() const noexcept -> bool { return header_ != nullptr; }

auto FrameRingReader::newest_sequence() const noexcept -> std::uint64_t {
  return header_ == nullptr ? 0 : header_->newest.load(std::memory_order_acquire);
}

auto FrameRingReader::read(std::span<std::byte> out, std::size_t out_stride,
                           std::uint64_t& presentation_time_us) const noexcept -> bool {
  if (header_ == nullptr) return false;
  if (out_stride != static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel) return false;
  if (out.size() < kFrameRingSlotBytes) return false;

  for (int attempt = 0; attempt < kTornReadRetries; ++attempt) {
    const std::uint64_t newest = header_->newest.load(std::memory_order_acquire);
    if (newest == 0) return false;
    const std::uint32_t index = static_cast<std::uint32_t>(newest % kFrameRingSlots);
    const FrameRingSlot& slot = header_->slot[index];
    const std::uint64_t before = slot.sequence.load(std::memory_order_acquire);
    if ((before & 1U) != 0) continue;  // a write is in progress
    std::memcpy(out.data(), payload_ + static_cast<std::size_t>(index) * kFrameRingSlotBytes,
                kFrameRingSlotBytes);
    const std::uint64_t presentation = slot.presentation_time_us;
    if (slot.sequence.load(std::memory_order_acquire) != before) continue;  // torn
    presentation_time_us = presentation;
    return true;
  }
  return false;
}

}  // namespace noisefactor::sync::camera
```

- [ ] **Step 5: Wire the target**

Above `if(APPLE)` in `CMakeLists.txt`:

```cmake
add_library(sync_camera_frame_ring
  native/src/camera/frame_ring.cpp
)
target_compile_features(sync_camera_frame_ring PUBLIC cxx_std_20)
target_include_directories(sync_camera_frame_ring PUBLIC native/include)

add_executable(sync_camera_frame_ring_tests
  native/test/camera/frame_ring_test.cpp
  native/test/test_main.cpp
)
target_compile_features(sync_camera_frame_ring_tests PRIVATE cxx_std_20)
target_include_directories(sync_camera_frame_ring_tests PRIVATE native/test)
target_link_libraries(sync_camera_frame_ring_tests PRIVATE sync_camera_frame_ring)
add_test(NAME sync_camera_frame_ring_tests COMMAND sync_camera_frame_ring_tests)
```

- [ ] **Step 6: Run the tests**

```powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure -R frame_ring
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat(camera): add the shared frame ring both halves agree on"
```

---

### Task 7: The Windows idle card

What the camera shows while no sender is live, so a consumer that opened the camera early sees Sync rather than a black picture that reads as a broken device. Drawn with Direct2D/DirectWrite in place of CoreGraphics.

**Files:**
- Create: `native/src/platform/windows/camera_idle_card.cpp`, `native/test/windows/camera_idle_card_test.cpp`
- Modify: `native/include/sync/platform/camera_idle_card.hpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `CameraCanvas`
- Produces: `draw_camera_idle_card(std::span<std::byte> bgra, std::size_t canvas_stride, CameraCanvas canvas) -> bool` on Windows. Task 8 calls it.

- [ ] **Step 1: Drop the platform guard from the header**

In `native/include/sync/platform/camera_idle_card.hpp`, delete:

```cpp
#if !defined(__APPLE__)
#error "camera_idle_card.hpp is available only on Apple platforms"
#endif
```

and change "or when CoreGraphics cannot draw into it" in the doc comment to "or when the platform's 2D renderer cannot draw into it".

- [ ] **Step 2: Write the failing test**

Create `native/test/windows/camera_idle_card_test.cpp`:

```cpp
#include "test_harness.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <sync/platform/camera_identity.hpp>
#include <sync/platform/camera_idle_card.hpp>

namespace {

using noisefactor::sync::camera::draw_camera_idle_card;
using noisefactor::sync::camera::kBytesPerPixel;
using noisefactor::sync::camera::kCanvas;

constexpr std::size_t kStride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;

SYNC_TEST(the_card_fills_the_canvas_opaquely) {
  std::vector<std::byte> canvas(kStride * kCanvas.height, static_cast<std::byte>(0x11));
  SYNC_REQUIRE(draw_camera_idle_card(canvas, kStride, kCanvas));
  for (std::uint32_t row = 0; row < kCanvas.height; row += 97) {
    for (std::uint32_t column = 3; column < kCanvas.width * kBytesPerPixel; column += 4 * 61) {
      SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[row * kStride + column]) == 255);
    }
  }
}

SYNC_TEST(the_card_is_dark_but_not_pure_black) {
  std::vector<std::byte> canvas(kStride * kCanvas.height);
  SYNC_REQUIRE(draw_camera_idle_card(canvas, kStride, kCanvas));
  // The corner is background: dark, and deliberately not 0 so a viewer can
  // tell a drawn card from a dead signal.
  const auto blue = static_cast<std::uint8_t>(canvas[0]);
  SYNC_REQUIRE(blue > 0 && blue < 64);
}

SYNC_TEST(the_card_draws_something_lighter_than_its_background) {
  std::vector<std::byte> canvas(kStride * kCanvas.height);
  SYNC_REQUIRE(draw_camera_idle_card(canvas, kStride, kCanvas));
  const auto background = static_cast<std::uint8_t>(canvas[0]);
  std::uint8_t brightest = 0;
  for (std::size_t i = 0; i < canvas.size(); i += 4) {
    brightest = std::max(brightest, static_cast<std::uint8_t>(canvas[i]));
  }
  SYNC_REQUIRE(brightest > background + 64);
}

SYNC_TEST(the_card_refuses_a_buffer_that_cannot_hold_the_canvas) {
  std::vector<std::byte> canvas(kStride * (kCanvas.height - 1), static_cast<std::byte>(0x22));
  SYNC_REQUIRE(!draw_camera_idle_card(canvas, kStride, kCanvas));
  SYNC_REQUIRE(static_cast<std::uint8_t>(canvas[0]) == 0x22);  // left untouched
}

}  // namespace
```

- [ ] **Step 3: Run it and watch it fail**

```powershell
cmake --build build --config Release --target sync_camera_idle_card_tests
```

Expected: FAIL — target does not exist.

- [ ] **Step 4: Implement the card**

Create `native/src/platform/windows/camera_idle_card.cpp`. It renders through a WIC bitmap so the result is a plain BGRA buffer with no window and no device dependency — that is what makes it testable in CI and usable inside the Frame Server.

```cpp
#include <sync/platform/camera_idle_card.hpp>

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>

namespace noisefactor::sync::camera {

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kMessage[] = L"Sync: waiting for Noisedeck";
// Deliberately not pure black: a viewer must be able to tell a drawn card
// from a dead signal, and 0x14 reads as "off" without reading as "broken".
constexpr float kBackground = 0x14 / 255.0F;

void paint_opaque_black(std::span<std::byte> bgra, std::size_t canvas_stride,
                        CameraCanvas canvas) noexcept {
  for (std::uint32_t row = 0; row < canvas.height; ++row) {
    std::byte* out = bgra.data() + static_cast<std::size_t>(row) * canvas_stride;
    for (std::uint32_t column = 0; column < canvas.width; ++column) {
      out[0] = std::byte{0};
      out[1] = std::byte{0};
      out[2] = std::byte{0};
      out[3] = std::byte{255};
      out += kBytesPerPixel;
    }
  }
}

}  // namespace

auto draw_camera_idle_card(std::span<std::byte> bgra, std::size_t canvas_stride,
                           CameraCanvas canvas) noexcept -> bool {
  if (canvas_stride < static_cast<std::size_t>(canvas.width) * kBytesPerPixel) return false;
  if (bgra.size() < canvas_stride * canvas.height) return false;

  // Any failure past this point leaves an opaque black canvas rather than
  // whatever the caller's buffer happened to hold.
  const auto fail_black = [&]() noexcept -> bool {
    paint_opaque_black(bgra, canvas_stride, canvas);
    return false;
  };

  ComPtr<IWICImagingFactory> wic;
  if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) {
    return fail_black();
  }
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(canvas.width, canvas.height, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnLoad, &bitmap))) {
    return fail_black();
  }

  ComPtr<ID2D1Factory> d2d;
  if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) {
    return fail_black();
  }
  ComPtr<ID2D1RenderTarget> target;
  const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target))) {
    return fail_black();
  }

  ComPtr<IDWriteFactory> dwrite;
  if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) {
    return fail_black();
  }
  ComPtr<IDWriteTextFormat> format;
  if (FAILED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      canvas.height / 18.0F, L"en-us", &format))) {
    return fail_black();
  }
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  ComPtr<ID2D1SolidColorBrush> brush;
  target->BeginDraw();
  target->Clear(D2D1::ColorF(kBackground, kBackground, kBackground, 1.0F));
  if (SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0.78F, 0.80F, 0.84F, 1.0F), &brush))) {
    const D2D1_RECT_F box = D2D1::RectF(0.0F, 0.0F, static_cast<float>(canvas.width),
                                        static_cast<float>(canvas.height));
    target->DrawTextW(kMessage, static_cast<UINT32>(std::size(kMessage) - 1), format.Get(), box,
                      brush.Get());
  }
  if (FAILED(target->EndDraw())) return fail_black();

  ComPtr<IWICBitmapLock> lock;
  const WICRect whole{0, 0, static_cast<INT>(canvas.width), static_cast<INT>(canvas.height)};
  if (FAILED(bitmap->Lock(&whole, WICBitmapLockRead, &lock))) return fail_black();
  UINT source_stride = 0;
  UINT source_bytes = 0;
  BYTE* source = nullptr;
  if (FAILED(lock->GetStride(&source_stride)) ||
      FAILED(lock->GetDataPointer(&source_bytes, &source)) || source == nullptr) {
    return fail_black();
  }
  for (std::uint32_t row = 0; row < canvas.height; ++row) {
    std::byte* out = bgra.data() + static_cast<std::size_t>(row) * canvas_stride;
    std::memcpy(out, source + static_cast<std::size_t>(row) * source_stride,
                static_cast<std::size_t>(canvas.width) * kBytesPerPixel);
    // The render target is premultiplied with an opaque clear, so alpha is
    // already 255; force it anyway because a camera has no alpha to offer.
    for (std::uint32_t column = 0; column < canvas.width; ++column) {
      out[static_cast<std::size_t>(column) * kBytesPerPixel + 3] = std::byte{255};
    }
  }
  return true;
}

}  // namespace noisefactor::sync::camera
```

- [ ] **Step 5: Wire the target**

Inside `elseif(WIN32)` in `CMakeLists.txt`:

```cmake
  add_library(sync_camera_idle_card
    native/src/platform/windows/camera_idle_card.cpp
  )
  target_compile_features(sync_camera_idle_card PUBLIC cxx_std_20)
  target_include_directories(sync_camera_idle_card PUBLIC native/include)
  target_link_libraries(sync_camera_idle_card PUBLIC d2d1 dwrite windowscodecs ole32)

  add_executable(sync_camera_idle_card_tests
    native/test/windows/camera_idle_card_test.cpp
    native/test/test_main.cpp
  )
  target_compile_features(sync_camera_idle_card_tests PRIVATE cxx_std_20)
  target_include_directories(sync_camera_idle_card_tests PRIVATE native/test)
  target_link_libraries(sync_camera_idle_card_tests PRIVATE sync_camera_idle_card)
  add_test(NAME sync_camera_idle_card_tests COMMAND sync_camera_idle_card_tests)
```

The test binary must initialize COM before calling the card. Add to `native/test/windows/camera_idle_card_test.cpp` above the first `SYNC_TEST`:

```cpp
struct ComScope {
  ComScope() { ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
  ~ComScope() { ::CoUninitialize(); }
};
ComScope com_scope;
```

with `#include <windows.h>` and `#include <objbase.h>` at the top.

- [ ] **Step 6: Run the tests**

```powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure -R idle_card
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat(camera): draw the waiting card on Windows"
```

---

### Task 8: The camera media source DLL

The heart of the feature. A COM in-proc server exposing `IMFMediaSource` with one stream that advertises NV12 then RGB32, reads the shared ring, and falls back to the idle card through `CameraRelayPolicy`. Tested in-process, without the Frame Server.

**Files:**
- Create: `native/src/platform/windows/camera_source/source_guids.hpp`, `native/src/platform/windows/camera_source/section_owner.hpp`, `native/src/platform/windows/camera_source/section_owner.cpp`, `native/src/platform/windows/camera_source/media_stream.hpp`, `native/src/platform/windows/camera_source/media_stream.cpp`, `native/src/platform/windows/camera_source/media_source.hpp`, `native/src/platform/windows/camera_source/media_source.cpp`, `native/src/platform/windows/camera_source/dll_main.cpp`, `native/src/platform/windows/camera_source/SyncCamera.def`, `native/test/windows/camera_source_inproc_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `FrameRingReader`, `bgra_to_nv12`, `draw_camera_idle_card`, `CameraRelayPolicy`, `kCanvas`
- Produces: `SyncCamera.dll` exporting `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer`, `DllUnregisterServer`; CLSID `kSyncCameraSourceClsid` in `source_guids.hpp`. Tasks 9, 12, and 14 use the CLSID.

- [ ] **Step 1: Define the shared GUIDs**

Create `native/src/platform/windows/camera_source/source_guids.hpp`:

```cpp
#pragma once

#include <guiddef.h>

// The media source's CLSID. syncd passes this to MFCreateVirtualCamera and the
// installer registers it under HKLM; all three must agree, so it lives here
// and nowhere else.
// {2F8E7B14-9C3D-4A62-B5E1-7D4A9F2C6B08}
inline constexpr GUID kSyncCameraSourceClsid = {
    0x2f8e7b14, 0x9c3d, 0x4a62, {0xb5, 0xe1, 0x7d, 0x4a, 0x9f, 0x2c, 0x6b, 0x08}};

inline constexpr wchar_t kSyncCameraSourceClsidString[] =
    L"{2F8E7B14-9C3D-4A62-B5E1-7D4A9F2C6B08}";
inline constexpr wchar_t kSyncCameraSourceFriendlyName[] = L"Sync Camera Source";
// Media Foundation appends "Windows Virtual Camera" to whatever friendly name
// the camera is created with, so this is deliberately just the product name.
inline constexpr wchar_t kSyncCameraDisplayName[] = L"Sync";
```

- [ ] **Step 2: Write the failing in-process test**

Create `native/test/windows/camera_source_inproc_test.cpp`. This is the Tier 1 workhorse: it loads the DLL, instantiates the media source directly, and drives it without the Frame Server.

```cpp
#include "test_harness.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <objbase.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include <sync/camera/frame_ring.hpp>
#include <sync/platform/camera_identity.hpp>

#include "../../src/platform/windows/camera_source/source_guids.hpp"

namespace {

using Microsoft::WRL::ComPtr;
using noisefactor::sync::camera::FrameRingWriter;
using noisefactor::sync::camera::frame_ring_bytes;
using noisefactor::sync::camera::kBytesPerPixel;
using noisefactor::sync::camera::kCanvas;
using noisefactor::sync::camera::kFrameRingSlotBytes;

constexpr std::size_t kStride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;

using DllGetClassObjectFn = HRESULT(__stdcall*)(REFCLSID, REFIID, void**);

struct Environment {
  HMODULE module = nullptr;
  Environment() {
    ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    module = ::LoadLibraryW(L"SyncCamera.dll");
  }
  ~Environment() {
    if (module != nullptr) ::FreeLibrary(module);
    ::MFShutdown();
    ::CoUninitialize();
  }
};

Environment environment;

[[nodiscard]] auto create_source() -> ComPtr<IMFMediaSource> {
  SYNC_REQUIRE(environment.module != nullptr);
  auto entry = reinterpret_cast<DllGetClassObjectFn>(
      ::GetProcAddress(environment.module, "DllGetClassObject"));
  SYNC_REQUIRE(entry != nullptr);
  ComPtr<IClassFactory> factory;
  SYNC_REQUIRE(SUCCEEDED(entry(kSyncCameraSourceClsid, IID_PPV_ARGS(&factory))));
  ComPtr<IMFMediaSource> source;
  SYNC_REQUIRE(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&source))));
  return source;
}

SYNC_TEST(the_source_advertises_nv12_first_then_rgb32) {
  auto source = create_source();
  ComPtr<IMFPresentationDescriptor> descriptor;
  SYNC_REQUIRE(SUCCEEDED(source->CreatePresentationDescriptor(&descriptor)));
  DWORD streams = 0;
  SYNC_REQUIRE(SUCCEEDED(descriptor->GetStreamDescriptorCount(&streams)));
  SYNC_REQUIRE(streams == 1);

  BOOL selected = FALSE;
  ComPtr<IMFStreamDescriptor> stream;
  SYNC_REQUIRE(SUCCEEDED(descriptor->GetStreamDescriptorByIndex(0, &selected, &stream)));
  ComPtr<IMFMediaTypeHandler> handler;
  SYNC_REQUIRE(SUCCEEDED(stream->GetMediaTypeHandler(&handler)));
  DWORD types = 0;
  SYNC_REQUIRE(SUCCEEDED(handler->GetMediaTypeCount(&types)));
  SYNC_REQUIRE(types == 2);

  const GUID expected[2] = {MFVideoFormat_NV12, MFVideoFormat_RGB32};
  for (DWORD index = 0; index < types; ++index) {
    ComPtr<IMFMediaType> type;
    SYNC_REQUIRE(SUCCEEDED(handler->GetMediaTypeByIndex(index, &type)));
    GUID subtype{};
    SYNC_REQUIRE(SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)));
    SYNC_REQUIRE(::IsEqualGUID(subtype, expected[index]));
    UINT32 width = 0, height = 0;
    SYNC_REQUIRE(SUCCEEDED(::MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)));
    SYNC_REQUIRE(width == kCanvas.width && height == kCanvas.height);
  }
}

SYNC_TEST(the_source_reports_a_sixty_fps_frame_rate) {
  auto source = create_source();
  ComPtr<IMFPresentationDescriptor> descriptor;
  SYNC_REQUIRE(SUCCEEDED(source->CreatePresentationDescriptor(&descriptor)));
  BOOL selected = FALSE;
  ComPtr<IMFStreamDescriptor> stream;
  SYNC_REQUIRE(SUCCEEDED(descriptor->GetStreamDescriptorByIndex(0, &selected, &stream)));
  ComPtr<IMFMediaTypeHandler> handler;
  SYNC_REQUIRE(SUCCEEDED(stream->GetMediaTypeHandler(&handler)));
  ComPtr<IMFMediaType> type;
  SYNC_REQUIRE(SUCCEEDED(handler->GetMediaTypeByIndex(0, &type)));
  UINT32 numerator = 0, denominator = 0;
  SYNC_REQUIRE(SUCCEEDED(::MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &numerator,
                                               &denominator)));
  SYNC_REQUIRE(denominator != 0 && numerator / denominator == 60);
}

SYNC_TEST(the_source_starts_and_stops_cleanly) {
  auto source = create_source();
  ComPtr<IMFPresentationDescriptor> descriptor;
  SYNC_REQUIRE(SUCCEEDED(source->CreatePresentationDescriptor(&descriptor)));
  PROPVARIANT start{};
  ::PropVariantInit(&start);
  start.vt = VT_EMPTY;
  SYNC_REQUIRE(SUCCEEDED(source->Start(descriptor.Get(), nullptr, &start)));
  SYNC_REQUIRE(SUCCEEDED(source->Stop()));
  SYNC_REQUIRE(SUCCEEDED(source->Shutdown()));
  ::PropVariantClear(&start);
}

SYNC_TEST(a_shut_down_source_refuses_further_calls) {
  auto source = create_source();
  SYNC_REQUIRE(SUCCEEDED(source->Shutdown()));
  ComPtr<IMFPresentationDescriptor> descriptor;
  SYNC_REQUIRE(source->CreatePresentationDescriptor(&descriptor) == MF_E_SHUTDOWN);
}

}  // namespace
```

- [ ] **Step 3: Run it and watch it fail**

```powershell
cmake --build build --config Release --target sync_camera_source_tests
```

Expected: FAIL — `SyncCamera.dll` does not exist.

- [ ] **Step 4: Implement the section owner**

Create `native/src/platform/windows/camera_source/section_owner.hpp`:

```cpp
#pragma once

#include <windows.h>

#include <cstddef>
#include <span>
#include <string>

namespace noisefactor::sync::camera {

// Creates the shared ring in session 0, where the Frame Server has the
// privilege to make Global objects that syncd (unelevated, session 1) does
// not. The DACL grants INTERACTIVE write access and both application package
// SIDs read access, so the logged-in user's syncd can feed it and AppContainer
// and LPAC consumers can read from it.
class SectionOwner {
 public:
  SectionOwner() = default;
  ~SectionOwner();

  SectionOwner(const SectionOwner&) = delete;
  auto operator=(const SectionOwner&) -> SectionOwner& = delete;

  // Idempotent: opens the section when it already exists.
  [[nodiscard]] auto open() noexcept -> bool;
  [[nodiscard]] auto mapping() const noexcept -> std::span<std::byte>;
  [[nodiscard]] auto frame_event() const noexcept -> HANDLE { return event_; }

 private:
  HANDLE section_ = nullptr;
  HANDLE event_ = nullptr;
  void* view_ = nullptr;
  std::size_t bytes_ = 0;
};

}  // namespace noisefactor::sync::camera
```

Create `native/src/platform/windows/camera_source/section_owner.cpp`:

```cpp
#include "section_owner.hpp"

#include <sddl.h>

#include <sync/camera/frame_ring.hpp>

namespace noisefactor::sync::camera {

namespace {

// DACL, in SDDL:
//   GA to SYSTEM (SY) and Local Service (LS) -- the Frame Server itself
//   GA to INTERACTIVE (IU) -- the logged-in user's syncd writes frames
//   GR+GX to ALL APPLICATION PACKAGES (AC) and ALL RESTRICTED APPLICATION
//     PACKAGES (RC) -- Store apps and LPAC consumers read frames
//
// INTERACTIVE rather than a specific SID because the media source cannot be
// told which user to pair with: IMFVirtualCamera's AddProperty and
// AddRegistryEntry both demand administrator permissions that unelevated
// syncd does not have.
constexpr wchar_t kSectionSddl[] =
    L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;IU)(A;;GRGX;;;AC)(A;;GRGX;;;RC)";

}  // namespace

SectionOwner::~SectionOwner() {
  if (view_ != nullptr) ::UnmapViewOfFile(view_);
  if (section_ != nullptr) ::CloseHandle(section_);
  if (event_ != nullptr) ::CloseHandle(event_);
}

auto SectionOwner::open() noexcept -> bool {
  if (view_ != nullptr) return true;

  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kSectionSddl, SDDL_REVISION_1, &descriptor, nullptr) == FALSE) {
    return false;
  }
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = descriptor;
  attributes.bInheritHandle = FALSE;

  const std::wstring section = section_name();
  const std::wstring event = frame_event_name();
  bytes_ = frame_ring_bytes();
  section_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0,
                                  static_cast<DWORD>(bytes_), section.c_str());
  if (section_ != nullptr) {
    event_ = ::CreateEventW(&attributes, FALSE, FALSE, event.c_str());
    view_ = ::MapViewOfFile(section_, FILE_MAP_ALL_ACCESS, 0, 0, bytes_);
  }
  ::LocalFree(descriptor);
  if (view_ == nullptr) {
    if (section_ != nullptr) {
      ::CloseHandle(section_);
      section_ = nullptr;
    }
    if (event_ != nullptr) {
      ::CloseHandle(event_);
      event_ = nullptr;
    }
    return false;
  }
  return true;
}

auto SectionOwner::mapping() const noexcept -> std::span<std::byte> {
  if (view_ == nullptr) return {};
  return {static_cast<std::byte*>(view_), bytes_};
}

}  // namespace noisefactor::sync::camera
```

- [ ] **Step 5: Implement the media stream and source**

Create `native/src/platform/windows/camera_source/media_stream.hpp` and `.cpp`, and `media_source.hpp` and `.cpp`, implementing `IMFMediaStream` and `IMFMediaSource` over `IMFMediaEventQueue`.

The stream's `RequestSample` does, in order:

1. `FrameRingReader::read` into a canvas-sized BGRA scratch. On success call `policy.client_frame_arrived(now_ns)`.
2. On failure, or when `CameraRelayPolicy::tick(now_ns)` returns `EmitBlack`, use the idle card drawn once at Start and cached.
3. Convert the BGRA scratch into the negotiated subtype: `bgra_to_nv12` for `MFVideoFormat_NV12`, a straight row copy for `MFVideoFormat_RGB32`.
4. Wrap in an `IMFSample` via `MFCreateMemoryBuffer` + `MFCreateSample`, set `SetSampleTime` from a monotonic clock and `SetSampleDuration` to the negotiated frame duration, and queue `MEMediaSample`.

The source implements `GetCharacteristics` returning `MFMEDIASOURCE_IS_LIVE`, `CreatePresentationDescriptor`, `Start`/`Stop`/`Pause`/`Shutdown`, and forwards events through its own `IMFMediaEventQueue`. Every public method returns `MF_E_SHUTDOWN` after `Shutdown`, guarded by a `std::mutex` and a `bool shutdown_` — the test `a_shut_down_source_refuses_further_calls` covers exactly this.

Media types are built once, in order, by a helper:

```cpp
[[nodiscard]] auto make_type(const GUID& subtype) -> ComPtr<IMFMediaType> {
  ComPtr<IMFMediaType> type;
  if (FAILED(::MFCreateMediaType(&type))) return nullptr;
  type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  type->SetGUID(MF_MT_SUBTYPE, subtype);
  type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  ::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, kCanvas.width, kCanvas.height);
  ::MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, kMaximumFramesPerSecond, 1);
  ::MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  const UINT32 stride = subtype == MFVideoFormat_NV12
                            ? kCanvas.width
                            : kCanvas.width * kBytesPerPixel;
  type->SetUINT32(MF_MT_DEFAULT_STRIDE, stride);
  return type;
}
```

and the array is `{make_type(MFVideoFormat_NV12), make_type(MFVideoFormat_RGB32)}` — NV12 first, per the global constraints.

- [ ] **Step 6: Implement the DLL entry points**

Create `native/src/platform/windows/camera_source/dll_main.cpp` with `DllMain`, a class factory for `kSyncCameraSourceClsid`, `DllGetClassObject`, `DllCanUnloadNow`, and `DllRegisterServer`/`DllUnregisterServer` (Task 9 tests the last two). Create `SyncCamera.def`:

```
LIBRARY SyncCamera
EXPORTS
    DllGetClassObject   PRIVATE
    DllCanUnloadNow     PRIVATE
    DllRegisterServer   PRIVATE
    DllUnregisterServer PRIVATE
```

- [ ] **Step 7: Wire the targets**

Inside `elseif(WIN32)`:

```cmake
  add_library(sync_camera_source SHARED
    native/src/platform/windows/camera_source/dll_main.cpp
    native/src/platform/windows/camera_source/media_source.cpp
    native/src/platform/windows/camera_source/media_stream.cpp
    native/src/platform/windows/camera_source/section_owner.cpp
    native/src/platform/windows/camera_source/SyncCamera.def
  )
  set_target_properties(sync_camera_source PROPERTIES OUTPUT_NAME SyncCamera)
  target_compile_features(sync_camera_source PRIVATE cxx_std_20)
  target_include_directories(sync_camera_source PRIVATE native/include)
  target_link_libraries(sync_camera_source PRIVATE
    sync_camera_frame_ring sync_camera_nv12 sync_camera_idle_card sync_camera_relay_policy
    mfplat mfuuid mf ole32 oleaut32 advapi32)

  add_executable(sync_camera_source_tests
    native/test/windows/camera_source_inproc_test.cpp
    native/test/test_main.cpp
  )
  target_compile_features(sync_camera_source_tests PRIVATE cxx_std_20)
  target_include_directories(sync_camera_source_tests PRIVATE native/test native/include)
  target_link_libraries(sync_camera_source_tests PRIVATE
    sync_camera_frame_ring mfplat mfuuid ole32)
  add_dependencies(sync_camera_source_tests sync_camera_source)
  # The test loads the DLL by name, so it must sit beside the test binary.
  add_custom_command(TARGET sync_camera_source_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:sync_camera_source> $<TARGET_FILE_DIR:sync_camera_source_tests>)
  add_test(NAME sync_camera_source_tests COMMAND sync_camera_source_tests)
```

- [ ] **Step 8: Run the tests**

```powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure -R camera_source
```

Expected: PASS, all four tests.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "feat(camera): add the Windows camera media source"
```

---

### Task 9: Self-registration, proven against a redirected hive

`DllRegisterServer` must write the CLSID where the Frame Server can find it. Testing that without touching the machine's real HKLM is what `RegOverridePredefKey` is for.

**Files:**
- Create: `native/test/windows/camera_registration_test.cpp`
- Modify: `native/src/platform/windows/camera_source/dll_main.cpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `kSyncCameraSourceClsidString`
- Produces: `DllRegisterServer` writing `HKLM\SOFTWARE\Classes\CLSID\<clsid>\InprocServer32` with the DLL's own path and `ThreadingModel=Both`; `DllUnregisterServer` removing it

- [ ] **Step 1: Write the failing test**

Create `native/test/windows/camera_registration_test.cpp`:

```cpp
#include "test_harness.hpp"

#include <windows.h>

#include <string>

#include "../../src/platform/windows/camera_source/source_guids.hpp"

namespace {

using RegistrationFn = HRESULT(__stdcall*)();

// Redirects HKLM into a scratch key under HKCU for the life of the scope, so
// registration is exercised end to end without an administrator and without
// touching the machine.
struct RedirectedHklm {
  HKEY scratch = nullptr;
  RedirectedHklm() {
    ::RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\SyncCameraRegistrationTest", 0, nullptr,
                      REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &scratch, nullptr);
    ::RegOverridePredefKey(HKEY_LOCAL_MACHINE, scratch);
  }
  ~RedirectedHklm() {
    ::RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
    if (scratch != nullptr) ::RegCloseKey(scratch);
    ::RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\SyncCameraRegistrationTest");
  }
};

[[nodiscard]] auto inproc_path() -> std::wstring {
  HKEY key = nullptr;
  const std::wstring path = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") +
                            kSyncCameraSourceClsidString + L"\\InprocServer32";
  if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return {};
  }
  wchar_t value[MAX_PATH]{};
  DWORD bytes = sizeof(value);
  const LSTATUS status = ::RegQueryValueExW(key, nullptr, nullptr, nullptr,
                                            reinterpret_cast<LPBYTE>(value), &bytes);
  ::RegCloseKey(key);
  return status == ERROR_SUCCESS ? std::wstring(value) : std::wstring{};
}

SYNC_TEST(register_then_unregister_round_trips) {
  RedirectedHklm redirect;
  HMODULE module = ::LoadLibraryW(L"SyncCamera.dll");
  SYNC_REQUIRE(module != nullptr);
  auto register_server =
      reinterpret_cast<RegistrationFn>(::GetProcAddress(module, "DllRegisterServer"));
  auto unregister_server =
      reinterpret_cast<RegistrationFn>(::GetProcAddress(module, "DllUnregisterServer"));
  SYNC_REQUIRE(register_server != nullptr && unregister_server != nullptr);

  SYNC_REQUIRE(inproc_path().empty());
  SYNC_REQUIRE(SUCCEEDED(register_server()));
  const std::wstring registered = inproc_path();
  SYNC_REQUIRE(registered.find(L"SyncCamera.dll") != std::wstring::npos);

  SYNC_REQUIRE(SUCCEEDED(unregister_server()));
  SYNC_REQUIRE(inproc_path().empty());
  ::FreeLibrary(module);
}

SYNC_TEST(registration_is_idempotent) {
  RedirectedHklm redirect;
  HMODULE module = ::LoadLibraryW(L"SyncCamera.dll");
  SYNC_REQUIRE(module != nullptr);
  auto register_server =
      reinterpret_cast<RegistrationFn>(::GetProcAddress(module, "DllRegisterServer"));
  SYNC_REQUIRE(SUCCEEDED(register_server()));
  SYNC_REQUIRE(SUCCEEDED(register_server()));
  SYNC_REQUIRE(!inproc_path().empty());
  ::FreeLibrary(module);
}

SYNC_TEST(unregistering_what_was_never_registered_succeeds) {
  RedirectedHklm redirect;
  HMODULE module = ::LoadLibraryW(L"SyncCamera.dll");
  SYNC_REQUIRE(module != nullptr);
  auto unregister_server =
      reinterpret_cast<RegistrationFn>(::GetProcAddress(module, "DllUnregisterServer"));
  // Uninstall must not fail because a previous uninstall already cleaned up.
  SYNC_REQUIRE(SUCCEEDED(unregister_server()));
  ::FreeLibrary(module);
}

}  // namespace
```

- [ ] **Step 2: Run it and watch it fail**

```powershell
cmake --build build --config Release --target sync_camera_registration_tests
```

Expected: FAIL.

- [ ] **Step 3: Implement registration in `dll_main.cpp`**

`DllRegisterServer` resolves its own path with `GetModuleFileNameW(module_handle_from_DllMain, …)`, then writes `SOFTWARE\Classes\CLSID\{clsid}` (default value `kSyncCameraSourceFriendlyName`) and `…\InprocServer32` (default value the DLL path, `ThreadingModel` = `Both`). `DllUnregisterServer` calls `RegDeleteTreeW` on the CLSID key and returns `S_OK` when the key is already absent.

- [ ] **Step 4: Wire the target**

```cmake
  add_executable(sync_camera_registration_tests
    native/test/windows/camera_registration_test.cpp
    native/test/test_main.cpp
  )
  target_compile_features(sync_camera_registration_tests PRIVATE cxx_std_20)
  target_include_directories(sync_camera_registration_tests PRIVATE native/test native/include)
  target_link_libraries(sync_camera_registration_tests PRIVATE advapi32)
  add_dependencies(sync_camera_registration_tests sync_camera_source)
  add_custom_command(TARGET sync_camera_registration_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:sync_camera_source> $<TARGET_FILE_DIR:sync_camera_registration_tests>)
  add_test(NAME sync_camera_registration_tests COMMAND sync_camera_registration_tests)
```

- [ ] **Step 5: Run the tests**

```powershell
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure -R registration
```

Expected: PASS. Confirm afterwards that the real hive was untouched:

```powershell
Test-Path "HKLM:\SOFTWARE\Classes\CLSID\{2F8E7B14-9C3D-4A62-B5E1-7D4A9F2C6B08}"
```

Expected: `False`.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(camera): register the media source under HKLM"
```

---

### Task 10: `MfCameraSink` in syncd

The `CameraSink` implementation that opens the ring and writes into it. Mirror of `CmioCameraSink`.

**Files:**
- Create: `native/include/sync/platform/mf_camera_sink.hpp`, `native/src/platform/windows/mf_camera_sink.cpp`, `native/test/windows/mf_camera_sink_test.cpp`
- Modify: `native/include/sync/platform/camera_sink.hpp`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `FrameRingWriter`, `section_name`, `frame_event_name`, `CameraSink`
- Produces: `MfCameraSink` with the `CameraSink` interface. Default-constructible; it opens the one well-known section. Task 11 constructs it.

- [ ] **Step 1: Add the Windows failure reasons**

In `native/include/sync/platform/camera_sink.hpp`, extend the enum:

```cpp
enum class CameraSinkUnavailableReason : std::uint8_t {
  None = 0,
  DeviceNotFound,
  SinkStreamMissing,
  QueueNotProvided,
  StreamNotStarted,
  // Windows. The camera provider is a Media Foundation virtual camera there,
  // and these are the ways it fails to become one.
  NotSupported,
  SourceNotRegistered,
  VirtualCameraRefused,
  SectionMissing,
  SectionAccessDenied,
};
```

and make `describe` platform-aware — the existing five strings all name a macOS system extension:

```cpp
[[nodiscard]] constexpr auto describe(CameraSinkUnavailableReason reason) noexcept -> const char* {
  switch (reason) {
    case CameraSinkUnavailableReason::None:
      return "no error";
    case CameraSinkUnavailableReason::DeviceNotFound:
      return "the Sync Camera extension is not installed, or has not been approved in System Settings";
    case CameraSinkUnavailableReason::SinkStreamMissing:
      return "the Sync Camera extension is a different version than this Sync";
    case CameraSinkUnavailableReason::QueueNotProvided:
      return "the Sync Camera extension did not provide its sink queue";
    case CameraSinkUnavailableReason::StreamNotStarted:
      return "the Sync Camera extension did not start its sink stream";
    case CameraSinkUnavailableReason::NotSupported:
      return "the camera needs Windows 11 (build 22000) or later";
    case CameraSinkUnavailableReason::SourceNotRegistered:
      return "the Sync Camera source is not registered; choose Enable Sync Camera from the Sync tray menu";
    case CameraSinkUnavailableReason::VirtualCameraRefused:
      return "Windows refused to create the Sync camera; camera privacy settings may be denying access";
    case CameraSinkUnavailableReason::SectionMissing:
      return "the Sync Camera source has not been started by a camera app yet";
    case CameraSinkUnavailableReason::SectionAccessDenied:
      return "the Sync Camera source refused this account access to its frame buffer";
  }
  return "unknown camera problem";
}
```

Also generalise `describe_unavailability`, whose text hardcodes `OSStatus`:

```cpp
[[nodiscard]] inline auto describe_unavailability(CameraSinkUnavailableReason reason,
                                                  std::int32_t status) -> std::string {
  std::string text = describe(reason);
  if (status != 0) {
#if defined(_WIN32)
    text += " (HRESULT 0x";
    char buffer[9]{};
    std::snprintf(buffer, sizeof(buffer), "%08X", static_cast<unsigned>(status));
    text += buffer;
    text += ')';
#else
    text += " (OSStatus ";
    text += std::to_string(status);
    text += ')';
#endif
  }
  return text;
}
```

Add `#include <cstdio>` to the header for `snprintf`.

- [ ] **Step 2: Write the failing test**

Create `native/test/windows/mf_camera_sink_test.cpp`. It creates the section itself — standing in for the media source — so the sink is tested with no camera registered:

```cpp
#include "test_harness.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <sync/camera/frame_ring.hpp>
#include <sync/platform/camera_identity.hpp>
#include <sync/platform/mf_camera_sink.hpp>

namespace {

using noisefactor::sync::camera::CameraSinkFrame;
using noisefactor::sync::camera::CameraSinkSubmit;
using noisefactor::sync::camera::CameraSinkUnavailableReason;
using noisefactor::sync::camera::FrameRingReader;
using noisefactor::sync::camera::MfCameraSink;
using noisefactor::sync::camera::frame_ring_bytes;
using noisefactor::sync::camera::kBytesPerPixel;
using noisefactor::sync::camera::kCanvas;
using noisefactor::sync::camera::kFrameRingSlotBytes;
using noisefactor::sync::camera::section_name;

constexpr std::size_t kStride = static_cast<std::size_t>(kCanvas.width) * kBytesPerPixel;

// Stands in for the media source: creates the section the sink expects.
struct FakeSource {
  HANDLE section = nullptr;
  void* view = nullptr;
  FakeSource() {
    const std::wstring name = section_name();
    section = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                   static_cast<DWORD>(frame_ring_bytes()), name.c_str());
    if (section != nullptr) {
      view = ::MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, frame_ring_bytes());
    }
  }
  ~FakeSource() {
    if (view != nullptr) ::UnmapViewOfFile(view);
    if (section != nullptr) ::CloseHandle(section);
  }
  [[nodiscard]] auto mapping() const -> std::span<const std::byte> {
    return {static_cast<const std::byte*>(view), frame_ring_bytes()};
  }
};

[[nodiscard]] auto canvas_filled(std::uint8_t value) -> std::vector<std::byte> {
  return std::vector<std::byte>(kFrameRingSlotBytes, static_cast<std::byte>(value));
}

SYNC_TEST(a_sink_with_no_source_reports_section_missing) {
  // No FakeSource in scope, so the section does not exist.
  MfCameraSink sink;
  SYNC_REQUIRE(!sink.available());
  SYNC_REQUIRE(sink.unavailable_reason() == CameraSinkUnavailableReason::SectionMissing);
}

SYNC_TEST(a_sink_opens_a_section_the_source_created) {
  FakeSource source;
  SYNC_REQUIRE(source.view != nullptr);
  MfCameraSink sink;
  SYNC_REQUIRE(sink.available());
  SYNC_REQUIRE(sink.unavailable_reason() == CameraSinkUnavailableReason::None);
  SYNC_REQUIRE(sink.has_capacity());
}

SYNC_TEST(a_submitted_frame_lands_in_the_ring) {
  FakeSource source;
  MfCameraSink sink;
  SYNC_REQUIRE(sink.available());
  const auto frame = canvas_filled(0x3C);
  const CameraSinkFrame submission{
      .width = kCanvas.width,
      .height = kCanvas.height,
      .row_stride = kStride,
      .bgra = frame,
      .presentation_time_us = 9999,
  };
  SYNC_REQUIRE(sink.submit(submission) == CameraSinkSubmit::Accepted);

  const FrameRingReader reader(source.mapping());
  SYNC_REQUIRE(reader.valid());
  SYNC_REQUIRE(reader.newest_sequence() == 1);
  std::vector<std::byte> out(kFrameRingSlotBytes);
  std::uint64_t presentation = 0;
  SYNC_REQUIRE(reader.read(out, kStride, presentation));
  SYNC_REQUIRE(presentation == 9999);
  SYNC_REQUIRE(static_cast<std::uint8_t>(out[0]) == 0x3C);
}

SYNC_TEST(a_frame_that_is_not_the_canvas_fails_rather_than_corrupting_the_ring) {
  FakeSource source;
  MfCameraSink sink;
  const auto frame = canvas_filled(0x11);
  const CameraSinkFrame wrong_size{
      .width = kCanvas.width,
      .height = kCanvas.height / 2,
      .row_stride = kStride,
      .bgra = std::span<const std::byte>(frame).first(kFrameRingSlotBytes / 2),
      .presentation_time_us = 1,
  };
  SYNC_REQUIRE(sink.submit(wrong_size) == CameraSinkSubmit::Failed);
  const FrameRingReader reader(source.mapping());
  SYNC_REQUIRE(reader.newest_sequence() == 0);
}

}  // namespace
```

- [ ] **Step 3: Run it and watch it fail**

```powershell
cmake --build build --config Release --target sync_mf_camera_sink_tests
```

Expected: FAIL.

- [ ] **Step 4: Implement the sink**

Create the header and implementation. `MfCameraSink::MfCameraSink()` opens `section_name()` with `OpenFileMappingW(FILE_MAP_WRITE, …)`. `ERROR_FILE_NOT_FOUND` → `SectionMissing`; `ERROR_ACCESS_DENIED` → `SectionAccessDenied`. On success it maps the view, constructs a `FrameRingWriter`, and opens the frame event. `submit` calls `FrameRingWriter::write`, `SetEvent`s the frame event, and returns `Accepted`, or `Failed` when the write is rejected. `has_capacity()` forwards to the writer. Discovery happens once at construction, matching `CmioCameraSink`'s documented behaviour.

- [ ] **Step 5: Wire the target and run**

```cmake
  add_library(sync_mf_camera_sink
    native/src/platform/windows/mf_camera_sink.cpp
  )
  target_compile_features(sync_mf_camera_sink PUBLIC cxx_std_20)
  target_include_directories(sync_mf_camera_sink PUBLIC native/include)
  target_link_libraries(sync_mf_camera_sink PUBLIC sync_camera_frame_ring PRIVATE advapi32)

  add_executable(sync_mf_camera_sink_tests
    native/test/windows/mf_camera_sink_test.cpp
    native/test/test_main.cpp
  )
  target_compile_features(sync_mf_camera_sink_tests PRIVATE cxx_std_20)
  target_include_directories(sync_mf_camera_sink_tests PRIVATE native/test)
  target_link_libraries(sync_mf_camera_sink_tests PRIVATE sync_mf_camera_sink)
  add_test(NAME sync_mf_camera_sink_tests COMMAND sync_mf_camera_sink_tests)
```

```powershell
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure -R mf_camera_sink
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(camera): feed the Windows camera from syncd"
```

---

### Task 11: Wire the provider into syncd

**Files:**
- Modify: `native/src/main.cpp:20-25,66-79,174-232`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `MfCameraSink`, `CameraFramePublisher`
- Produces: `syncd --publisher camera` working on Windows; the `ready` record reporting camera availability

- [ ] **Step 1: Include the Windows camera headers**

In `native/src/main.cpp`, inside the existing `#if defined(_WIN32)` include block (lines 20-25):

```cpp
#include <sync/platform/camera_publisher.hpp>
#include <sync/platform/mf_camera_sink.hpp>
```

- [ ] **Step 2: Construct the sink and publisher**

Beside the `SpoutFramePublisher` construction (line ~175):

```cpp
  nfsync::camera::MfCameraSink camera_sink;
  nfsync::camera::CameraFramePublisher camera(camera_sink);
```

- [ ] **Step 3: Replace the camera availability block**

The `#if defined(__APPLE__)` / `#else` pair at lines 220-232 currently hardcodes "this build does not implement camera on this platform" for Windows. Change the `#else` to an `#elif defined(_WIN32)` carrying the real values, and leave the `#else` for every other platform:

```cpp
#if defined(__APPLE__)
  constexpr bool kCameraImplemented = true;
  const bool camera_available = camera.available();
  nfsync::FramePublisher *const camera_publisher = &camera;
  const std::string camera_reason_text = nfsync::camera::describe_unavailability(
      camera.unavailable_reason(), camera.unavailable_status());
  const char *const camera_reason = camera_reason_text.c_str();
#elif defined(_WIN32)
  constexpr bool kCameraImplemented = true;
  const bool camera_available = camera.available();
  nfsync::FramePublisher *const camera_publisher = &camera;
  const std::string camera_reason_text = nfsync::camera::describe_unavailability(
      camera.unavailable_reason(), camera.unavailable_status());
  const char *const camera_reason = camera_reason_text.c_str();
#else
  constexpr bool kCameraImplemented = false;
  constexpr bool camera_available = false;
  nfsync::FramePublisher *const camera_publisher = nullptr;
  const char *const camera_reason = "this build does not implement camera on this platform";
#endif
```

- [ ] **Step 4: Link the new libraries**

In the `elseif(WIN32)` `target_link_libraries(syncd …)` call, add `sync_camera_publisher sync_mf_camera_sink`.

- [ ] **Step 5: Verify by hand and by test**

```powershell
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
.\build\Release\syncd.exe --publisher camera --static-test 2>&1 | Select-String -Pattern "camera"
```

Expected: every test still passes, and syncd names the camera provider with a real reason string — `the Sync Camera source is not registered…` before Task 12 registers it — never "this build does not implement camera on this platform".

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(camera): offer the camera provider on Windows"
```

---

### Task 12: `--register-camera` and `--unregister-camera`

**Files:**
- Modify: `native/src/cli.hpp`, `native/src/cli.cpp`, `native/src/main.cpp`, `native/test/cli_test.cpp`, `CMakeLists.txt`
- Create: `native/src/platform/windows/camera_registration.cpp`

**Interfaces:**
- Consumes: `kSyncCameraSourceClsidString`
- Produces: `cli::Mode::RegisterCamera` and `cli::Mode::UnregisterCamera`; `register_camera_source()` / `unregister_camera_source()`. Task 13 launches the first elevated, Task 15 the second from the uninstaller.

- [ ] **Step 1: Write the failing CLI test**

Append to `native/test/cli_test.cpp`:

```cpp
SYNC_TEST(register_camera_is_its_own_mode) {
  const std::array<std::string_view, 1> arguments{"--register-camera"};
  const auto result = noisefactor::sync::cli::parse(arguments);
  SYNC_REQUIRE(result.ok());
  SYNC_REQUIRE(result.options.mode == noisefactor::sync::cli::Mode::RegisterCamera);
}

SYNC_TEST(unregister_camera_is_its_own_mode) {
  const std::array<std::string_view, 1> arguments{"--unregister-camera"};
  const auto result = noisefactor::sync::cli::parse(arguments);
  SYNC_REQUIRE(result.ok());
  SYNC_REQUIRE(result.options.mode == noisefactor::sync::cli::Mode::UnregisterCamera);
}

SYNC_TEST(camera_registration_modes_reject_extra_arguments) {
  const std::array<std::string_view, 2> arguments{"--register-camera", "--port"};
  SYNC_REQUIRE(!noisefactor::sync::cli::parse(arguments).ok());
}
```

- [ ] **Step 2: Run it and watch it fail**

```powershell
cmake --build build --config Release --target sync_cli_tests
ctest --test-dir build --build-config Release -R cli --output-on-failure
```

Expected: FAIL — `Mode::RegisterCamera` does not exist.

- [ ] **Step 3: Extend the CLI**

In `cli.hpp`: `enum class Mode { Production, StaticTest, ListPairings, RevokeOrigin, RegisterCamera, UnregisterCamera };`. In `cli.cpp`, parse both flags exactly like the existing `--list-pairings` (which is also a standalone mode taking no other arguments), and add both to `print_usage`.

- [ ] **Step 4: Implement registration**

Create `native/src/platform/windows/camera_registration.cpp` exporting:

```cpp
namespace noisefactor::sync::camera {
// Loads SyncCamera.dll from beside the running executable and calls its
// DllRegisterServer. Returns a process exit code: 0 on success, 1 on failure.
[[nodiscard]] auto register_camera_source() noexcept -> int;
[[nodiscard]] auto unregister_camera_source() noexcept -> int;
}
```

Both resolve `SyncCamera.dll` via `GetModuleFileNameW(nullptr, …)` and replacing the filename — never a bare `LoadLibraryW(L"SyncCamera.dll")`, which would search the working directory. `unregister_camera_source` additionally calls `MFCreateVirtualCamera` with the same parameters syncd uses and then `IMFVirtualCamera::Remove`, so the device disappears along with its registration.

In `main.cpp`, dispatch both modes before the server starts, under `#if defined(_WIN32)`.

- [ ] **Step 5: Run the tests and try it for real**

```powershell
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

Then, in an **elevated** shell:

```powershell
.\build\Release\syncd.exe --register-camera; $LASTEXITCODE
Get-ItemProperty "HKLM:\SOFTWARE\Classes\CLSID\{2F8E7B14-9C3D-4A62-B5E1-7D4A9F2C6B08}\InprocServer32"
```

Expected: exit 0 and the DLL path. Leave it registered — Task 14 needs it.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(camera): register and unregister the camera source from the CLI"
```

---

### Task 13: The tray menu line

**Files:**
- Modify: `native/include/sync/camera_activation.hpp`, `native/src/camera_activation.cpp`, `native/src/platform/windows/app_main.cpp:64-73,440-520`, `native/test/camera_activation_test.cpp`

**Interfaces:**
- Consumes: `CameraActivationState`
- Produces: a tray line reflecting registration state and running `--register-camera` elevated

- [ ] **Step 1: Write the failing test**

Append to `native/test/camera_activation_test.cpp`:

```cpp
SYNC_TEST(needs_elevation_invites_the_user_to_enable_the_camera) {
  SYNC_REQUIRE(std::string_view(camera_activation_title(
                   CameraActivationState::NeedsElevation)) == "Enable Sync Camera…");
  SYNC_REQUIRE(camera_activation_is_actionable(CameraActivationState::NeedsElevation));
}

SYNC_TEST(an_active_camera_is_not_actionable) {
  SYNC_REQUIRE(!camera_activation_is_actionable(CameraActivationState::Active));
}

SYNC_TEST(a_failed_activation_can_be_retried) {
  SYNC_REQUIRE(camera_activation_is_actionable(CameraActivationState::Failed));
}
```

- [ ] **Step 2: Run it and watch it fail**

```powershell
ctest --test-dir build --build-config Release -R companion_model --output-on-failure
```

Expected: FAIL.

- [ ] **Step 3: Extend the state machine**

Add `NeedsElevation` and `Registering` to `CameraActivationState`, give both titles in `camera_activation_title` ("Enable Sync Camera…" and "Enabling Sync Camera…"), and add:

```cpp
// True for the states where selecting the menu line should do something: ask
// macOS for approval, or ask Windows for elevation. Replaces the macOS-only
// camera_activation_opens_settings for callers that only need "is this line
// clickable"; that function stays for the macOS Settings deep link.
[[nodiscard]] auto camera_activation_is_actionable(CameraActivationState state) noexcept -> bool;
```

Keep `camera_activation_opens_settings` and its macOS behaviour untouched — `app_main.mm:194` depends on it.

- [ ] **Step 4: Add the menu line**

In `native/src/platform/windows/app_main.cpp`, add `constexpr UINT kCommandEnableCamera = 1005;` beside the other command IDs (line ~67), a `camera::CameraActivationState camera_state` member on `AppState`, and in `show_context_menu` — after the Restart item, before Pairings:

```cpp
  UINT camera_flags = MF_STRING;
  if (!camera::camera_activation_is_actionable(app.camera_state)) {
    camera_flags |= MF_GRAYED;
  }
  ::AppendMenuW(menu, camera_flags, kCommandEnableCamera,
               to_wide(camera::camera_activation_title(app.camera_state)).c_str());
```

In `handle_command`, add a branch that sets `camera_state` to `Registering`, runs `Sync.exe --register-camera` elevated via `ShellExecuteExW` with `lpVerb = L"runas"`, waits for the process, sets `Active` on exit code 0 and `Failed` otherwise, then calls `restart_sync(app)` so syncd re-runs its once-at-construction discovery and picks up the now-registered source.

`camera_state` is computed at startup by probing `HKLM\SOFTWARE\Classes\CLSID\{…}\InprocServer32`: present → `Active`, absent → `NeedsElevation`, and `NotSupported` when the build is below 22000.

- [ ] **Step 5: Run the tests**

```powershell
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(camera): offer Enable Sync Camera from the Windows tray"
```

---

### Task 14: The virtual camera, end to end

Tier 2. The only proof the whole path works.

**Files:**
- Create: `native/test/windows/virtual_camera_e2e_test.cpp`
- Modify: `native/src/platform/windows/mf_camera_sink.cpp`, `CMakeLists.txt`, `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: everything above
- Produces: a registered, enumerable "Sync (Windows Virtual Camera)" device that yields frames

- [ ] **Step 1: Create the virtual camera from syncd**

In `MfCameraSink`'s constructor, after the section opens (or fails with `SectionMissing`), call:

```cpp
  const HRESULT hr = ::MFCreateVirtualCamera(
      MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
      MFVirtualCameraAccess_CurrentUser, kSyncCameraDisplayName,
      kSyncCameraSourceClsidString, nullptr, 0, &camera_);
```

then `camera_->Start(nullptr)`. On `E_ACCESSDENIED` set `VirtualCameraRefused` and store the `HRESULT`.

No configuration is passed to the media source: `IMFVirtualCamera` has no `SetProperty`, and `AddProperty` and `AddRegistryEntry` both require administrator permissions that unelevated syncd lacks. The source instead grants `INTERACTIVE` on its section and pairs with whoever is logged in — see spec §4.1.

- [ ] **Step 2: Write the end-to-end test**

Create `native/test/windows/virtual_camera_e2e_test.cpp`: create the camera, `MFEnumDeviceSources` with `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID`, find the device whose friendly name contains `Sync`, activate it, read a sample, assert it is the idle card (dark, non-black); then construct an `MfCameraSink`, submit a known solid-colour canvas, read another sample, and assert it matches that colour after NV12 round-trip within tolerance. Finish with `IMFVirtualCamera::Remove`.

- [ ] **Step 3: Run it locally**

```powershell
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure -R virtual_camera_e2e
```

Expected: PASS on LARGEBOI. This is the moment the feature is real.

- [ ] **Step 4: Confirm with a third-party consumer**

Open the Windows Camera app and confirm "Sync" appears and shows the waiting card. This is a sanity check, not the gate.

- [ ] **Step 5: Add Tier 2 to CI**

If Task 2's hosted verdict was exit 0, add the test to the existing `windows` job. Registration needs elevation; GitHub-hosted Windows runners run as an administrator, so `--register-camera` succeeds there without a prompt:

```yaml
      - name: Register the camera source and run the end-to-end camera test
        shell: pwsh
        run: |
          $ErrorActionPreference = 'Stop'
          & ".\build\$env:BUILD_TYPE\syncd.exe" --register-camera
          if ($LASTEXITCODE -ne 0) { throw "camera registration failed" }
          ctest --test-dir build --build-config $env:BUILD_TYPE --output-on-failure -R virtual_camera_e2e
          if ($LASTEXITCODE -ne 0) { throw "virtual camera end-to-end test failed" }
          & ".\build\$env:BUILD_TYPE\syncd.exe" --unregister-camera
```

- [ ] **Step 6: If the hosted verdict was 2 or 3, add a self-hosted runner**

Register LARGEBOI as a self-hosted runner labelled `sync-camera`, add the job:

```yaml
  windows-camera:
    name: Windows camera end-to-end
    runs-on: [self-hosted, Windows, X64, sync-camera]
    timeout-minutes: 30
```

with the same steps, and document the machine in Scaffold's `docs/runbook/office-lan.md` beside the other desk machines — it is the second self-hosted runner after `spare-mac`.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "test(camera): prove the Windows virtual camera end to end"
```

---

### Task 15: Packaging

**Files:**
- Modify: `packaging/windows/Sync.iss:57-78`, `scripts/package-windows.ps1`, `scripts/verify-windows-bundle.ps1`

**Interfaces:**
- Consumes: `SyncCamera.dll`
- Produces: an installer that ships the DLL and unregisters it on uninstall

- [ ] **Step 1: Stage the DLL**

In `scripts/package-windows.ps1`, copy `SyncCamera.dll` into the bundle beside `syncd.exe`. The `[Files]` line in `Sync.iss` already globs `{#SyncBundleDir}\*`, so no installer change is needed for staging.

- [ ] **Step 2: Assert it in the bundle verifier**

In `scripts/verify-windows-bundle.ps1`, add `SyncCamera.dll` to the required-files list so a bundle missing it fails the build rather than shipping a camera-less installer.

- [ ] **Step 3: Unregister on uninstall**

In `Sync.iss`, before the two `taskkill` lines in `[UninstallRun]`:

```
; Registration wrote to HKLM, so removal needs the same elevation. A per-user
; uninstall is not elevated and will prompt; a declined prompt leaves the key
; behind for the next install to reuse, which is why failure is ignored.
Filename: "{app}\syncd.exe"; Parameters: "--unregister-camera"; Flags: runhidden skipifdoesntexist; RunOnceId: "UnregisterSyncCamera"
```

- [ ] **Step 4: Build and smoke the installer**

```powershell
pwsh scripts/package-windows.ps1
pwsh scripts/verify-windows-bundle.ps1
pwsh scripts/smoke-windows-app.ps1
```

Expected: all pass, and the staged bundle contains `SyncCamera.dll`.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "build(camera): ship and unregister the camera source"
```

---

### Task 16: Release harness (Scaffold repo)

Cross-repo: `~/platform/scaffold`, not the Sync tree.

**Files:**
- Modify: `.github/workflows/build-sync-preview.yml`, `scripts/create-sync-windows-release-manifest.mjs`, `scripts/verify-sync-preview-release.mjs`, `scripts/create-sync-release-manifest.test.mjs`, `docs/runbook/office-lan.md` (only if Task 14 Step 6 applied)

**Interfaces:**
- Consumes: the packaged bundle
- Produces: a release manifest recording the camera source

- [ ] **Step 1: Note the runner floor**

`build-windows` runs on `windows-2022` (build 20348), below the camera's 22000 floor. Building is unaffected — only running a virtual camera needs 22000 — so the build job stays as it is. Add a comment saying so, next to the existing `SPOUT_REVISION` comment block, so nobody later "fixes" it by bumping the runner.

- [ ] **Step 2: Record the DLL in the manifest**

In `create-sync-windows-release-manifest.mjs`, add a `cameraSourcePath` option hashed the same way `spoutLibraryPath` is, emitted as a `cameraSource` record. Update `verify-sync-preview-release.mjs` to require it, and add a case to `create-sync-release-manifest.test.mjs` asserting a manifest missing it is rejected.

- [ ] **Step 3: Run the Scaffold tests**

```bash
cd ~/platform/scaffold && node --test scripts/create-sync-release-manifest.test.mjs scripts/verify-sync-preview-release.test.mjs
```

Expected: PASS.

- [ ] **Step 4: Commit in the Scaffold repo**

```bash
git add -A && git commit -m "build(sync): record the Windows camera source in the release manifest"
```

---

### Task 17: Documentation

**Files:**
- Modify: `README.md:78-88` (Sync repo), `~/platform/noisedeck/app/docs/Sync.md:7-12,37`

- [ ] **Step 1: Update the Sync README provider table**

Add a Windows row to the provider table and extend the camera paragraph, which currently describes only macOS activation. State the Windows 11 floor, the one-time Enable step, and that both NV12 and RGB32 are offered.

- [ ] **Step 2: Update the Known issues list**

Add: the camera is unavailable on Windows 10; and a declined elevation prompt at uninstall leaves the CLSID registered.

- [ ] **Step 3: Update Noisedeck's Sync.md**

In `~/platform/noisedeck/app/docs/Sync.md`, the "Current availability" list says Windows provides "Spout and NDI output". Add the camera, with its Windows 11 requirement and the one-time enable step, and add it to the step 7 list of what receiving applications can select.

- [ ] **Step 4: Commit both repos**

```bash
cd ~/platform/sync && git add -A && git commit -m "docs: describe the Windows camera provider"
cd ~/platform/noisedeck && git add -A && git commit -m "docs(sync): describe the Windows camera provider"
```

---

### Task 18: Acceptance — Noisedeck to Noisedeck

The user's stated bar. Not a substitute for the CI gate; the last check before delivery.

- [ ] **Step 1: Prepare Noisedeck**

```bash
cd ~/platform/noisedeck && node scripts/fetch-noisemaker.js && npm install
```

Noisemaker v0 is banned — the pin is the rolling `/1` channel. If the engine is missing, run the fetch script; never point anything at a local `0.8.0/` directory.

- [ ] **Step 2: Install and enable Sync**

Install the Task 15 installer, launch Sync, and choose **Enable Sync Camera…** from the tray. Approve the elevation prompt. Confirm the line then reads Active.

- [ ] **Step 3: Send from the first Noisedeck**

Open Noisedeck, choose **View > send to sync…**, **Connect Sync**, approve the origin in the native prompt, name the output, and **Start sending**. Confirm the dialog's **Sent** counter climbs.

- [ ] **Step 4: Receive in the second Noisedeck**

In a second Noisedeck instance, add a media effect and select **Sync** as its camera source. Confirm the first instance's output renders inside the second.

- [ ] **Step 5: Check the failure modes deliberately**

Stop sending and confirm the media effect shows the waiting card rather than freezing or going black. Start sending again and confirm it recovers. Check the dialog's **GPU busy** / **Network** counters stay sane.

- [ ] **Step 6: Record the evidence**

Capture a screenshot of the second Noisedeck rendering the first instance's output through the camera, and note the frame counters. This is the acceptance artifact.

---

### Task 19: Review and address feedback

- [ ] **Step 1: Run the review**

Use the `code-review` skill at `high` over the full diff.

- [ ] **Step 2: Triage with the receiving-code-review skill**

Use `superpowers:receiving-code-review`. Verify each finding technically before acting; do not implement a suggestion that is wrong, and say why.

- [ ] **Step 3: Fix what survives triage, with a test for each behavioural finding**

- [ ] **Step 4: Re-run everything**

```powershell
cmake --build build --config Release
ctest --test-dir build --build-config Release --output-on-failure
```

Expected: all green, including `virtual_camera_e2e`.

- [ ] **Step 5: Confirm CI is green**

Push and confirm the `windows` job — including the Tier 2 step — passes. The feature does not ship on a red or skipped camera job.

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "fix(camera): address review feedback"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
| --- | --- |
| §1 mechanism, platform floor | 8, 11, 14 |
| §1.1 HKLM registration, session-0 section ownership | 8, 9, 12 |
| §2 components | 5, 6, 7, 8, 10 |
| §2.1 portability refactor | 3, 4 |
| §3 both pixel formats | 5, 8 |
| §4 frame path, §4.1 naming and DACL | 6, 8, 10 |
| §5 activation lifecycle | 12, 13, 15 |
| §5.1 failure taxonomy | 10 |
| §6.1 Tier 1 | 4, 5, 6, 7, 8, 9, 10 |
| §6.2 Tier 2 | 14 |
| §6.3 runner question | 2, 14 |
| §7 packaging and documentation | 15, 16, 17 |
| §8 out of scope — unsigned DLL confirmation | 2, 14 |

**Placeholder scan:** Task 8 Steps 5-6 and Task 14 Step 2 describe implementations in prose rather than full listings — `IMFMediaSource`/`IMFMediaStream` boilerplate runs to several hundred lines and the exact shape depends on what Task 8 Step 2's tests demand. Both steps name every method, every return value, and the exact ordering, and their tests are given in full. Every other step carries literal code.

**Type consistency:** `CameraSinkUnavailableReason` members (Task 10 Step 1) match their uses in Tasks 11 and 14. `MfCameraSink` is default-constructible (Task 10) and constructed that way in Task 11 Step 2. `frame_ring_bytes()`, `section_name()`, `frame_event_name()`, `FrameRingWriter`, `FrameRingReader` (Task 6) match their uses in Tasks 8 and 10. `kSyncCameraSourceClsidString` (Task 8) matches Tasks 9, 12, 13, 14. `camera_activation_is_actionable` (Task 13) is new and does not collide with the retained `camera_activation_opens_settings`.
