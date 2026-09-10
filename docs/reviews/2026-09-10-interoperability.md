# Sync interoperability verification

Date: 2026-09-10

Status: local implementation and review complete. Publication is separate.

## Result

The SDK candidate supports independent web visualizers without a Noise Factor renderer or account.
It includes typed ES modules, RGBA input, Canvas export, WebGL2 export, WebGPU export, native statistics, and local diagnostics.
The native companion accepts independent packaged applications through the documented `app://` origin grammar.
Each origin still needs user approval.

The examples include browser and Electron applications.
The developer guide and protocol reference describe installation, permissions, frame ownership, limits, errors, and cleanup.
The local Scaffold website copy describes the same integration path.
New feature prose uses the STE100 skill.

## SDK artifact

The package is [noisefactor-sync-0.2.0.tgz](../../dist/sdk/0.2.0/noisefactor-sync-0.2.0.tgz).
The distribution also contains a complete module directory and SHA-256 checksums.
Every distributed module matches the final browser source.

Tarball SHA-256:

```text
28026d8a63cce2646b677c0c05120e71937243d860229da1bf6f0ebe6263b628
```

The package test installs a generated tarball into a separate project.
It imports the public entry point and sends literal RGBA bytes.
An external TypeScript 5.9.3 consumer compiles against the final tarball with strict DOM types.
The distribution command rejects an existing version directory.

## Automated checks

| Check | Result |
| --- | --- |
| `npm run test:unit` | 245 passed, no failures or skips |
| `npm run test:packaging` | 16 passed, no failures, 7 artifact checks skipped |
| CMake build and CTest | Build passed, all 22 native test groups passed |
| Real loopback integration | 26 passed, no failures, 1 occupied-port case skipped |
| Integration memory soak | 4 passed, no failures or skips |
| External TypeScript consumer | Strict compilation passed |
| STE lint | No violations in the new guides or changed public prose |

The seven package skips require staged native release artifacts.
The loopback skip requires exclusive access to port 53979.
An installed Sync companion occupied that port during this run.
All new native acceptance processes used separate ports and isolated credentials.

## Real browser and native pixels

The physical test host used macOS and an Apple M2 GPU.
Chrome 151.0.7922.34 used the Metal graphics backend.
Headless Chrome did not expose a WebGPU adapter, so the hardware tests used a visible browser.

Each case compared a literal 3 by 2 RGBA pattern with the complete exported payload.
The pattern includes asymmetric rows and alpha values of 0, 128, and 255.
The same cases then passed through Sync and Syphon to an independent native receiver.
That receiver compared every pixel byte after Metal readback.

| Source | Browser bytes | Independent Syphon receiver |
| --- | --- | --- |
| RGBA with padded input rows | Exact | Exact |
| Canvas 2D | Exact | Exact |
| WebGL2 default framebuffer | Exact | Exact |
| WebGL2 framebuffer object | Exact | Exact |
| WebGPU RGBA8 | Exact | Exact |
| WebGPU BGRA8 | Exact | Exact |
| WebGPU RGBA8 sRGB texture | Exact | Exact |
| WebGPU BGRA8 sRGB texture | Exact | Exact |

These cases used the sRGB descriptor and straight alpha.
The GPU checks also covered WebGL state restoration and WebGPU row alignment.
Unit tests cover additional alpha, bounds, pressure, resize, stale completion, and cleanup cases.

A separate 12-second run sent changing markers at 64 by 32 pixels.
The native receiver observed 751 unique markers.
It reported no invalid markers, missed sequences, dimension errors, or command errors.
Thirteen RSS samples ranged from 21,584 to 21,664 KiB.
This small-frame smoke test does not establish sustained performance at larger resolutions.

Read the [pixel and soak evidence](evidence/2026-09-10/pixels-and-soak.json).

## Runnable example checks

Canvas, WebGL2, and WebGPU passed in each of these environments:

- Loopback HTTP in Chrome.
- HTTPS in Chrome with a temporary test certificate.
- An HTTP child frame with explicit delegation from its HTTPS parent.
- Electron 44.0.0 with `app://com.example.visualizer`.

All 12 combinations included resize, stop, and restart checks.
Request headers contained the expected HTTP, HTTPS, and packaged application origins.
Electron retained sandboxing and context isolation, with Node integration disabled.
The examples also passed pairing denial, pending-start cancellation, WebGL context loss, and daemon-loss checks.

Chrome received loopback permission only for the test origins.
Electron used the example's exact-origin permission policy.
The pairing server used an approval fixture instead of the native consent dialog.
These tests do not certify the native dialog's visual behavior.

Read the [example evidence](evidence/2026-09-10/examples.json).

## Reproduce the checks

Run the native build and automated tests from the repository root:

```bash
cmake --build build-review -j 4
ctest --test-dir build-review --output-on-failure
npm run test:unit
npm run test:packaging
SYNC_DAEMON_PATH=build-review/syncd npm run test:integration
```

The physical acceptance command needs Playwright and the Syphon framework:

```bash
PLAYWRIGHT_MODULE=/absolute/path/to/playwright \
SYNC_ACCEPTANCE_HEADFUL=1 \
SYNC_DAEMON_PATH=build-review/syncd \
SYPHON_FRAMEWORK_PATH=/absolute/path/to/Syphon.framework \
npm run test:acceptance:interop
```

Use `SYNC_ACCEPTANCE_BROWSER_ONLY=1` only for an explicitly limited browser run.
That mode reports its reduced scope and does not certify native delivery.

The example test also needs the Electron executable:

```bash
PLAYWRIGHT_MODULE=/absolute/path/to/playwright \
ELECTRON_PATH=/absolute/path/to/Electron \
SYNC_PAIRING_SERVER_PATH=build-review/sync_pairing_test_server \
npm run test:acceptance:examples
```

Compile [the typed consumer](../../test/packaging/consumer.ts) in a separate project after installation of the SDK tarball:

```bash
tsc --strict --noEmit --module nodenext --moduleResolution nodenext \
  --target es2022 --lib es2022,dom consumer.mts
```

Copy `consumer.ts` as `consumer.mts`, or mark the consumer project as an ES module project.

## Review and boundaries

Independent reviews covered frame adapters, SDK and native changes, packaging, types, examples, documentation, and acceptance checks.
We fixed every actionable issue.
The final review also verified cancellation during renderer creation and sender teardown.

Physical receiver certification in this run covers macOS and Syphon only.
Spout, NDI, and camera receivers need separate physical tests on their supported platforms.
This run does not certify Safari, Firefox, Display P3 output, or a Noisedeck camera round trip.
The existing Noisedeck SDK snapshot remains unchanged.
The existing sustained-1080p60 and long-session performance notices remain in place.

No Git operation, push, package publication, website deployment, or signed companion release occurred.
