# Sync Installable macOS Preview Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a public, signed, notarized Apple Silicon `Sync.app` DMG from `downloads.noisefactor.io` that runs the existing Sync daemon as a managed menu-bar companion with bundled Syphon.

**Architecture:** A small AppKit `LSUIElement` application supervises the existing `syncd` executable as an embedded child and consumes its loopback health and JSON pairing-management interfaces. Sync owns deterministic unsigned app/DMG packaging; Scaffold owns signing credentials, notarization, public installer serving, atomic deployment, and downloads-page publication.

**Tech Stack:** C++20, Objective-C++/AppKit/Foundation/ServiceManagement, CMake, libuv, OpenSSL 3, Syphon/Metal, shell packaging, GitHub Actions, Python/aiohttp, Node.js page tooling.

## Global Constraints

- Work on existing branches only; do not create a branch, worktree, or pull request.
- Preserve unrelated dirty Scaffold files and stage only this plan's exact files.
- The public app supports Apple Silicon and requires macOS 13 or later.
- Release versions are `0.2.<scaffold workflow run number>`; wire protocol remains version 1 and browser SDK remains independently versioned.
- Build Syphon from official commit `71351d4b484cd2d1917867f7846a5cdca724552d` and include its complete redistribution notice.
- The public URL is `/public-downloads/sync-mac-arm64.dmg`; existing `/downloads/*` licensing behavior must not change.
- Never expose Apple, SSH, or GitHub App credentials to the public Sync repository.
- Never include pairing tokens or paired origins in copied diagnostics.
- Do not publish the downloads card before notarization, upload, checksum verification, and remote HTTP verification succeed.

---

### Task 1: Live sender count in the health contract

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `native/include/sync/control.hpp`
- Modify: `native/include/sync/protocol.hpp`
- Modify: `native/src/control.cpp`
- Modify: `native/src/server.cpp`
- Modify: `native/test/control_test.cpp`
- Modify: `native/test/server_contract_test.cpp`
- Modify: `test/integration/loopback.test.js`

**Interfaces:**
- Produces: `control::encode_health(product_version, instance_id, providers, active_senders)`.
- Produces: additive `activeSenders` integer in `GET /health`.
- Produces: CMake cache string `SYNC_PRODUCT_VERSION`, default `0.2.0`, compiled into `kProductVersion`.

- [ ] **Step 1: Add failing control and server tests**

Assert that the health encoder emits `"activeSenders":3`, and that a real server reports 0, then 1 after `createSender`, then 0 after sender closure. Keep existing field assertions.

```cpp
const auto health = control::encode_health("0.2.0", "instance", providers, 3);
SYNC_REQUIRE(health.find("\"activeSenders\":3") != std::string::npos);
```

```js
assert.equal((await health()).activeSenders, 0)
const sender = await client.createSender({ name: 'health-count' })
assert.equal((await health()).activeSenders, 1)
await sender.close()
assert.equal((await health()).activeSenders, 0)
```

- [ ] **Step 2: Run the focused tests and confirm failure**

Run:

```bash
cmake --build build-acceptance -j 8
ctest --test-dir build-acceptance -R 'sync_protocol_tests|sync_server_contract' --output-on-failure
SYNC_DAEMON_PATH=build-acceptance/syncd node --test test/integration/loopback.test.js
```

Expected: compilation or assertion failure because the new encoder parameter/field is absent.

- [ ] **Step 3: Implement dynamic health generation and product-version injection**

Add an occupied-sender counter to `Server`. Remove the cached `health_body_`; encode the response at request time. Define the version through CMake:

```cmake
set(SYNC_PRODUCT_VERSION "0.2.0" CACHE STRING "Sync product version")
target_compile_definitions(sync_protocol PUBLIC
  SYNC_PRODUCT_VERSION="${SYNC_PRODUCT_VERSION}"
)
```

```cpp
#ifndef SYNC_PRODUCT_VERSION
#define SYNC_PRODUCT_VERSION "0.2.0"
#endif
inline constexpr std::string_view kProductVersion = SYNC_PRODUCT_VERSION;
```

- [ ] **Step 4: Run native, browser, and integration suites**

