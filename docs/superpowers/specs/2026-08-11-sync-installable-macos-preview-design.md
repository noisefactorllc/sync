# Sync Installable macOS Preview Design

**Status:** Approved for autonomous implementation and delivery on 2026-08-11

**Repositories:** `noisefactorllc/sync`, `noisefactorllc/scaffold`, and the
Noisedeck preview documentation

## Goal

Ship a public, signed, notarized, Apple Silicon macOS preview of Sync that a
person can download from `downloads.noisefactor.io`, install without a terminal,
launch as a menu-bar companion, and use to publish Noisedeck output to a Syphon
host.

The preview remains explicitly unsuitable for production shows. Windows,
Spout, NDI, reverse-direction sources, automatic updates, and Mac Intel builds
remain outside this milestone.

## Approaches considered

### Native menu app supervising the existing daemon — selected

Build a small AppKit menu-bar executable and keep `syncd` as a separately signed
helper inside the app bundle. The app starts, monitors, and stops the helper,
polls its loopback health endpoint, and invokes its existing JSON management
commands to list and revoke pairings.

This preserves the already-tested libuv server lifecycle and command-line test
surface. A daemon failure cannot take down AppKit, and the menu app can present a
useful recovery state instead of entering a crash loop.

### Single-process AppKit and libuv application — rejected

Running the native server inside the menu app would remove one executable, but
it requires a new cross-thread shutdown contract, reconciliation between the
AppKit and libuv event loops, and new signal behavior. Those changes are not
needed to make the preview installable and would put the proven server seam at
risk.

### Electron wrapper — rejected

Electron would fit the existing desktop product matrix, but Sync has no web UI
and needs only a status menu. Shipping a browser runtime around a small native
bridge would greatly increase download size, memory use, startup time, and the
attack surface.

## Product behavior

`Sync.app` is an `LSUIElement` application: it appears in the menu bar and not
in the Dock. The first launch shows one short explanation that Sync is running
in the menu bar and links to the Sync documentation. Subsequent launches open
quietly.

The menu contains:

- current state: starting, running, another instance detected, stopped, or
  failed;
- Syphon availability and the number of active browser senders;
- Restart Sync when the managed helper is stopped or failed;
- paired origins, with confirmation before revoking one;
- Launch at Login, implemented with `SMAppService.mainApp`;
- Copy Diagnostics, containing versions, process state, health status, provider
  capability, sender count, and at most 64 KiB of recent helper stderr;
- links to the Noisedeck Sync guide and the Sync source repository;
- About Sync and Quit.

Diagnostics never include pairing tokens. They also omit paired origins unless
the user explicitly views the pairings menu.

The app starts its embedded helper on launch. If a compatible Sync instance is
already healthy on port 53979, the app reports that external instance and does
not attempt to replace or terminate it. If the managed helper exits
unexpectedly, the app reports the failure and offers a manual restart; it does
not loop indefinitely. On Quit, the app sends the managed helper `SIGTERM`,
waits for a two-second graceful shutdown deadline, and force-terminates only
that child if the deadline expires.

Pairing approval remains in the existing native security prompt. Pairing
storage remains origin-scoped and revocable in the existing secure store.

## Bundle and dependency layout

The app supports Apple Silicon and requires macOS 13 or later.

```text
Sync.app/
  Contents/
    Info.plist
    MacOS/
      Sync
      syncd
    Frameworks/
      Syphon.framework/
      runtime dependency dylibs, if any
    Resources/
      Sync.icns
      LICENSE.txt
      Third-Party-Notices.txt
```

`Sync.icns` is generated deterministically from a checked-in branded SVG during
packaging; the build does not fetch an icon from a mutable URL.

The app passes the exact bundled Syphon framework path to `syncd`; it does not
depend on `/Library/Frameworks` or a user-installed SDK. Syphon is built from
official source at commit `71351d4b484cd2d1917867f7846a5cdca724552d` and its
redistribution notice is included verbatim in `Third-Party-Notices.txt`.

All non-system dynamic libraries used by `syncd` are copied into
`Contents/Frameworks` and rewritten to bundle-relative install names. Packaging
fails if `otool -L` finds a Homebrew, runner, or workspace path after fixup.

The menu executable, helper, bundled libraries, Syphon framework, and outer app
are signed in inside-out order with the existing Noise Factor Developer ID
identity and hardened runtime. The DMG contains `Sync.app`, an Applications
symlink, the MIT license, and the third-party notice.

## Health and management interfaces

`GET /health` remains unauthenticated, loopback-only, and protocol-version
neutral. It gains one additive numeric field:

```json
{"activeSenders": 0}
```

