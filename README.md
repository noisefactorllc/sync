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
- a macOS Metal publisher and dynamically discovered Syphon integration; and
- native, browser, protocol, security-boundary, and real-loopback tests.

Signed desktop packaging, Windows/Spout, NDI, and reverse-direction native
sources are not part of the current public implementation.

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

Syphon is discovered dynamically rather than linked or bundled. See
[docs/dependencies/syphon.md](docs/dependencies/syphon.md) for the runtime and
license boundary.

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
