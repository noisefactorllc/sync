# Sync: the Windows camera provider — design

Status: implementation spec. Brings the `camera` provider, shipped on macOS as
a CoreMediaIO system extension, to Windows as a Media Foundation virtual
camera. The provider seam, the fan-out, and the sender lifecycle do not
change; this spec covers the Windows half that sits beneath `CameraSink`.

## 1. Mechanism and platform floor

Windows gets a Media Foundation virtual camera: `MFCreateVirtualCamera` plus a
custom media source COM DLL. The API's floor is **Windows build 22000**, so the
provider reports itself unavailable on Windows 10 with a reason, exactly as NDI
does when its runtime is absent. `Sync.iss` keeps `MinVersion=10.0`; the
installer still installs on Windows 10, the camera line simply reads
unavailable there.

The rejected alternative is a DirectShow filter, which would cover Windows 10
and need no elevation, but is invisible to Store/UWP apps and the Windows
Camera app. A camera some of the user's apps silently cannot see is a worse
product than a camera that is honestly absent on an older OS.

### 1.1 Two facts that shape everything below

The media source DLL is **loaded into the Frame Server service**, which runs as
Local Service in session 0. Two consequences drive the rest of this design:

- The CLSID must be registered under **HKLM**. Local Service cannot see the
  interactive user's `HKCU`, so registration is a one-time elevated act, and
  the DLL must live where Local Service can read it — `{autopf}` on an elevated
  install, never `%LOCALAPPDATA%\Programs`.
- syncd runs unelevated in the user's session and therefore **cannot create
  `Global\` kernel objects** (that needs `SeCreateGlobalPrivilege`). So the
  media source creates the shared section in session 0, where it needs no such
  privilege, and syncd only ever opens it. This inverts macOS, where
  `CmioCameraSink` pushes into a queue the extension hands it.

Elevation is needed for the HKLM write and nothing else.
`MFCreateVirtualCamera` with `MFVirtualCameraAccess_CurrentUser` requires no
administrator, so syncd creates the camera itself, unelevated, in the user's
own session.

## 2. Components

```
                       session 1 (user)          |  session 0 (Local Service)
FrameReceiver -> PublisherHub -> CameraFramePublisher
                                      |
                                 MfCameraSink  --opens-->  [ shared section ]
                                                                  ^  creates
                                                           SyncCamera.dll
                                                           (IMFMediaSource)
                                                                  |
                                                            Frame Server -> consumer
```

| Unit | Where | Role |
| --- | --- | --- |
| `SyncCamera.dll` | new | The media source. Self-registering COM, owns the section, hosts `CameraRelayPolicy` and draws the idle card. Plays the role `camera_extension/main.mm` plays on macOS. |
| `sync_camera_transport` | new | Ring header, slot layout, sequence numbers, object naming, and the BGRA→NV12 conversion. Linked by both sides. Pure logic, no Windows camera APIs. |
| `MfCameraSink` | new | `CameraSink` implementation in syncd. Mirror of `CmioCameraSink`. |
| portable fitter | refactor | `fit_camera_frame` without vImage. |
| Windows idle card | new | Direct2D/DirectWrite in place of CoreGraphics. |

### 2.1 Portability refactor

`camera_publisher.cpp` and `camera_extension/relay_policy.cpp` are already
plain portable `.cpp` files that happen to sit under
`native/src/platform/macos/`. Windows needs both unchanged, so they move to
`native/src/camera/` and their CMake targets leave the `if(APPLE)` block.
`camera_frame_fitter.hpp` and `camera_idle_card.hpp` lose their
`#error "Apple platforms"` guards and gain per-platform implementations;
`compute_camera_placement` is pure arithmetic and moves as-is.

This is the smallest refactor that lets both platforms share the logic. No
other restructuring is in scope.

## 3. Pixel formats

The camera advertises **NV12 first, RGB32 second**. Both ship in the first
release. Most Media Foundation consumers expect NV12 and some handle RGB32
poorly; advertising only one of them would either surprise consumers or lean on
pipeline conversion we do not control.

