<!-- repo-hero -->
<a href="https://sync.noisedeck.app/"><img src="docs/hero.jpg" alt="Sync Browser to native video bridge" width="100%"></a>

<sub>Open source from <a href="https://noisefactor.io">Noise Factor</a> &middot; <a href="https://github.com/noisefactorllc">more projects</a></sub>

# Sync

Sync is Noise Factor's low-latency bridge between browser renderers and native
video ecosystems. It carries GPU-rendered RGBA frames over authenticated
loopback WebSockets and republishes them through platform-native video-sharing
providers, allowing applications such as Noisedeck to appear in existing source
pickers without a custom plugin in every downstream host.

## Project status

Sync is under active development. This source tree currently includes:

- an independently versioned frame and control protocol;
- a browser SDK with bounded discovery, explicit pairing, and separate control
  and sender-data sockets;
- a native C++20 loopback daemon with per-origin, revocable authorization;
- bounded sender and connection ownership with non-blocking browser submission;
- a macOS Metal publisher and dynamically discovered Syphon integration;
- a Windows Spout publisher and a cross-platform NDI publisher, both
  dynamically discovered;
- a generic Ubuntu 24.04 user daemon with an owner-only control socket and a
  stock-v4l2loopback `Sync Camera` output;
- an Apple Silicon menu-bar companion and a Windows tray companion, each with
  bounded helper supervision; and
- native, browser, protocol, security-boundary, and real-loopback tests.