The value is generated per request from occupied sender slots, not cached at
server initialization. Existing health fields and wire protocol version 1 do
not change.

The menu app runs the embedded helper with `--list-pairings` and
`--revoke-origin <normalized-origin>`. Both commands already emit bounded JSON
and use the pairing store's inter-process lock. The app parses only the expected
schema and treats malformed or timed-out management output as a recoverable UI
error. Health requests use a 500 ms request timeout. Pairing management commands
use a two-second total timeout.

## Build and release architecture

Sync receives CMake targets for the menu app and deterministic app-bundle/DMG
packaging. Local unsigned packaging remains possible for tests. Release version
uses `0.2.<scaffold workflow run number>`; CMake injects that exact value into
the health response and `Info.plist` without changing wire protocol version 1
or the independently versioned browser SDK.

Scaffold receives a dedicated `build-sync-preview.yml` workflow rather than
adding Sync to the Electron three-platform matrix. The workflow:

1. accepts an exact 40-character Sync commit on `main`;
2. checks out Sync and the pinned official Syphon source on `macos-latest`
   (ARM64);
3. installs CMake, OpenSSL 3, libuv, pkg-config, and bundle-fixup tooling;
4. runs native, browser, integration, app-model, and packaging tests;
5. builds Syphon and the self-contained `Sync.app`;
6. imports the existing Developer ID certificate;
7. signs every nested executable/framework/library and the app with hardened
   runtime;
8. builds and signs the DMG;
9. submits it to Apple, staples the accepted ticket, and verifies it with
   `codesign`, `spctl`, and a mounted-app launch/health smoke test;
10. emits a SHA-256 checksum and retained workflow artifact;
11. atomically uploads the DMG and checksum to the downloads host;
12. updates and deploys the Sync card on `downloads.noisefactor.io` only after
    the uploaded artifact verifies byte-for-byte.

The workflow is manually dispatched for this preview. It does not run on every
Sync push and does not expose signing or server credentials to the public Sync
repository.

## Public download delivery

The existing subscriber download endpoint always creates a personalized ZIP,
so Sync does not use `/downloads/*`. The desktop-downloads sidecar gains one
allowlisted public installer endpoint:

```text
/public-downloads/sync-mac-arm64.dmg
```

It serves only the stable staged file
`/installers/sync/Sync-arm64.dmg`, as a raw DMG, with an attachment filename,
`nosniff`, HTTP byte-range support through `aiohttp.web.FileResponse`, and a
five-minute public cache lifetime. Arbitrary paths and all existing licensed
download behavior stay unchanged.

The downloads page adds a final Sync card labeled **Preview**, with the public
Mac download, supported-platform text, a link to the Noisedeck Sync guide, the
current preview version/date, and an explicit warning that it is not ready for
production use. Page-level copy distinguishes this public MIT preview from the
subscriber-gated licensed applications above it.

Noisedeck's in-app Sync guide changes its first step from building the companion
to downloading and installing the public preview, while retaining source-build
instructions as an advanced option.

## Failure handling

- A missing or incompatible bundled Syphon framework makes the app report
  “Syphon unavailable”; it does not claim to be ready.
- A loopback port collision is distinguished from a compatible external Sync
  instance and shown as a recoverable error.
- Helper stderr is drained continuously into a fixed-size ring; it cannot fill
  a pipe and deadlock the daemon or grow without bound.
- Health requests have a 500 ms timeout and pairing-management commands have a
  two-second timeout; menu actions never block AppKit.
- The release workflow never updates the public card before notarization,
  upload, checksum verification, and remote HTTP verification succeed.
- An upload lands under a temporary name and is renamed atomically. The prior
  stable DMG remains available until replacement.
- A failed page deployment leaves the verified DMG staged but does not falsely
  claim the new version is live.

## Verification contract

Local verification must prove:

- all existing Sync native, browser, integration, and real Syphon tests remain
  green;
- health reports exact live sender counts through sender create/close paths;
- app state transitions, external-instance detection, helper-exit recovery,
  bounded diagnostics, and pairing JSON parsing are covered by focused tests;
- the unsigned app bundle has the expected layout and no unbundled non-system
  runtime paths;
- launching the packaged app produces a healthy loopback daemon with bundled
  Syphon available, and quitting leaves no helper process.

Release verification must additionally prove:

- every nested code object and the app pass strict signature verification;
- Apple accepts notarization and the ticket staples successfully;
- Gatekeeper accepts both the DMG and installed app;
- the downloaded public DMG matches the published SHA-256 checksum;
- the live downloads page links to that public artifact and identifies it as a
  preview;
- the Noisedeck preview documentation links to the live public download.