The ring always carries **top-down BGRA**, the same canvas macOS uses, and the
media source converts per negotiated type. Conversion lives on the consumer
side of the boundary rather than in syncd because two consumers may negotiate
different types against the same device, and a single canvas keeps one source
of truth. `bgra_to_nv12` lives in `sync_camera_transport` as an ordinary
function so it is unit-tested in CI with no camera present, with the Video
Processor MFT as an optional accelerated path and this function as the
correctness reference.

## 4. Frame path

syncd fits the sender frame to the 1920×1080 canvas and memcpys it into a ring
slot behind a seqlock. The media source reads the newest complete slot on
`RequestSample`, converts if the negotiated type is NV12, and wraps the result
as an `IMFSample`.

There is no signal. An earlier draft had syncd set an event per frame, but the
source is pull-driven — it reads the ring when a consumer asks it for a frame —
so nothing ever waited on that event and it cost a syscall per frame to
achieve nothing.

`has_capacity()` answers "has a consumer asked for a frame recently", not "is
the ring full". The ring is never full: it is three slots and the newest wins.
What it has to detect is the camera being open to nobody, and the section
cannot tell it that — syncd keeps its own view mapped, which keeps the named
section alive after every consumer has gone. So the source stamps
`last_demand_us` in the ring header on each `RequestSample`, and syncd treats
demand older than one second as nobody watching. That preserves the contract
that fitting is only paid for frames something will read.

Both halves stamp and compare that timestamp with `camera_clock_us()`
(`steady_clock`, which is machine-wide on Windows), and the ring header
carries a version so a mixed-version install refuses rather than reading
payloads at the wrong offset.

`CameraRelayPolicy` moves into the DLL unchanged, including the 250 ms grace
that stops a jittery 30 fps sender from flickering black. With no sender the
idle card is drawn, so the camera behaves as it does on macOS: present and
showing "Sync: waiting for Noisedeck" whether or not syncd is running.

### 4.1 Naming and access

Section `Global\SyncCamera.frames`, created
by the media source with a DACL granting `INTERACTIVE` write access and
`ALL APPLICATION PACKAGES` plus `ALL RESTRICTED APPLICATION PACKAGES` read
access, so whichever user is logged in interactively can feed the camera and
AppContainer and LPAC consumers — Store apps, the Windows Camera app — can read
from it.

An earlier draft keyed both names by user SID and had syncd pass its SID to the
media source through `IMFVirtualCamera::SetProperty`. That is not possible:
`IMFVirtualCamera` has no `SetProperty`, and the two methods that could carry
such configuration — `AddProperty` and `AddRegistryEntry` — both require
administrator permissions, which unelevated syncd does not have. Granting
`INTERACTIVE` reaches the same place with less machinery and no admin-only API.

The cost is that two users logged in at once through fast user switching share
one section, and the most recent writer wins. See §8.

## 5. Activation lifecycle

The tray gains one line, mirroring the macOS menu item at `app_main.mm:411`.
`CameraActivationState` ports with `NeedsElevation` replacing `NeedsApproval`
and `NotInApplications` dropping out, having no Windows analogue.

1. Not registered → the line reads "Enable Sync Camera…" and is enabled.
2. Selecting it runs `Sync.exe --register-camera` via `ShellExecuteW(…, "runas", …)`.
   That elevated process registers the CLSID under HKLM and exits. It does
   nothing else.
3. syncd, unelevated, calls `MFCreateVirtualCamera(SoftwareCameraSource,
   Lifetime_System, Access_CurrentUser, "Sync", <CLSID>)`. `Lifetime_System`
   keeps the camera present across reboots, matching macOS.
4. Uninstall runs `Sync.exe --unregister-camera`, which calls
   `IMFVirtualCamera::Remove` and unregisters the CLSID. A per-user uninstall
   is not elevated, so this step prompts; if the user declines, the HKLM key is
   orphaned and the next install reuses it. The uninstaller must not fail on a
   declined prompt.

### 5.1 Failure taxonomy

`CameraSinkUnavailableReason` gains Windows members, and `describe()` becomes
per-platform — every existing string says "the Sync Camera extension", which
means nothing on Windows. `unavailable_status()` carries the `HRESULT` exactly
as it carries the `OSStatus` today.

