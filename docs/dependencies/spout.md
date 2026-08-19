# Spout runtime integration

Sync discovers `SpoutLibrary.dll` dynamically at runtime on Windows. The
native code does not import Spout headers or link `SpoutLibrary.dll` (or any
Spout import library) at build time -- the only contact point is the
documented C export `GetSpout()`, resolved with `GetProcAddress` after a
bounded, deduplicated `LoadLibraryExW` search (see
`native/src/platform/windows/spout_publisher.cpp`). Absence of the DLL, a
failed `GetSpout` probe, or a failed `CreateOpenGL` is never an error:
`SpoutFramePublisher::available()` reports false and the provider is simply
not offered, the same shape `syphon_consumer.mm` established on macOS.

Spout (the "Spout2" SDK, https://github.com/leadedge/Spout2) is
BSD-2-Clause, and that license explicitly permits redistributing binary
copies -- unlike NDI, whose SDK license forbids redistributing the runtime
(see `docs/superpowers/specs/2026-08-19-sync-spout-ndi-windows-design.md`
section 2). Sync's Windows installer therefore ships a pinned
`SpoutLibrary.dll` in the application directory, discovered as search
location (b) below. Source-only daemon builds may continue to supply another
compatible DLL at runtime via `--spout-library`.

**Pinned revision:** `TODO(release-engineer): record the exact SpoutLibrary
release/commit this build's SpoutLibrary.dll was built from, and the
SHA-256 of the shipped binary, before the first installer build. Mirror
syphon.md's precedent of naming an exact upstream revision -- do not leave
this as a floating "latest" dependency.`

## Discovery boundary

Bounded to at most 4 candidates (mirroring `kMaximumDiscoveryPaths` in both
`syphon_consumer.mm` and `spout_publisher.cpp`), tried in order, first
success wins:

1. The explicit path from `--spout-library`, if the caller passed one.
2. The directory containing the running executable (`GetModuleFileNameW`),
   where the installer places its pinned copy.
3. The bare name `SpoutLibrary.dll`, letting the loader apply its own safe
   default search directories.

Every load goes through `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`
(plus `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` for the absolute-path candidates),
never the legacy, flag-less `LoadLibraryW` search order. Loading a module
executes its code, and the legacy order includes the process's current
working directory, which is attacker-influenced (an "open with" launch from
an arbitrary folder, a dropped file, a working directory inherited from an
untrusted launcher). No user-writable directory is ever added to the search
set -- the same reasoning `syphon_consumer.mm` records for its own
discovery list.

## ABI surface depended on

Sync depends on exactly one exported symbol, `GetSpout`, and a fixed prefix
of the vtable it returns:

```c++
extern "C" SPOUTAPI SPOUTHANDLE WINAPI GetSpout(VOID);
// where SPOUTHANDLE is `SPOUTLIBRARY*` -- a pointer to a stable,
// pure-virtual C++ interface published precisely so that applications do
// not need to compile Spout's sources.
```

Sync calls seven methods on the returned interface: `SetSenderName`,
`SendImage`, `ReleaseSender`, `IsInitialized`, `GetName`, `CreateOpenGL`,
`CloseOpenGL`, and `Release`. Because the real `SpoutLibrary.h` cannot be
vendored into this repository (see the vendor boundary in `CLAUDE.md`),
`spout_publisher.cpp` declares a local mirror of the `SPOUTLIBRARY` vtable's
*shape*: correctly-ordered placeholder slots for every intervening method it
does not call, so the ones it does call land at the right offset.

The mirror is a mechanical transcription of the upstream declaration, not a
guess. It has 172 slots, `SetSenderName` at index 0 and `Release` at index
171, and each spacer is named after the method whose slot it occupies so the
list can be diffed against the upstream header line for line.

### The build-configuration assumption

`SpoutLibrary.h` wraps six adapter-preference methods (indices 145-150) in
`#ifdef NTDDI_WIN10_RS4`. That macro is defined by every Windows SDK from
10.0.17134 (2018) onward simply by including `sdkddkver.h`, so every
realistic build of the DLL contains them. A DLL built without them would
have a 166-slot vtable in which `CreateOpenGL`, `CloseOpenGL`, and `Release`
sit six slots earlier, and calling index 171 on it would invoke an unrelated
function with the wrong arguments.

Two things keep that assumption honest:

1. The installer ships a **pinned** `SpoutLibrary.dll` built by the Noise
   Factor release workflow, so the shipped configuration is controlled
   rather than discovered.
2. `probe_abi()` reads the vtable entry at index 171 and refuses the module
   unless it points inside the module's own image, which a 166-slot vtable's
   would not. It then exercises the early, unconditional slots behaviourally.
   A module that fails the probe is never called through any late slot,
   including `Release`.

`--spout-library` is therefore a developer escape hatch pointed at a
known-good build, not a general compatibility mechanism.

### Still to confirm against a real install

Frame orientation. `SendImage`'s `bInvert` is passed `false`, because Sync's
protocol guarantees top-down rows and a DXGI texture's row 0 is likewise its
top row, which is also why `bInvert` defaults to `false` upstream (unlike
`SendTexture`, which defaults to `true` for OpenGL's bottom-left origin). No
unit test can confirm orientation; check one real receiver.

## License

The public `SpoutLibrary.h` interface this integration is based on is
distributed under the BSD 2-Clause license. Its copyright and redistribution
notice, from the top of `SpoutLibrary.h` in the Spout2 SDK, is preserved
below:

> SpoutLibrary.dll
>
> Spout SDK dll compatible with any C++ compiler
>
> Copyright (c) 2016-2025, Lynn Jarvis. All rights reserved.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are
> met:
>
> 1. Redistributions of source code must retain the above copyright notice,
>    this list of conditions and the following disclaimer.
>
> 2. Redistributions in binary form must reproduce the above copyright
>    notice, this list of conditions and the following disclaimer in the
>    documentation and/or other materials provided with the distribution.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
> AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
> IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
> ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
> LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
> CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
> SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
> INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
> CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
> ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
> POSSIBILITY OF SUCH DAMAGE.

When the installer ships the pinned `SpoutLibrary.dll`, this notice must
also be preserved verbatim in the Windows installer's third-party notices
file, the same way `syphon.md` requires for
`packaging/macos/Third-Party-Notices.txt`.