Run:

```bash
cmake --build build-acceptance -j 8
ctest --test-dir build-acceptance --output-on-failure
npm run test:browser
SYNC_DAEMON_PATH=build-acceptance/syncd npm run test:integration
```

Expected: all suites pass and health transitions match live sender ownership.

- [ ] **Step 5: Commit the health contract**

```bash
git add CMakeLists.txt native/include/sync/control.hpp native/include/sync/protocol.hpp native/src/control.cpp native/src/server.cpp native/test/control_test.cpp native/test/server_contract_test.cpp test/integration/loopback.test.js
git commit -m "feat: expose live Sync sender health"
```

### Task 2: Testable companion state and bounded diagnostics

**Files:**
- Create: `native/include/sync/platform/companion_model.hpp`
- Create: `native/src/platform/macos/companion_model.cpp`
- Create: `native/test/macos/companion_model_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `enum class CompanionState { Starting, Running, External, Stopped, Failed, PortConflict }`.
- Produces: `HealthSnapshot` with product/version/Syphon availability/active sender count.
- Produces: `CompanionModel::append_stderr(std::string_view)` with a 65,536-byte newest-data cap.
- Produces: `CompanionModel::diagnostics()` with no token or origin input.

- [ ] **Step 1: Write failing model tests**

Cover helper launch, compatible external instance, failed health with a live child, child exit, manual restart, a 70 KiB stderr append retaining exactly the newest 65,536 bytes, and diagnostic output containing no configured secret/origin strings.

```cpp
CompanionModel model("0.2.7");
model.helper_started(42);
model.observe_health({.compatible = true, .syphon_available = true,
                      .active_senders = 2});