| Reason | Meaning | What the user does |
| --- | --- | --- |
| `NotSupported` | Windows build below 22000 | Nothing; Windows 10 has no camera |
| `SourceNotRegistered` | CLSID absent from HKLM | Choose "Enable Sync Camera…" |
| `VirtualCameraRefused` | `MFCreateVirtualCamera` failed | Read the HRESULT; camera privacy may be denied |
| `SectionMissing` | Registered, but no consumer has activated the source | Open a camera app once |
| `SectionAccessDenied` | DACL rejected syncd | Bug; report the HRESULT |

## 6. Verification

**The camera provider does not ship until its end-to-end path runs green in
CI.** This is a gate, not an aspiration: no part of this feature merges on the
strength of manual testing on one desk.

### 6.1 Tier 1 — every Windows runner, no camera, no elevation

Runs in the existing `windows` job in `.github/workflows/ci.yml`:

- Ring transport: sequence, wrap, backpressure, torn-read rejection.
- `bgra_to_nv12` correctness against known values, including odd dimensions and
  the full and limited range decisions. This is where "both formats" earns its
  coverage.
- Fitter: letterbox and pillarbox placement, premultiply over black, rejection
  of malformed frames. Ports the existing macOS tests.
- `CameraRelayPolicy` and `CameraActivationState`: already tested, moved.
- `DllRegisterServer` against a redirected hive via `RegOverridePredefKey`, so
  registration is proven without touching the machine's real HKLM.
- **In-process media source**: load `SyncCamera.dll` through `DllGetClassObject`,
  instantiate the `IMFMediaSource` directly, negotiate NV12 and then RGB32,
  feed frames through a test-owned section, and assert the samples produced.
  This needs `mfplat` but **not** the Frame Server, so it should run on any
  Windows runner including Server SKUs. Media Foundation is an optional feature
  on Server, so the probe in §6.3 must confirm `mfplat` is present on the
  hosted image; if it is not, this test moves to the self-hosted runner with
  Tier 2 and Tier 1 keeps only the camera-free logic tests.

### 6.2 Tier 2 — needs Windows 11 build 22000+

`MFCreateVirtualCamera` → `MFEnumDeviceSources` finds the device → open it as
an ordinary consumer → assert the idle card with no sender, real frames with
one → `Remove()`. This is the only proof that the whole path works, and it
cannot run on a Windows 10-era or Server-restricted image.

### 6.3 The runner question — measured 2026-09-03

`ci.yml` uses `windows-latest`, which is Windows Server 2025 (build 26100). The
build number clears 22000, but Microsoft documents `MFCreateVirtualCamera`
against a minimum *client* only — the server minimum is blank — and Media
Foundation is an optional feature on Server SKUs. Whether Tier 2 could run
there was unknown, and this section said it had to be measured rather than
assumed. It has been.

**A hosted runner cannot host the camera, and stops in exactly one place.**
The capability probe on `windows-latest`, after registering the source:

```
build_number=26100                        Windows 11 era
mfplat=1                                  Media Foundation present
cocreateinstance_imfactivate_hr=0x00000000   the DLL registers and loads
activateobject_hr=0x00000000                 the source activates
source_implements_imfmediasourceex=1         every required interface
source_implements_imfgetservice=1
source_implements_ikscontrol=1
createpresentationdescriptor_hr=0x00000000
mfcreatevirtualcamera_hr=0x00000000          the camera is created
virtualcamera_start_hr=0x80070005            E_ACCESSDENIED
```

Everything this project owns succeeds. The image refuses only
`IMFVirtualCamera::Start`, which is where the frame server has to accept the
camera into the pipeline, and it refuses with `E_ACCESSDENIED` — an
environment policy on a Server SKU with no interactive session, not a defect
in the source. Worth recording precisely, because "the camera does not work in
CI" would otherwise read as something to fix in the code.

So branch 3 applies:

- **Tier 1 runs on the hosted job** and is the bulk of the coverage: the media
  source driven in process through its own DLL, both negotiated formats, the
  ring, the conversion, registration against a redirected hive. `ci.yml` gates
  on all of it.
- **Tier 2 is gated on the probe verdict.** Where an image can host a camera
  the end-to-end test runs and fails the job; where it cannot, the job records
  the per-step HRESULTs rather than failing for something the runner cannot
  do. On the hosted job it therefore never fails; the job that turns it into a
  gate is the self-hosted one below.
