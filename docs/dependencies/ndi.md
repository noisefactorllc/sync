# NDI runtime integration

Sync discovers the NDI runtime dynamically at process start, on every
platform it targets (Windows, macOS, Linux). The native code does not
import any NDI SDK header and does not link against the NDI SDK at build
time — `native/src/platform/ndi_publisher.cpp` resolves everything through
`LoadLibraryExW`/`dlopen` plus a single exported C entry point.

**The runtime is not bundled with Sync.** Unlike Syphon (BSD-2-Clause,
redistributed and pinned in `Sync.app`), the NDI SDK licence does not permit
redistributing the NDI runtime. A user who wants an NDI send provider
installs the NDI Runtime (or NDI Tools, which bundles it) themselves, from
the vendor. If no compatible runtime is present, is unsupported on the
current CPU, or fails its initialization probe, `NdiFramePublisher::available()`
is simply `false` and the provider is not offered — absence is never an
error.

## Discovery

`NdiFramePublisher` builds a bounded, deduplicated list of at most 4
candidate library paths, in this order, and stops at the first one that
loads, resolves, and passes its ABI probe:

1. The directory named by `Options::runtime_path` (wired from the
   `--ndi-runtime` CLI flag), if the caller supplied one.
2. The directory named by the `NDI_RUNTIME_DIR_V6` environment variable, if
   set.
3. The directory named by the `NDI_RUNTIME_DIR_V5` environment variable, if
   set.
4. The bare platform library name, deferring entirely to the OS loader's
   own default search path.

`NDI_RUNTIME_DIR_V6` / `NDI_RUNTIME_DIR_V5` are the NDI SDK's own documented
discovery variables — the official NDI Runtime installer sets them. Sync
adds no other search location (no per-user or working-directory fallback):
loading a module executes its code, so every directory searched must
already be one this process trusts, exactly the reasoning
`syphon_consumer.mm` documents for `Syphon.framework` discovery on macOS.

Per-platform library file name:

| Platform | File name |
| --- | --- |
| Windows | `Processing.NDI.Lib.x64.dll` |
| macOS | `libndi.dylib` |
| Linux | `libndi.so.5` |

On Linux, install the vendor runtime so `libndi.so.5` is reachable through its
documented runtime-directory environment variable or the system loader. Avahi
is required for normal mDNS discovery between machines; `syncctl doctor`
reports it separately because an NDI sender can initialize even when discovery
is unavailable. PipeWire is unrelated to NDI and is not used by Sync's send
path.

The Linux provider is an optional compatibility path, not part of the virtual
camera certification gate. Sync never bundles `libndi.so.5`, links against it,
or claims certification by Vizrt; a missing runtime leaves the camera and
control service fully operational.

## The exact ABI surface Sync depends on

Sync resolves one exported C function, `NDIlib_v5_load`, which the SDK
documents in `Processing.NDI.DynamicLoad.h`. It returns a pointer to a
struct of function pointers. Sync cannot include that header (see above), so
`ndi_publisher.cpp` declares a **hand-reproduced, minimal prefix** of that
struct: the first 52 fields, in the vendor's documented order, from
`initialize` through `send_send_video_async_v2`. Fields Sync does not call
are declared as opaque, correctly-sized `void*` placeholders named after the
SDK field they stand in for, so the offsets of the fields Sync *does* call
stay correct without requiring every intervening function's exact
signature.

This is safe against SDK version drift because the vendor's dynamic-load
header defines every versioned struct name (`NDIlib_v2` through the current
`NDIlib_v6`) as a **typedef alias of the same, monotonically-growing
struct** — fields are only ever appended at the end, never reordered or
removed. A 52-field prefix is therefore layout-compatible with whatever
struct an installed v3-or-later runtime actually returns.

This field order was verified (not reconstructed purely from memory) by
fetching and reading the vendor's own MIT-licensed compatibility headers —
`Processing.NDI.DynamicLoad.h`, `Processing.NDI.structs.h`, and
`Processing.NDI.Send.h` — at the time this file was written. Those files
carry their own separate MIT grant for exactly this kind of reproduction;
that grant does not extend to the SDK or runtime itself, which Sync
continues to treat as undistributable per the SDK licence. A reviewer can
re-audit the field order below against a current copy of
`Processing.NDI.DynamicLoad.h` (obtainable from the NDI SDK, or from any of
several MIT-licensed open-source mirrors of that specific file, e.g.
`obs-ndi`/`DistroAV`'s `lib/ndi/Processing.NDI.DynamicLoad.h`).

| # | SDK field | Sync's use |
| --- | --- | --- |
| 1 | `initialize` | ABI probe |
| 2 | `destroy` | shutdown |
| 3 | `version` | declared, currently unused |
| 4 | `is_supported_CPU` | ABI probe |
| 5–8 | `find_create` .. `find_get_sources` | opaque, unused |
| 9 | `send_create` | `open_sender` |
| 10 | `send_destroy` | `close_sender` / destructor |
| 11–21 | `send_send_video` .. `send_set_failover` | opaque, unused |
| 22–35 | `recv_create_v2` .. `recv_get_no_connections` | opaque, unused |
| 36–39 | `routing_create` .. `routing_clear` | opaque, unused |
| 40–47 | `util_send_send_audio_interleaved_16s` .. `util_send_send_audio_interleaved_32f` (v1.5/v2 audio + find blocks) | opaque, unused |
| 48–50 | `recv_free_video_v2`, `recv_free_audio_v2`, `recv_capture_v2` | opaque, unused |
| 51 | `send_send_video_v2` | synchronous flush before destroying a sender |
| 52 | `send_send_video_async_v2` | per-frame publish |

Sync also declares local, field-for-field mirrors of two SDK data structs,
both documented in `Processing.NDI.Send.h` / `Processing.NDI.structs.h`:

- `NDIlib_send_create_t` — `p_ndi_name`, `p_groups`, `clock_video`,
  `clock_audio`.
- `NDIlib_video_frame_v2_t` — `xres`, `yres`, `FourCC`, `frame_rate_N`,
  `frame_rate_D`, `picture_aspect_ratio`, `frame_format_type`, `timecode`,
  `p_data`, the `line_stride_in_bytes`/`data_size_in_bytes` union (collapsed
  to the line-stride meaning, since Sync only ever sends uncompressed
  RGBA), `p_metadata`, `timestamp`.

And two constants: `NDIlib_FourCC_video_type_RGBA` (`NDI_LIB_FOURCC('R',
'G', 'B', 'A')`, i.e. `'R' | ('G'<<8) | ('B'<<16) | ('A'<<24)`, least
byte first) and `NDIlib_frame_format_type_progressive` (`1`).

**Must-verify-against-SDK item:** the `void*` placeholder approach assumes
function pointers and object pointers are the same width on every ABI Sync
targets. That holds for Win64, macOS, and Linux today, but is not a
guarantee the C++ standard makes in general — re-check this assumption
before porting to any exotic target.

## Trademark

NDI® is a registered trademark of Vizrt NDI AB. Sync is an independent
project; this integration is not affiliated with, sponsored by, or endorsed
by Vizrt NDI AB.
