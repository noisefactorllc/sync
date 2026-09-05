# Sync review — 2026-09-05

Reviewed the separate checkout from baseline `7ca2dae` on `main`. Repairs are
contained in Sync; sibling Noisemaker, Noisedeck, and Scaffold sources were
consulted for interoperability and release behavior. The installed companion
was left running. The initial review did not start a release or remote workflow.

## Findings addressed

| Area | Failure | Repair and evidence |
| --- | --- | --- |
| Browser export resources | A data-socket failure was treated as GPU backend destruction. Noisemaker then abandoned live export slots without destroying their textures, buffers, or fences. | Transport failures now use ordinary resource cleanup. Close/error regressions failed with zero slots destroyed before the fix and pass with all three destroyed once. Explicit backend loss still abandons resources as intended. Also checked against the actual sibling Noisemaker export queue. |
| macOS companion lifetime | Destruction after helper exit changed immutable NSTask stdio properties and raised an exception. Once that was removed, an already queued termination callback dereferenced freed supervisor state, confirmed by ASan. | Queued callbacks use a weak ownership record, destruction disarms callbacks, and post-launch stdio mutation is removed. Tests cover destruction before queue drain, destruction from an exit callback, and queued termination with no helper. Existing terminate/relaunch ordering tests still pass. |
| NDI alpha | Premultiplied pixels were copied unchanged into NDI's straight-alpha RGBA format; opaque mode also preserved an ignored alpha byte. | Normalize into the existing owned buffer. Four production-conversion tests cover packed/padded rows, straight and opaque modes, zero/full/tiny alpha, rounding, saturation, unchanged input, and destination bounds. Three failed before normalization. No new frame allocations. |
| Linux camera colors | NV12 bytes use BT.709, but an unspecified V4L2 colorspace lets v4l2loopback choose sRGB and its default BT.601 YCbCr interpretation. | Declare 709 primaries and matrix, limited range, and sRGB transfer using the extended-format contract. The regression runs real encoder bytes through metadata-based matrix, range, and transfer interpretation, including saturated and neutral patches. Negotiated color drift is rejected. |
| Windows RGB camera levels | RGB32 copied full-range BGRA bytes while declaring limited range. | Advertise full range for RGB32 and retain limited range for NV12. Added a media-type regression. Windows native CI passed on `315c00f`. |
| Windows camera activation | ShutdownObject did nothing; after DetachObject, ActivateObject permanently failed. | Serialize cached-source ownership, create sources lazily, shut down and release on ShutdownObject, and detach without shutting down the caller's source. Regressions check fresh usable activation after both operations. Windows native CI passed on `315c00f`. |
| Soak process lifecycle | Missing executables, malformed/oversized readiness, invalid ports, or silent children could escape promise handling or leave the caller waiting indefinitely. | Bound and validate readiness; stop the child before rejecting. Both soak programs share this lifecycle. The long runner emits an error plus final JSONL summary even when startup fails. Real-child regressions demonstrated failures before the repair. |
| Soak WebSocket lifecycle | Closed/silent/failed upgrades and stalled writes could hang; failed upgrades retained sockets; fragmented headers accumulated listeners; coalesced first messages were not drained. | Bound connection/upgrade/write waits and header size, clean up failed sockets and temporary listeners, and drain handshake remainder immediately. Ten real TCP tests cover failure and successful wire-byte behavior; eight reproduced original failures. |
| Soak scheduling and verdict | Tiny frames could starve timers: a five-second run lasted until a sixty-second sender cycle. A final pending health failure could arrive after a successful verdict. | Periodically yield to the event loop, enforce the short run's elapsed-time bound, and join issued health checks before shutdown/verdict. Real-daemon regressions cover duration and delayed health failure. |
| CI coverage | Meta, soak, and acceptance unit suites were not invoked by CI. Objective-C++ compilation did not receive the macOS warning/sanitizer matrix flags. | Add a combined unit script to each platform job, pass the Windows daemon path to native subprocess tests, and apply the macOS flags to Objective-C++. |
| Local verification and documentation | Two loopback cases assumed the installed companion's port was free. The README claimed CLI management breaks a live pairing store, which current code no longer does. | Preserve a running local companion, explicitly skip only the occupied default-port case outside CI, and retain strict CI behavior. Added a live-daemon regression proving repeated listing and selective revocation preserve other pairings. Removed the stale warning and corrected the CMake prerequisite. |