The SDK 0.3.0 and native companion source at
[`1972af1ce3f0d14054f3693e250c668aff536884`](https://github.com/noisefactorllc/sync/commit/1972af1ce3f0d14054f3693e250c668aff536884)
include audio input for 1–32 channels per source. Native interfaces feed browser
audio controls over the same authenticated loopback service used for video.
That exact source passed the
[cross-platform CI matrix](https://github.com/noisefactorllc/sync/actions/runs/34803984585)
and the separate
[Windows camera end-to-end workflow](https://github.com/noisefactorllc/sync/actions/runs/34803984593).
The [combined audio and video diagnostic report](https://sync.noisedeck.app/performance/research-2026-09-18-combined-audio-video/)
records a 30-minute synthetic 32-channel audio test with concurrent 1080p video
submission. Its video rate counts sender submissions, not distinct camera or
Noisedeck B pixels. It reports zero audio cursor gaps and ring drops, but does
not validate sample values. Physical 32-channel capture and sustained combined
1080p60 delivery remain unqualified. The
[Windows platform report](https://sync.noisedeck.app/performance/research-2026-09-18-windows-platform-parity/)
provides architecture and test evidence, not combined-load platform parity.
[Native preview 0.2.68](https://sync.noisedeck.app/#download)
and [SDK 0.3.0](https://github.com/noisefactorllc/sync/releases/tag/sdk-v0.3.0)
are published from that source. Current evidence is not a general hardware
compatibility claim. See the
[audio research, design, and qualification matrix](docs/audio-input.md).

Both companions are previews and are not ready for general use.
Reverse-direction native video sources and automatic updates are not part of the
current public implementation.

## Known issues

Known problems and limits in the current preview, with a workaround where
one exists. Add an entry when a report is diagnosed, and remove it when the
fix ships.

- **Heat limits long sessions on fanless Macs.** Sync does carry 1080p60
  with 32 channels of audio: on an M2 MacBook Air with a WING Rack, a
  one-hour run through Syphon delivered 99.97% of marked frames with every
  channel identified, inside the 1% loss budget the run was held to (see the
  [thermal report](https://sync.noisedeck.app/performance/research-2026-09-23-thermal-load-gpu-clock-cap/)).
  But a heavy program can heat a fanless laptop until macOS caps the GPU
  clock. The render keeps its share and the WebCodecs encoder starves, so
  delivery used to collapse to 30-40 fps. noisemaker `fde2ea40` and
  `9b88e567` cut the default program's GPU cost roughly in half, and
  Noisedeck `788feedc` skips a draw while the encoder is behind, so a capped
  machine now runs slower instead of collapsing. Workaround: connect power,
  give the laptop airflow, use a lighter program or smaller image, or use a
  Mac with a fan.
- **A saturated GPU stops the picture instead of slowing it.** When another
  app takes the whole GPU, the hardware encoder produces nothing and the
  output goes dark. Noisedeck `729d9e62` retries, and sending resumes a few
  seconds after the load ends. Starting while the GPU is saturated fails
  with a warmup timeout and does not retry; start again after the load ends.
  A recovered output is a new Syphon server with the same name. OBS
  reconnects to it on its own; other receivers may need the source selected
  again.
- **Audio can drop out briefly on a busy Mac.** In that hour of WING
  capture, one disturbance stalled the browser's AudioWorklet for about
  three seconds, while native audio reads kept zero drops and zero cursor
  gaps. Outside that moment the audio stayed intact in our runs.
- **Not yet qualified at 1080p60 with 32-channel audio:** Sync Camera,
  Spout, NDI, Windows, and Linux. Capture from a physical audio interface has
  been checked on macOS only.
- **The native render helper (sync-render) has rough edges.** A Seance
  session with guests turned off refuses the helper's anonymous identity and
  the render path stops. A render error that repeats every frame, other than
  audio, leaves the output dark with no restart. The Windows helper needs the
  Visual C++ runtime installed. Linux does not package the helper yet
  (it needs Qt 6.9; Ubuntu 24.04 ships 6.4).
- **Syphon receivers can hang when they stop mid-stream.** Syphon's `-stop`
  and `-newFrameImage` take the same lock in opposite orders, upstream too.
  Sync's own receiver probe works around it (`ab97ae5`, `c8f880c`); any
  other Syphon receiver app that stops while frames arrive can still hit it.
- **Sync output stops when the Noisedeck window is fully covered.** Chrome
  marks a fully covered page hidden, Noisedeck then pauses rendering, and
  Sync output stops until the window is visible again. Workaround: keep part
  of the Noisedeck window visible, or put it on a second screen.
- **Content blockers block the loopback health request.** uBlock Origin,
  uBlock Origin Lite, and AdGuard ship EasyPrivacy and "block LAN" rules that
  stop public pages from reaching `127.0.0.1`. Chrome logs
  `net::ERR_BLOCKED_BY_CLIENT` and Noisedeck reports the companion absent.
  Workaround: set the blocker to no filtering on the Noisedeck origin.
- **Browsers require a loopback permission first.** Chrome 145+ and Firefox
  150+ gate `127.0.0.1` behind the `loopback-network` permission. The passive
  check stops at "Needs attention". Only Connect Sync can show the browser's
  prompt. Noisedeck Standalone (Electron) grants it by default.
- **The macOS approval row can lag a day behind the request.** System
  Settings > Privacy & Security > Security shows "System software from
  application "Sync" was blocked from loading" only for a live request.
  If the row is missing, quit Sync. Relaunch it from Applications.
  Open Settings again.
- **The native pairing prompt defaults to Deny.** Pressing Return in the
  companion's pairing dialog denies the origin.
- **The Windows installer is not code-signed** and Windows warns about an
  unrecognised publisher. Each release publishes a SHA-256 instead.
- **The Windows camera needs Windows 11.** `MFCreateVirtualCamera` arrived in
  build 22000, so on Windows 10 the provider reports "the camera needs
  Windows 11 (build 22000) or later" and the rest of Sync is unaffected.
- **Declining the uninstall prompt leaves the camera registered.** Removing
  the CLSID from HKLM needs the same elevation that added it, and a per-user
  uninstall is not elevated. The stale key is harmless and the next install
  reuses it. Run `syncd --unregister-camera` as an administrator to clear it.
- **Two users signed in at once share one Windows camera.** The media source
  cannot be told which account to pair with without an administrator-only
  API, so it accepts frames from whoever is logged in interactively. With
  fast user switching the most recent sender wins.

## Providers

Sync publishes through every provider that is available on the running
platform, at once: a single named output appears simultaneously as a Spout
sender and an NDI source on Windows. Receiving applications pick whichever
they support.

| Provider | Platform | Runtime | Bundled |
| --- | --- | --- | --- |
| Syphon | macOS | `Syphon.framework` | Yes — see [docs/dependencies/syphon.md](docs/dependencies/syphon.md) |
| Spout | Windows | `SpoutLibrary.dll` | Yes — see [docs/dependencies/spout.md](docs/dependencies/spout.md) |
| NDI | Windows, macOS, Linux | Operator-installed NDI Runtime (`libndi.so.5` on Linux) | No — the SDK licence forbids redistribution; see [docs/dependencies/ndi.md](docs/dependencies/ndi.md) |
| Camera | macOS | Sync Camera extension, bundled in Sync.app | Yes — activated by Sync.app on first launch, approved once in System Settings |
| Camera | Windows 11 | `SyncCamera.dll`, bundled with the installer | Yes — enabled once from the tray menu, which asks for administrator rights |
| Camera | Ubuntu 24.04 x86_64 | Ubuntu `v4l2loopback` packages | No kernel module is bundled; one-time setup configures the stock module |

The camera provider publishes a 1920×1080 stream, so any app that picks a
camera can use it. While no sender is live, the camera shows a dark Sync
waiting card instead of a black picture.

On **macOS** it is a CoreMediaIO system extension shipping inside Sync.app,
appearing as "Sync Camera". macOS activates it only for an app under
/Applications, and asks the user once. Sync.app restarts its helper when
activation completes, which is when the camera first appears in Noisedeck's
provider list.

On **Windows** it is a Media Foundation virtual camera, appearing as "Sync"
— the pipeline appends "Windows Virtual Camera" to the name itself. It needs
Windows 11 (build 22000), because `MFCreateVirtualCamera` does not exist
before it; on Windows 10 the provider reports itself unavailable and says so.
The media source is a COM server the frame server loads, so its CLSID has to
live in HKLM, which needs administrator rights once: choose **Enable Sync
Camera…** from the tray menu and approve the prompt. Sync restarts its helper
afterwards, for the same reason it does on macOS.

The Windows camera offers NV12 first and RGB32 second, and converts from the
one BGRA canvas per consumer, so two applications can negotiate different
formats against the same device.

On **Ubuntu 24.04**, `syncd` is an unprivileged systemd user service and the
camera appears as `Sync Camera`. The setup command installs a fixed
`v4l2loopback` configuration and a group-scoped udev rule; ordinary daemon
operation never needs root. Sync deliberately creates no `/dev` alias or udev
symlink: it discovers and validates the kernel-owned `/dev/videoN` each time it
opens the camera. PipeWire and WirePlumber are useful interoperability checks
reported by `syncctl doctor`, but are not in the frame path. The daemon writes
NV12 directly to V4L2.

No provider is ever linked at build time. Sync discovers each provider at
runtime through its documented public entry point. A provider whose runtime
is absent reports itself unavailable rather than failing the daemon.
An unavailable selected provider also prints one line to stderr that explains
why. Thus, `available: false` is never the whole diagnosis.
The `ready` record on stdout keeps its exact shape.

## Building the native daemon

Sync requires CMake 3.21 or newer, a C++20 compiler, OpenSSL 3, and libuv.
macOS builds also use the system Foundation and Metal frameworks and locate
libuv through pkg-config. Windows builds use MSVC and locate libuv and
OpenSSL through a CONFIG package such as vcpkg:

```powershell
vcpkg install libuv:x64-windows openssl:x64-windows
cmake -S . -B build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release --target syncd
ctest --test-dir build --build-config Release --output-on-failure
```

The camera's end-to-end test is labelled `sync_camera_e2e` and is the one test
that needs more than a build: Windows 11 and the media source registered under
HKLM. Run `syncd --register-camera` from an elevated prompt first, or exclude
it with `ctest --label-exclude sync_camera_e2e`. Everything else, including the
media source driven in process, runs on any Windows machine.

MSVC is what CI builds and what the installer ships. The tree also builds and
passes its tests under MinGW-w64 (GCC), which needs no administrator rights
and is a practical local setup:

```bash
pacman -S --needed mingw-w64-x86_64-{gcc,cmake,ninja,openssl,libuv,pkgconf}
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
```

Run the tests from a shell with a Windows-shaped environment.
An MSYS2 login shell unsets `LOCALAPPDATA` and points `TMP`/`TEMP` at `/tmp`.
The pairing store resolves its default path from `%LOCALAPPDATA%`.
It refuses paths that are not drive-absolute.
Thus, several tests fail in an MSYS2 login shell for reasons unrelated to the code.

```bash
cmake -S . -B build
cmake --build build --target syncd -j4
ctest --test-dir build --output-on-failure
```

The daemon binds only to IPv4 and IPv6 loopback. Production mode uses port
`53979` unless overridden:

```bash
./build/syncd
./build/syncd --port 54000
./build/syncd --list-pairings
./build/syncd --revoke-origin https://visuals.example
```

Naming no publisher selects every provider the platform offers. Naming one or
more restricts the daemon to exactly those, and each accepts an explicit
runtime path for development builds:

```bash
./build/syncd --publisher spout --publisher ndi
./build/syncd --publisher ndi --ndi-runtime /opt/ndi/lib
./build/syncd --publisher spout --spout-library C:/Spout/SpoutLibrary.dll
```

See the provider table above for each runtime and license boundary.

### Ubuntu 24.04 daemon

The supported Linux package is x86_64. The same `.deb` can be downloaded
directly or indexed unchanged by a signed Noisefactor-hosted APT repository:

```bash
sudo apt install ./Sync-<version>-linux-amd64.deb
sudo syncctl camera setup --user "$USER"
# Log out and back in if setup added the group.
systemctl --user enable --now noisedeck-sync.service
syncctl pair
syncctl doctor
```

If another `v4l2loopback` configuration or loaded instance already exists,
setup refuses to merge or renumber it. Resolve that administrator-owned state
explicitly and rerun setup. To select one already validated device, add a user
unit drop-in with `systemctl --user edit noisedeck-sync.service`:

```ini
[Service]
ExecStart=
ExecStart=/usr/bin/syncd --camera-device /dev/video12
```

Safe removal stops and disables the user service first, removes the package,
then purges only setup files whose complete contents still match Sync's
templates:

```bash
systemctl --user disable --now noisedeck-sync.service
sudo apt remove noisedeck-sync
sudo apt purge noisedeck-sync
```

Locally modified module configuration is retained and named on stderr. Removal
does not unload a live module or remove the `noisedeck-sync` group.

## Packaging the desktop previews

### macOS

Packaging requires macOS 13 or newer, an Apple Silicon build, `dylibbundler`,
`librsvg`, and a locally built `Syphon.framework`. The release workflow pins
Syphon source revision `71351d4b484cd2d1917867f7846a5cdca724552d`; use that
same revision for local release-equivalent packages.

```bash
SYNC_PRODUCT_VERSION=X.Y.Z
cmake -S . -B build-package \
  -DSYNC_PRODUCT_VERSION="$SYNC_PRODUCT_VERSION" \
  -DSYNC_SYPHON_FRAMEWORK_PATH=/absolute/path/to/Syphon.framework
cmake --build build-package --target sync_macos_dmg -j4
SYNC_PACKAGE_DIR=build-package/package \
  node --test test/packaging/macos-package.test.js
scripts/smoke-macos-app.sh "$PWD/build-package/package/Sync.app"
```

The local target creates an unsigned app and DMG. Developer ID signing,
notarization, stapling, and public publication belong to the Noise Factor
release workflow so credentials never enter this public repository.

### Windows

Packaging requires Windows 10 or newer, an x64 MSVC toolchain, Inno Setup 6
(`ISCC` on `PATH`), ImageMagick (`magick` on `PATH`), and a locally built
`SpoutLibrary.dll`.

```powershell
$SyncProductVersion = "X.Y.Z"
cmake -S . -B build-package -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DSYNC_PRODUCT_VERSION="$SyncProductVersion" `
  -DSYNC_SPOUT_LIBRARY_PATH=C:\absolute\path\to\SpoutLibrary.dll `
  -DSYNC_WINDOWS_DEPENDENCY_PATH="$env:VCPKG_INSTALLATION_ROOT\installed\x64-windows\bin"
cmake --build build-package --config Release --target sync_windows_installer --parallel 4
$env:SYNC_PACKAGE_DIR = "build-package/package"
node --test test/packaging/windows-package.test.js
./scripts/smoke-windows-app.ps1 -Bundle "$PWD/build-package/package/Sync"
```

The local target creates an unsigned application directory and installer.
Authenticode signing and public publication likewise belong to the Noise
Factor release workflow.

## Browser SDK

The browser SDK can connect any web renderer to Sync. It includes direct RGBA,
Canvas 2D, WebGL2, and WebGPU export queues plus native audio-source discovery
and bounded PCM reads. The dependency-free source modules live in
[`browser/`](browser/).

The [Sync SDK 0.3.0 release page](https://github.com/noisefactorllc/sync/releases/tag/sdk-v0.3.0)
includes an installable tarball, browser modules, and SHA-256 checksums.
Install the tarball in your application:

```bash
npm install https://github.com/noisefactorllc/sync/releases/download/sdk-v0.3.0/noisefactor-sync-0.3.0.tgz
```

You can then import from `@noisefactor/sync`.
See the [developer guide](docs/developers.md) for direct browser imports, local builds, and the complete API.

Passive discovery never starts pairing. A deliberate user action must call
`pair()`. The host application owns storage for the returned token. See the
[browser client guide](browser/README.md) for permission and lifecycle details.
Runnable [browser and Electron examples](examples/) cover Canvas 2D, WebGL2, and WebGPU.
Audio integrations must check for a selected and available `audio` provider
with direction `receive`; the SDK and companion product versions are
independent. See the [developer audio example](docs/developers.md#receive-native-audio).
Linux users who need JACK through PipeWire must start Sync through `pw-jack`;
see the [explicit service setup](docs/audio-input.md#linux-jack-through-pipewire).

```js
import { SyncBridgeClient } from '@noisefactor/sync'

const pairingClient = new SyncBridgeClient()
const { token } = await pairingClient.pair('My visual app')
pairingClient.close()

const sync = new SyncBridgeClient({ token })
await sync.connect()
```

## Tests

```bash
npm run test:unit
npm run test:browser
npm run test:packaging
SYNC_DAEMON_PATH=build/syncd npm run test:integration
ctest --test-dir build --output-on-failure
```

`test:unit` checks protocol and harness behavior without a native build. The
integration command also runs `test:integration:soak` in a separate test process:
the regular short soak uses a 60 FPS ceiling and fixed memory readings, while an
idle real daemon verifies process inspection (including private memory on Windows).
Windows inspection has a 60-second command budget inside a 75-second test and a
180-second runner budget. A missing daemon fails integration instead of skipping it.
To repeat these checks after a harness change, run `test:integration:soak` on the
same build several times. Every run must pass without a retry that hides failure.
Fairness under unlimited writes is checked with immediately completed writes and a
queued stop, independently of native throughput or runner scheduling speed.

The memory soak streams 1080p frames through a test-receiver daemon while
cycling senders and probing health, and fails on footprint growth. Run it
against a Release build. A Debug daemon is too slow to be representative:

```bash
SYNC_DAEMON_PATH=build-release/syncd SYNC_SOAK_SECONDS=60 npm run test:soak
```

This standalone soak still sends at unlimited speed by default. Set
`SYNC_SOAK_FPS` to a positive frame-rate ceiling for a paced workload.

## Security

Unknown origins cannot silently publish. Pairing requires a browser-initiated
request and a visible native approval prompt. Reusable credentials are scoped
to an exact normalized origin and can be revoked. Please report suspected
vulnerabilities privately using [SECURITY.md](SECURITY.md).

## License

Sync is released under the [MIT License](LICENSE). See
[TRADEMARK.md](TRADEMARK.md) for the branding boundary. Third-party runtime
providers retain their own licenses and are not relicensed by this repository.

Copyright © 2026 Noise Factor LLC
