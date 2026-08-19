# Sync: Spout, NDI, and the Windows desktop build — design

Status: implementation spec. Extends the shipped macOS/Syphon preview with two
new send providers (Spout, NDI) and an installable Windows desktop companion
analogous to `Sync.app`.

## 1. Provider model

`FramePublisher` stays the single provider interface and `PublisherHub` stays
the fan-out point (`kMaximumProviders = 4`). Providers are added beside the
existing macOS stack, never inside it.

```
FrameReceiver -> PublisherHub -> [ FramePublisher, ... ]
                                   |
  macOS   MetalFramePublisher -> MetalFrameConsumer[] -> SyphonMetalConsumer
  Windows SpoutFramePublisher            (direct, CPU RGBA)
  both    NdiFramePublisher              (direct, CPU RGBA)
```

### 1.1 Why Spout and NDI are direct publishers

The Metal/Syphon split exists because `SyphonMetalServer` consumes an
`id<MTLTexture>`, so something has to own the GPU upload and the shared
staging ring. Neither new provider has that shape:

* Spout's stable public C boundary is `SpoutLibrary.dll`'s `GetSpout()`
  handle, whose `SendImage` takes a CPU RGBA pointer and performs the GPU
  upload and texture share internally.
* NDI's send path is `NDIlib_send_send_video_async_v2`, which takes a CPU
  frame descriptor.

Introducing a Direct3D staging layer underneath either one would add a copy
and a failure surface that the providers' own public ABIs do not ask for.
Windows therefore has no GPU publisher; both Windows providers consume the
decoded `protocol::FrameView` directly.

The cost is explicit and accepted: with Spout and NDI both live, the frame is
uploaded twice. That is the correct trade against inventing a private GPU
layer beneath two vendor ABIs that already own their own upload.

### 1.2 Fan-out and selection

Every *available* send provider is also *selected*. One `Start sending` in
Noisedeck publishes the same named output through every available provider
simultaneously — on Windows an output appears as a Spout sender and an NDI
source at once. `PublisherHub` already opens, publishes to, and closes all
providers as a unit, so this needs no hub change.

Consequence for the browser contract: `capabilities.providers` may now carry
more than one `available && selected && direction === 'send'` entry. Hosts
must treat the *set* of selected send provider ids as the identity to hold
across a recovery, not a single id.

## 2. Runtime dependency boundary

Both new providers follow the boundary `docs/dependencies/syphon.md`
established for Syphon: discover at runtime, never link at build time, never
vendor the provider's headers.

| Provider | Module | Entry point | Bundled? |
| --- | --- | --- | --- |
| Spout | `SpoutLibrary.dll` | `GetSpout()` (C export, stable vtable) | Yes — BSD-2-Clause permits redistribution; pinned revision ships in the installer |
| NDI | `Processing.NDI.Lib.x64.dll` / `libndi.dylib` | `NDIlib_v5_load()` | No — the NDI SDK licence forbids redistributing the runtime; Sync discovers a user-installed runtime via `NDI_RUNTIME_DIR_V6`/`NDI_RUNTIME_DIR_V5` and the documented install locations |

Search paths are bounded and must never include a user-writable directory
that the daemon would otherwise not already trust, for the reason recorded in
`syphon_consumer.mm`: loading a module executes its code. The installer's
private directory and the vendor's own documented install location are the
only defaults; a developer build passes an explicit `--spout-library` or
`--ndi-runtime` path.

A provider whose module is absent, whose ABI probe fails, or whose
initialisation fails reports `available == false` and is simply not offered.
Absence is never an error.

### 2.1 Why both providers are pinned differently

Neither provider's header may be vendored, so each implementation mirrors the
vendor's ABI locally. The two mirrors carry different risk, and that drove
different mitigations.

NDI's dynamic-load struct is **append-only**: every versioned name
(`NDIlib_v2`..`NDIlib_v6`) is a typedef of the same growing struct, so a
declared prefix stays layout-compatible with any newer runtime. Discovering a
user-installed runtime is therefore safe.

Spout's `SPOUTLIBRARY` is a **172-slot C++ vtable** with no such guarantee,
and six of those slots (145-150) sit inside `#ifdef NTDDI_WIN10_RS4`. A DLL
built without that block has a 166-slot vtable in which `CreateOpenGL`,
`CloseOpenGL`, and `Release` all move six slots earlier — calling the wrong
slot is undefined behaviour, not a clean failure. Two things contain that:
the installer ships a **pinned** `SpoutLibrary.dll` so the shipped layout is
controlled rather than discovered, and the implementation validates the
assumption at load time by checking that the vtable entry at index 171 points
inside the module's own image, which a 166-slot vtable's would not.
`--spout-library` is a developer escape hatch, not a compatibility mechanism.

## 3. Failure taxonomy

`ProviderFailureKind` gains, alongside the existing Metal kinds:

```
SpoutInitializationFailed, SpoutSendFailed,
NdiInitializationFailed,   NdiSendFailed,
```

`provider_failure_name` gains matching snake_case names. Existing enumerator
values are not renumbered.

## 4. Windows platform layer

| Concern | macOS today | Windows |
| --- | --- | --- |
| Pairing store | `openat`/`renameat`/`fsync`, `fchmod 0700`, ACL strip | `CreateFileW`, `MoveFileExW(MOVEFILE_WRITE_THROUGH \| MOVEFILE_REPLACE_EXISTING)`, `FlushFileBuffers`, explicit owner-only DACL |
| Store location | `~/Library/Application Support/Noisefactor Sync/pairings.v1` | `%LOCALAPPDATA%\Noisefactor Sync\pairings.v1` |
| Pairing prompt | `MacPairingPrompt` (`begin`/`poll`/`cancel`) | `WindowsPairingPrompt`, same non-blocking contract, `MessageBoxW` on a dedicated thread |
| Companion | `Sync.app` menu-bar app + managed `syncd` helper | `Sync.exe` tray app (`Shell_NotifyIconW`) + managed `syncd.exe` in a kill-on-close Job Object |
| Event pump | `CFRunLoopRunInMode` | `PeekMessageW`/`DispatchMessageW` drain |
| Package | `.app` + DMG | staged bundle + Inno Setup `Sync-<version>-x64-Setup.exe` + portable ZIP |

`CompanionModel` is already platform-neutral C++ and moves from
`native/src/platform/macos/` to `native/src/companion_model.cpp` so both
companions share it. Its `HealthSnapshot::syphon_available` bool generalises
to a bounded list of available provider ids, because a Windows companion has
two providers to report and the macOS one has one.

## 5. Verification

Native code is gated in CI, not on the authoring machine. `.github/workflows/ci.yml`
gains a `windows-latest` matrix job that configures, builds, and `ctest`s the
Windows targets, plus a packaging job. The dynamic-discovery seams are
exercised by tests that assert graceful unavailability when the module is
absent — the same shape as `sync_syphon_missing_pool_diagnostics`.
