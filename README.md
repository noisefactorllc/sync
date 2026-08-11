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
- an Apple Silicon menu-bar companion with bounded helper supervision; and
- native, browser, protocol, security-boundary, and real-loopback tests.

The macOS companion is a preview and is not ready for general use. Windows/
Spout, NDI, reverse-direction native sources, and automatic updates are not
part of the current public implementation.

## Building the native daemon

Sync requires CMake 3.20 or newer, a C++20 compiler, OpenSSL 3, libuv, and
pkg-config. macOS builds also use the system Foundation and Metal frameworks.

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

The daemon discovers Syphon dynamically rather than linking against it. The
installable companion supplies a pinned framework inside its private bundle.
See [docs/dependencies/syphon.md](docs/dependencies/syphon.md) for the runtime
and license boundary.

## Packaging the macOS preview

Packaging requires macOS 13 or newer, an Apple Silicon build, `dylibbundler`,
`librsvg`, and a locally built `Syphon.framework`. The release workflow pins
Syphon source revision `71351d4b484cd2d1917867f7846a5cdca724552d`; use that
same revision for local release-equivalent packages.

```bash
cmake -S . -B build-package \
  -DSYNC_PRODUCT_VERSION=0.2.0 \
  -DSYNC_SYPHON_FRAMEWORK_PATH=/absolute/path/to/Syphon.framework
cmake --build build-package --target sync_macos_dmg -j4
SYNC_PACKAGE_DIR=build-package/package \
  node --test test/packaging/macos-package.test.js
scripts/smoke-macos-app.sh "$PWD/build-package/package/Sync.app"
```

The local target creates an unsigned app and DMG. Developer ID signing,
notarization, stapling, and public publication belong to the Noise Factor
release workflow so credentials never enter this public repository.

## Browser SDK

The dependency-free browser modules live in [`browser/`](browser/). Passive
discovery never initiates pairing; `pair()` must be called from a deliberate
user action, and the host application owns returned token storage. See the
[browser client guide](browser/README.md) for the API and loopback Permissions
Policy requirements.

```js
import { SyncBridgeClient } from './browser/index.js'

const pairingClient = new SyncBridgeClient()
const { token } = await pairingClient.pair('My visual app')
pairingClient.close()

const sync = new SyncBridgeClient({ token })
await sync.connect()
```

## Tests

```bash
npm run test:browser
SYNC_DAEMON_PATH=build/syncd npm run test:integration
ctest --test-dir build --output-on-failure
```

## Security

Unknown origins cannot silently publish. Pairing requires a browser-initiated
request and a visible native approval prompt; reusable credentials are scoped
to an exact normalized origin and can be revoked. Please report suspected
vulnerabilities privately using [SECURITY.md](SECURITY.md).

## License

Sync is released under the [MIT License](LICENSE). See
[TRADEMARK.md](TRADEMARK.md) for the branding boundary. Third-party runtime
providers retain their own licenses and are not relicensed by this repository.

Copyright © 2026 Noise Factor LLC