## Review coverage

Examined browser discovery, permissions, pairing, session cancellation, sender
recovery, frame encoding, resource ownership, and backpressure; native
WebSocket/control/frame parsing, authentication, origin normalization, pairing
storage and locks, authority cancellation, payload budgets, callbacks, and
publisher ownership; camera, NDI, Spout, Metal/Syphon and companion lifecycle
paths; CMake, platform packaging, CI/release routing, and soak/acceptance gates.

No additional reproducible native transport/authentication defect was
established. That is a review result, not a formal security certification.

## Verification

Local build directories and detailed logs are ignored build artifacts, retained
under `build-review*`. The final evidence logs are in `build-review/review-logs`.

- macOS C++ and Objective-C++ with `-Wall -Wextra -Wpedantic -Werror`: all
  21 CTest suites passed.
- macOS with address and undefined-behavior sanitizers on both languages:
  all 21 CTest suites passed, including the companion lifetime regressions.
- Ubuntu 24.04 ARM64 warning-clean build: all 18 CTest suites passed.
- Ubuntu 24.04 x86_64 warning-clean Release build: all 18 CTest suites passed.
- Ubuntu ARM64 ASan/UBSan with leak detection: all 18 CTest suites passed;
  the final NDI conversion change additionally passed its rebuilt sanitizer
  suite.
- Real loopback: 23/23 passed in isolated Linux. On macOS, 22 passed and the
  default-port case was explicitly skipped because the installed companion
  owns port 53979. Linux `syncctl` daemon-control integration also passed.
- Node 22: all 187 combined meta/browser/soak/acceptance unit tests passed on
  both macOS and isolated Linux, including native subprocess regressions.
- Packaging source checks: 14 passed; seven artifact/platform-specific cases
  are explicitly skipped without their required artifacts. The separate real
  Linux package test passed against the built `.deb`.
- Built an x86_64 `.deb`, verified its contents and architecture, and ran its
  install/remove/purge smoke test in a disposable container. This intentionally
  does not install or exercise a kernel camera driver.
- ShellCheck, shell syntax, actionlint, and whitespace checks passed. The
  actionlint invocation allows the existing custom `sync-camera` runner label.
- Short real-daemon runs exercised lifecycle cycling, memory sampling, healthy
  shutdown, tiny-frame timer fairness, and delayed health failure. These are
  bounded functional checks, not a new multi-hour soak certification.

Representative reproduction commands, from the checkout root:

```sh
cmake -S . -B build-review -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-Wall -Wextra -Wpedantic -Werror' \
  -DCMAKE_OBJCXX_FLAGS='-Wall -Wextra -Wpedantic -Werror'
cmake --build build-review --parallel 4
ctest --test-dir build-review --output-on-failure
SYNC_DAEMON_PATH=build-review/syncd npm run test:unit
SYNC_DAEMON_PATH=build-review/syncd npm run test:integration
npm run test:packaging
SYNC_DAEMON_PATH=build-review/syncd SYNC_SOAK_SECONDS=10 \
  SYNC_SOAK_CYCLE=2 npm run test:soak
shellcheck --severity=warning scripts/*.sh packaging/linux/postrm
actionlint -ignore 'label "sync-camera" is unknown' .github/workflows/*.yml
```

## Independent review follow-up

A fresh reviewer examined `7ca2dae..341a1c1` and found one important correction:
REC709's default transfer function did not describe the unchanged sRGB-encoded
channels. The original matrix-only regression missed the resulting brightness
shift. The extended regression failed against that implementation, as did new
checks for unsupported extended metadata and incompatible negotiated color.
The repair declares sRGB transfer with the 709 matrix and limited range, checks
extended-format capability, sets the required `priv` magic, and validates the
returned format. The reviewer examined this correction and reported no
remaining findings.

The reviewer also confirmed a minor test guard defect: an explicit missing
`SYNC_DAEMON_PATH` skipped both native soak regressions with exit zero. It now
fails with a clear path assertion; an absent optional default build can still
skip. Verified both the rejected path and the two successful real-daemon tests.

Fresh follow-up verification passed all 21 macOS native suites in both warning
and sanitizer configurations, and all 18 native suites on Ubuntu ARM64 and
x86_64. The changed Linux camera device and sink suites also passed ASan/UBSan
with leak detection. Detailed evidence is retained alongside the original logs.