SYNC_REQUIRE(model.state() == CompanionState::Running);
SYNC_REQUIRE(model.health().active_senders == 2);
```

- [ ] **Step 2: Run the model target and confirm failure**

Run: `cmake --build build-acceptance --target sync_companion_model_tests -j 8`

Expected: target/source missing.

- [ ] **Step 3: Implement the pure C++ model**

Keep process launching, JSON parsing, AppKit, and pasteboard calls out of this unit. Make every transition explicit and ensure diagnostics draw only from version, state, health, child exit status, and the bounded stderr ring.

- [ ] **Step 4: Register and run the tests**

Run:

```bash
cmake -S . -B build-acceptance
cmake --build build-acceptance --target sync_companion_model_tests -j 8
ctest --test-dir build-acceptance -R sync_companion_model_tests --output-on-failure
```

Expected: focused model tests pass.

- [ ] **Step 5: Commit the model seam**

```bash
git add CMakeLists.txt native/include/sync/platform/companion_model.hpp native/src/platform/macos/companion_model.cpp native/test/macos/companion_model_test.cpp
git commit -m "feat: add Sync companion state model"
```

### Task 3: Native menu-bar companion and helper supervision

**Files:**
- Create: `native/src/platform/macos/companion_process.hpp`
- Create: `native/src/platform/macos/companion_process.mm`
- Create: `native/src/platform/macos/app_main.mm`
- Create: `native/test/macos/companion_process_test.mm`
- Create: `packaging/macos/Info.plist.in`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `CompanionProcess` with `probe`, `start`, `terminate`, `list_pairings`, and `revoke_pairing` asynchronous completion APIs.
- Consumes: embedded helper at `NSBundle.mainBundle.bundlePath/Contents/MacOS/syncd`.
- Consumes: bundled framework at `NSBundle.mainBundle.privateFrameworksPath/Syphon.framework`.
- Produces: bundle identifier `io.noisefactor.sync`, `LSUIElement=true`, minimum macOS 13.

- [ ] **Step 1: Add failing process tests**

Use temporary executable fixtures to prove arguments contain the exact bundled Syphon path, stderr is drained, a two-second management timeout terminates the command, JSON pairings parse only normalized origins, and Quit never signals an external PID.

- [ ] **Step 2: Run the focused test and confirm failure**

Run: `cmake --build build-acceptance --target sync_companion_process_tests -j 8`

Expected: target/source missing.

- [ ] **Step 3: Implement `CompanionProcess`**

Use `NSTask`, `NSPipe`, `NSURLSession`, and `NSJSONSerialization`. Health requests use a 500 ms request timeout. Management tasks use a two-second termination deadline. All completions return to the main queue; stderr reads remain off the main queue.

- [ ] **Step 4: Implement `Sync.app` menu behavior**

Create an `NSStatusItem` using the `arrow.triangle.2.circlepath` system symbol. Build the menu from the current model whenever it opens. Use `SMAppService.mainApp` for Launch at Login. Run pairing list/revoke work asynchronously. Copy diagnostics through `NSPasteboard`. Show the one-time first-launch explanation via `NSUserDefaults`. On application termination, stop only the owned child with a two-second deadline.

- [ ] **Step 5: Configure the macOS target**

Create the `sync_menu` target only under `APPLE`, link AppKit/Foundation/ServiceManagement, set output name `Sync`, and configure `Info.plist` with the injected version.

- [ ] **Step 6: Run process/model/native tests and an unsigned local launch**

Run:

```bash
cmake -S . -B build-acceptance -DSYNC_PRODUCT_VERSION=0.2.0
cmake --build build-acceptance -j 8
ctest --test-dir build-acceptance --output-on-failure
```

Expected: all tests pass; running `build-acceptance/sync_menu` produces a menu item without a Dock icon and exits without leaving `syncd`.

- [ ] **Step 7: Commit the companion application**

```bash
git add CMakeLists.txt native/src/platform/macos/companion_process.hpp native/src/platform/macos/companion_process.mm native/src/platform/macos/app_main.mm native/test/macos/companion_process_test.mm packaging/macos/Info.plist.in
git commit -m "feat: add Sync macOS menu companion"
```

### Task 4: Deterministic self-contained app and DMG packaging

**Files:**
- Create: `packaging/macos/Sync.svg`
- Create: `packaging/macos/Third-Party-Notices.txt`
- Create: `scripts/package-macos.sh`
- Create: `scripts/verify-macos-bundle.sh`
- Create: `scripts/smoke-macos-app.sh`
- Create: `test/packaging/macos-package.test.js`
- Modify: `CMakeLists.txt`
- Modify: `README.md`

**Interfaces:**
- Produces: `sync_macos_bundle` target at `<build>/package/Sync.app`.
- Produces: `sync_macos_dmg` target at `<build>/package/Sync-<version>-arm64.dmg`.
- Consumes: `SYNC_SYPHON_FRAMEWORK_PATH` and `dylibbundler`.
- Produces: verification failure if a Mach-O dependency references Homebrew, a runner workspace, or another absolute non-system path.

- [ ] **Step 1: Write a failing packaging contract test**

Assert expected bundle paths, exact `CFBundleIdentifier`, `LSUIElement`, version, license/notice files, executable modes, Syphon framework, and absence of forbidden `otool -L` prefixes.

- [ ] **Step 2: Run it against the absent bundle**

Run: `node --test test/packaging/macos-package.test.js`

Expected: fail because `Sync.app` does not exist.

- [ ] **Step 3: Add deterministic resources and icon generation**

Check in a branded SVG and the full Syphon license for the pinned commit. Generate the iconset with `rsvg-convert` and `sips`, then produce `Sync.icns` with `iconutil`.

- [ ] **Step 4: Assemble and fix the app bundle**

Copy `Sync`, `syncd`, Syphon, licenses, and icon into a clean staging directory. Use `dylibbundler` to copy and rewrite non-system dependencies. Reject unresolved absolute paths with `verify-macos-bundle.sh`.

- [ ] **Step 5: Build the unsigned DMG**

Create a staging folder with `Sync.app`, an Applications symlink, `LICENSE.txt`, and `Third-Party-Notices.txt`; call `hdiutil create -format UDZO`. Do not sign or notarize locally in this target.

- [ ] **Step 6: Run packaging and lifecycle smoke tests**

Run:

```bash
cmake -S . -B build-package -DSYNC_PRODUCT_VERSION=0.2.0 -DSYNC_SYPHON_FRAMEWORK_PATH=/absolute/path/to/Syphon.framework
cmake --build build-package --target sync_macos_dmg -j 8
node --test test/packaging/macos-package.test.js
scripts/smoke-macos-app.sh build-package/package/Sync.app
```

Expected: bundle verification passes, `/health` reports bundled Syphon available, and quitting leaves no child.

- [ ] **Step 7: Document local packaging and commit**

```bash
git add CMakeLists.txt README.md packaging/macos/Sync.svg packaging/macos/Third-Party-Notices.txt scripts/package-macos.sh scripts/verify-macos-bundle.sh scripts/smoke-macos-app.sh test/packaging/macos-package.test.js
git commit -m "build: package Sync as a macOS app"
```

### Task 5: Public allowlisted installer route

**Repository:** `/Users/aayars/platform/scaffold`

**Files:**
- Modify: `apps/desktop-downloads/lib/installers.py`
- Modify: `apps/desktop-downloads/server.py`
- Modify: `apps/desktop-downloads/tests/test_server.py`
- Modify: `apps/noisedeck-desktop/groundsquirrel.template.json`
- Modify: `docs/runbook/standalone-builds.md`

**Interfaces:**
- Produces: `locate_public_installer("sync-mac-arm64.dmg")` mapping only to `/installers/sync/Sync-arm64.dmg`.
- Produces: public `GET`/`HEAD /public-downloads/sync-mac-arm64.dmg` through `aiohttp.web.FileResponse`.
- Preserves: authenticated personalized ZIP behavior for every `/downloads/*` route.

- [ ] **Step 1: Write failing sidecar tests**

Cover successful anonymous GET/HEAD, `Content-Disposition`, `application/x-apple-diskimage`, `nosniff`, five-minute cache control, byte range response, missing staged file, unknown filename, traversal input, and unchanged subscription enforcement on `/downloads/noisedeck-mac-arm64.zip`.

- [ ] **Step 2: Run focused tests and confirm failure**

Run: `pytest -q apps/desktop-downloads/tests/test_server.py`

Expected: public route tests receive 404.

- [ ] **Step 3: Implement the exact public mapping and handler**

Use a separate immutable map and route. Do not reuse the personalized license builder. Apply the same trusted-proxy boundary as the existing backend, then return `web.FileResponse` with fixed headers.

- [ ] **Step 4: Run the entire sidecar suite**

Run: `pytest -q apps/desktop-downloads/tests`

Expected: all existing and new tests pass.

- [ ] **Step 5: Update the template and runbook**

Add an explicit public override for `/public-downloads/*`, describe the stable Sync path, atomic staging, cache policy, and live verification commands.

- [ ] **Step 6: Commit exact Scaffold files**

```bash
git add apps/desktop-downloads/lib/installers.py apps/desktop-downloads/server.py apps/desktop-downloads/tests/test_server.py apps/noisedeck-desktop/groundsquirrel.template.json docs/runbook/standalone-builds.md
git commit -m "feat(downloads): serve the public Sync preview"
```

### Task 6: Sync downloads card and release metadata updater

**Repository:** `/Users/aayars/platform/scaffold`

**Files:**
- Modify: `apps/changelist-generator/products.yml`
- Modify: `apps/noisedeck-desktop/content/index.html`
- Modify: `apps/noisedeck-desktop/bin/update-downloads.mjs`
- Modify: `apps/noisedeck-desktop/lib/update-downloads.mjs`
- Modify: `apps/noisedeck-desktop/test/update-downloads.test.mjs`

**Interfaces:**
- Produces: optional `--version` argument updating only a matching `span.product-version[data-product]`.
- Produces: final Sync card with `data-product="sync"` markers and public DMG URL.
- Preserves: weekly Electron build plan because Sync has no `desktop` configuration.

- [ ] **Step 1: Write failing updater and registry tests**

Assert that `version: "Preview 0.2.17"` updates only the Sync card, HTML escaping remains correct, a missing card is reported, and the products registry includes Sync without adding it to the desktop matrix.

- [ ] **Step 2: Run the focused tests and confirm failure**

Run:

```bash
node --test apps/noisedeck-desktop/test/update-downloads.test.mjs
pytest -q apps/changelist-generator/tests/test_config.py apps/changelist-generator/tests/test_plan.py
```

Expected: version-marker tests fail and Sync is absent from the registry.

- [ ] **Step 3: Extend the updater and add the card**

Add optional version replacement without affecting existing callers. Add page copy explaining that paid apps remain account-gated while the MIT Sync preview is public. Add one Mac Apple Silicon CTA and Preview warning.

- [ ] **Step 4: Run page, changelist, and markup checks**

Run:

```bash
node --test apps/noisedeck-desktop/test/update-downloads.test.mjs
pytest -q apps/changelist-generator/tests
node apps/noisedeck-desktop/bin/update-downloads.mjs --help || true
```

Expected: tests pass and a fixture update leaves non-Sync cards byte-identical.

- [ ] **Step 5: Commit exact Scaffold files**

```bash
git add apps/changelist-generator/products.yml apps/noisedeck-desktop/content/index.html apps/noisedeck-desktop/bin/update-downloads.mjs apps/noisedeck-desktop/lib/update-downloads.mjs apps/noisedeck-desktop/test/update-downloads.test.mjs
git commit -m "feat(downloads): add the Sync preview card"
```

### Task 7: Dedicated signed and notarized Sync release workflow

**Repository:** `/Users/aayars/platform/scaffold`

**Files:**
- Create: `.github/workflows/build-sync-preview.yml`
- Create: `scripts/verify-sync-preview-release.mjs`
- Create: `scripts/verify-sync-preview-release.test.mjs`
- Modify: `docs/runbook/deploy-pipelines.md`
- Modify: `docs/runbook/standalone-builds.md`

**Interfaces:**
- Consumes: required `sync_sha` matching `^[0-9a-f]{40}$` and reachable from Sync `main`.
- Produces: signed/notarized `Sync-0.2.<run>-arm64.dmg`, `Sync-arm64.dmg`, and `SHA256SUMS`.
- Produces: atomic remote stage at `/home/deploy/desktop-installers/sync/Sync-arm64.dmg`.
- Produces: downloads-page commit/deploy only after public artifact verification.

- [ ] **Step 1: Write failing static workflow tests**

Parse the YAML and assert exact SHA validation, ARM64 runner, pinned Syphon commit, existing certificate/notary secret names, hardened-runtime signing order, notarize/staple/Gatekeeper checks, atomic `.tmp` upload, remote checksum comparison, public HTTP checksum comparison, and delayed page update.

- [ ] **Step 2: Run and confirm failure**

Run: `node --test scripts/verify-sync-preview-release.test.mjs`

Expected: workflow file missing.

- [ ] **Step 3: Implement build and verification jobs**

Use `macos-latest`, checkout exact Sync SHA, verify ancestry, build pinned Syphon with signing disabled, install runtime/build dependencies, run all Sync tests, package the app, import the certificate using the existing keychain commands, sign inside-out, build/sign/notarize/staple the DMG, mount it, verify the app, launch it, and require healthy bundled Syphon.

- [ ] **Step 4: Implement atomic deploy and publication job**

Download the notarized artifact on Ubuntu, compute SHA-256, SCP a temporary file, atomically rename it, compare remote SHA-256, download through the public route, compare again, then update the Sync card version/date. Commit only the page, rebase normally, push, dispatch `deploy-noisedeck-desktop.yml`, and verify the live page and link.

- [ ] **Step 5: Run workflow static tests and repository CI checks**

Run:

```bash
node --test scripts/verify-sync-preview-release.test.mjs
ruby -e 'require "yaml"; YAML.load_file(".github/workflows/build-sync-preview.yml"); puts "yaml ok"'
git diff --check
```

Expected: all pass.

- [ ] **Step 6: Document operation and rollback**

Document the exact dispatch command, required secrets, how to verify notarization and the live SHA, and rollback by atomically restoring the prior stable DMG and prior page commit. Do not delete retained artifacts.

- [ ] **Step 7: Commit exact Scaffold files**

```bash
git add .github/workflows/build-sync-preview.yml scripts/verify-sync-preview-release.mjs scripts/verify-sync-preview-release.test.mjs docs/runbook/deploy-pipelines.md docs/runbook/standalone-builds.md
git commit -m "build: add signed Sync preview releases"
```

### Task 8: Noisedeck installation documentation

**Repository:** `/Users/aayars/platform/noisedeck`

**Files:**
- Modify: `app/docs/Sync.md`
- Modify: `tests/sync-output.node-test.js`

**Interfaces:**
- Consumes: live `https://downloads.noisefactor.io/public-downloads/sync-mac-arm64.dmg`.
- Preserves: Preview warning and source-build instructions.

- [ ] **Step 1: Add a failing documentation contract**

Assert that the Sync guide contains the public HTTPS DMG URL, Apple Silicon/macOS 13 requirement, menu-bar launch behavior, bundled Syphon statement, source-build alternative, and “not ready for general use” warning.

- [ ] **Step 2: Run and confirm failure**

Run: `node --test tests/sync-output.node-test.js`

Expected: installer-link assertion fails.

- [ ] **Step 3: Update the guide**

Make download/install/launch the primary path. Keep CLI pairing management and build-from-source content as advanced troubleshooting.

- [ ] **Step 4: Run Noisedeck verification**

Run:

```bash
node --test --test-reporter=dot 'tests/**/*.node-test.js'
npm run lint
```

Expected: all tests and lint pass.

- [ ] **Step 5: Commit exact Noisedeck files**

```bash
git add app/docs/Sync.md tests/sync-output.node-test.js
git commit -m "docs(sync): link the installable macOS preview"
```

### Task 9: Review, push, release, and live proof

**Files:** No new implementation files unless review or release evidence finds a defect.

**Interfaces:**
- Produces: public notarized preview and evidence tying source SHA, workflow artifact, remote checksum, live page, and in-app documentation together.

- [ ] **Step 1: Run final local verification**

Run the full Sync native/browser/integration/package suites, full affected Scaffold sidecar/changelist/page/workflow tests, Noisedeck node suite/lint, and `git diff --check` in all three repositories. Record exact totals and any platform-limited skips.

- [ ] **Step 2: Request a code review and address only validated findings**

Review product behavior, process ownership, token/origin leakage, public-route traversal/auth bypass, code-signing order, notarization provenance, atomic publication, tests, and docs. Re-run affected suites after fixes.

- [ ] **Step 3: Fetch and verify fast-forward push safety**

For each repository, fetch the remote and require `git rev-list --left-right --count HEAD...@{u}` to show no remote-only commits before pushing. Preserve all unrelated Scaffold WIP.

- [ ] **Step 4: Push Sync, Scaffold, and Noisedeck existing branches**

Use ordinary pushes only. Do not force-push or open pull requests.

- [ ] **Step 5: Deploy the public sidecar route**

Dispatch the existing desktop-downloads build/deploy workflow for the pushed Scaffold SHA. Verify the running image and `/up`; the new public URL may return 503 until the installer is staged but must not return an authentication redirect.

- [ ] **Step 6: Dispatch the Sync preview release**

```bash
gh workflow run build-sync-preview.yml -R noisefactorllc/scaffold -f sync_sha=<40-char-sync-main-sha>
```

Wait for completion. If signing, notarization, packaging smoke, checksum, or publication fails, diagnose and fix forward; do not publish the card manually.

- [ ] **Step 7: Verify public delivery independently**

Download the live DMG and SHA, compare checksums, verify content type/disposition/range behavior, mount the DMG, copy the app to a temporary directory, run `codesign --verify --deep --strict`, `spctl --assess`, launch it, verify `/health` reports the released version and Syphon available, then quit and prove no child remains.

- [ ] **Step 8: Verify the live page and Noisedeck preview docs**

Confirm the downloads page shows the Sync Preview card and its public link, and the Noisedeck preview deployment serves the updated guide. Distinguish workflow success from live metadata/source SHA.

- [ ] **Step 9: Report delivery with exact evidence**

Report commit SHAs, workflow URLs, notarization/Gatekeeper results, public URL and SHA-256, live deployment metadata, test totals, supported platform, and explicit preview limitations.