- **The ship gate is a self-hosted Windows 11 runner, and it now exists.**
  LARGEBOI — Alienware Aurora R13, Windows 11 Home 26200, with `FrameServer`,
  `FrameServerMonitor`, and `mfsensorgroup.dll` 10.0.26100.9278 — is
  registered as `largeboi-sync-camera` with labels
  `[self-hosted, Windows, X64, sync-camera]`, mirroring the existing
  `[self-hosted, macOS, ARM64, sync-performance]` job in Scaffold's
  `build-sync-preview.yml`. `.github/workflows/camera-e2e.yml` drives it. Host
  setup is in Scaffold's `docs/runbook/office-lan.md`.

Scaffold's `build-sync-preview.yml` builds Windows on `windows-2022`, which is
build 20348 and below the floor. What §6.3 concludes applies there too.

### 6.4 What a camera host has to be — measured 2026-09-03

Standing up that runner turned up three more constraints, each of which
presents as the *same* `E_ACCESSDENIED` from `MFCreateVirtualCamera` or
`Start`. They are worth separating, because the single shared symptom makes
them very easy to confuse for one another — and for a defect in the source.

1. **The runner must not be a Windows service.** A service runs in session 0,
   which is the same condition the hosted image is in, and produces the same
   `virtualcamera_start_hr=0x80070005` above. Run in the interactive desktop
   session, where the identical probe returns `0x00000000` for every call.
   This is why the runner is not installed with `--runasservice`.
2. **The registered DLL must live outside every user profile.** The frame
   server runs as LOCAL SERVICE, which cannot read `C:\Users\<someone>`. A
   source registered from a path under a profile loads for `syncd` — which is
   why registration itself reports success — and then fails inside the frame
   server, surfacing as `MFCreateVirtualCamera` returning `E_ACCESSDENIED`
   with nothing else to distinguish it from constraint 1. Measured directly:
   the same build registered from `C:\Users\aayar\platform\sync\build\Release`
   fails, and from `C:\actions-runner-sync\...` passes, because the drive root
   grants `BUILTIN\Users` read and LOCAL SERVICE is a member of Users.
3. **The runner does not need to be elevated, and should not be.**
   Registration writes `HKLM\SOFTWARE\Classes\CLSID\{2F8E7B14-…}` and its
   `InprocServer32` child, and nothing else. Granting the runner account
   `FullControl` on that one subtree lets an unelevated runner re-point the
   registration at each build. The alternative — an elevated runner — would
   run every workflow step as administrator to obtain two registry writes.
   The grant confers nothing on an account that can already elevate; it must
   not be widened to `Users`, and must not be applied to the parent `CLSID`
   key, which would allow hijacking any COM class on the machine.

The workflow re-points the registration at each build and restores the
installed one in an `always()` step. It restores by re-registering the
installed `syncd`, never by `--unregister-camera`: unregistering deletes the
CLSID key, which would both break the Sync install on that desktop and destroy
the ACL grant from constraint 3 along with it.

## 7. Packaging and documentation

- `Sync.iss` ships `SyncCamera.dll` and adds the `--unregister-camera` step.
- Scaffold's `build-sync-preview.yml` builds and packages the DLL.
- `create-sync-windows-release-manifest.mjs` records it, and
  `verify-sync-preview-release.mjs` asserts the widened shape. Both assert
  manifest structure today and both must change together.
- The README provider table gains a Windows row for Camera; the Known Issues
  list gains the Windows 10 exclusion and the declined-uninstall-prompt case.
- Noisedeck's `app/docs/Sync.md` availability section gains the Windows camera,
  including that it requires Windows 11 and one elevated enable step.

## 8. Out of scope

Windows 10 support, DirectShow, incoming camera sources (Sync as an input),
resolutions other than the fixed 1920×1080 canvas, code-signing the installer,
and simultaneous use by two interactively logged-in users (§4.1 — the second
writer wins, which is acceptable for a preview on a desktop product). Nothing in Microsoft's documentation requires the media source DLL
be signed and independent implementations ship unsigned, but §6.3's probe
should confirm the Frame Server loads an unsigned DLL before the design leans
on it.