Integrated upstream `06e0c62` on `main`, preserving protocol-soak geometry
rotation together with the startup bound and timer fairness corrections. The
reviewer separately approved the constructor conflict resolution. The combined
Node 22 unit suite passed 190/190 on macOS and Linux; Linux loopback passed
23/23 and the syncctl integration passed 1/1. An eight-second real-daemon run
completed three geometry changes and three session cycles with zero dropped
frames or reconnects, exit zero, and confirmed child reaping. This run verifies
integration behavior, not sustained performance or memory-growth certification.

### CI follow-up

The first authorized push, `315c00f`, passed all Linux CI jobs, Windows native
Release and warning builds (19 suites each), Windows package verification and
installed-app lifecycle smoke, and the dedicated Windows camera end-to-end gate.
The overall CI run failed in new JavaScript regressions, exposing two harness
assumptions that local unsanitized runs had missed.

Synchronous memory inspection stalled the short soak's network writes long
enough to trip daemon header/frame deadlines. The sampler now uses bounded
asynchronous reads, keeps one sample in flight, joins both inspectors even on
failure, and excludes reads crossing the end of streaming. The macOS lifecycle
unit fixture uses real RSS because vmmap pauses busy targets and explicitly
does not support ASan heaps; it does not certify physical-footprint growth.
The final-health fixture uses deterministic fast metrics with a real daemon
and HTTP request/body. Removing both health joins now makes this regression
fail, proving incidental inspection time cannot mask the missing wait.

The Windows loopback stack could buffer the old fixture's entire 32 MiB write.
The timeout regression now corks a real socket to hold its write callback,
then verifies timeout rejection and destruction. Removing the timeout makes
that regression fail. The final local suite passed 190/190 against an ASan
macOS daemon and 190/190 on Linux after these corrections.

Upstream `ef4ef28` then added generator-aware daemon discovery and enabled the
real-child startup fixtures on Windows. It retained the explicit-path failure
guard and received a clean independent review. Its CI passed all macOS/Linux
jobs, Windows native tests and packaging, and the dedicated camera gate, but
exposed a remaining Windows inspection limit: PowerShell was terminated by the
new two-second async deadline before producing a reading. Windows async reads
now have a ten-second bound. A successful 2.2-second command reproduced the
old timeout locally and completes with the corrected budget.

Increasing that bound alone cannot guarantee a completed warm sample within
a five-second run. The Windows timer regression therefore uses deterministic
metrics, retaining the real daemon, stream, health requests, and shutdown.
A separate Windows test invokes the real resident/private inspectors against
an idle daemon, requires positive finite readings, rejects private-memory
fallback, and joins both commands before cleanup. Physical memory-growth
acceptance remains a separate sustained run.

## Remaining acceptance boundaries

Windows native builds/tests, packaging/lifecycle checks, and the dedicated
camera end-to-end gate passed remotely at `315c00f`; the later follow-up only
changes JavaScript verification and documentation. No signed/notarized macOS
bundle or physical Spout, NDI, or V4L2 receiver certification was performed in
this task. NDI normalization tests verify production copy bytes without loading
the vendor runtime. The actual Noisedeck A → Sync Camera → Noisedeck B pixel
path remains a separate acceptance requirement.

A push to public `main` can request a preview release through Scaffold after
green CI. The user subsequently authorized pushing after review feedback is
addressed. The upstream protocol geometry-rotation change was integrated and
verified before the push.

## External contracts checked

- [NDI frame types](https://docs.ndi.video/all/developing-with-ndi/sdk/frame-types):
  RGBA uses straight alpha.
- [V4L2 extended-format contract](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/pixfmt-v4l2.html):
  capability and `priv` magic are required for independent transfer metadata.
- [V4L2 colorspace defaults](https://kernel.org/doc/html/v6.15/userspace-api/media/v4l/colorspaces-details.html)
  and [v4l2loopback source](https://github.com/v4l2loopback/v4l2loopback/blob/v0.13.2/v4l2loopback.c):
  declared colorspace controls the default YCbCr matrix.
- [Media Foundation nominal range](https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/ne-mfobjects-mfnominalrange):
  full and limited range have distinct numeric interpretations.
- [ShutdownObject](https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imfactivate-shutdownobject)
  and [DetachObject](https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imfactivate-detachobject):
  both release the activator's cached reference and allow fresh activation;
  only shutdown stops the existing object.
