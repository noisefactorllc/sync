# Sync browser SDK

`SyncBridgeClient` connects a web renderer to the local Sync companion.
The default endpoint is `http://127.0.0.1:53979`.
You can supply an explicit IPv4 or IPv6 loopback endpoint for development.

The local SDK candidate is `@noisefactor/sync` 0.2.0.
The local installation procedures do not publish the package to npm.
Read the [repository developer guide](https://github.com/noisefactorllc/sync/blob/main/docs/developers.md) for tarball and vendored-module procedures.
The SDK does not install the native companion.
For candidate testing, run the daemon from the same source checkout.

## Pair and connect

Passive `probe()` and `connect()` calls do not start pairing.
Call `pair(name)` from a deliberate user action, such as a click.
The browser can show its local network prompt during this call.
The Sync companion then shows the exact origin in a separate approval prompt.

The SDK returns the approved token to the caller.
It does not store or log the token.
It does not put the token in a URL or a WebSocket subprotocol.

```js
import { SyncBridgeClient } from './browser/index.js';

pairButton.addEventListener('click', async () => {
  const pairingClient = new SyncBridgeClient();
  const { token } = await pairingClient.pair('My visual app');
  pairingClient.close();

  const sync = new SyncBridgeClient({ token });
  await sync.connect();
});
```

The example keeps the token in memory.
Use your application's credential policy if you persist it.
Create a new client with the token after pairing.

## Permissions Policy

Send this HTTP response header from a top-level web application:

```http
Permissions-Policy: loopback-network=(self)
```

The top-level page must name a cross-origin child in its response header:

```http
Permissions-Policy: loopback-network=(self "https://visuals.example")
```

The page must also delegate the feature to the intended frame:

```html
<iframe src="https://visuals.example/app" allow="loopback-network"></iframe>
```

The SDK queries the legacy `local-network-access` permission only when the browser does not know `loopback-network`.
The legacy name is a compatibility path.

## Create a sender

`createRgbaSender()` creates a direct RGBA queue and uses a two-frame buffer limit by default.
Its source has `width`, `height`, `rowStride`, and `data` fields.

```js
const sender = await sync.createRgbaSender('My output');
sender.configure({
  width: 1280,
  height: 720,
  format: 'rgba8unorm',
  colorSpace: 'srgb',
  alphaMode: 'opaque',
  fps: 60,
});

sender.submit({
  width: 1280,
  height: 720,
  rowStride: 1280 * 4,
  data: rgbaBytes,
}, performance.now());
```

`createSender()` accepts a caller-selected export queue.
You must set exactly one `maxBufferedFrames` or `maxBufferedBytes` limit.
The Canvas, WebGL2, and WebGPU queue constructors are:

```js
new CanvasExportQueue({ canvas });
new WebGL2ExportQueue({ gl, slots: 3 });
new WebGPUExportQueue({ device, slots: 3 });
```

Call `sender.configure(descriptor)` after each output-size change.
Call `sender.submit(source, performance.now())` from your render loop.
The application continues to own the render loop and every source resource.

GPU queues complete readbacks during `poll()`.
`sender.submit()` calls `poll()` before it admits the next source.
Each callback receives a top-down RGBA `Uint8Array`.
The callback borrows that array until it returns.
The queue can reuse the array after the callback returns.

Canvas export performs a synchronous 2D readback on each accepted submission.
Measure that cost at your target size and rate.

The GPU queues preserve the source color and alpha bytes.
They do not convert color spaces, premultiply alpha, or remove premultiplication.
Set the descriptor to match the bytes that your renderer produces.

See the [repository examples](https://github.com/noisefactorllc/sync/tree/main/examples) for Canvas 2D, WebGL2, and WebGPU source code.

## Stop and diagnose

Local counters are available through `sender.stats`.
Native counters are available through `await sender.getStats()`.
The native checksum is a 16-digit lowercase hexadecimal string.
Local `sent` means that the browser passed bytes to its WebSocket.
Native `accepted` means that the daemon providers accepted a frame.
Neither counter proves that a receiving application showed the frame.

```js
try {
  const nativeStats = await sender.getStats();
  console.log(nativeStats);
} finally {
  try {
    sender.close();
    await sender.closed;
  } finally {
    sync.close();
  }
}
```

`sender.closed` rejects if the data connection ends unexpectedly.
The rejection is a `SyncSenderLostError` with a bounded close code and reason.

Use `createDiagnosticSnapshot({ client, sender, descriptor, error })` to create a safe local report.
The snapshot excludes token values, error messages, and error causes.

## API reference

The public module exports these main interfaces:

- `SyncBridgeClient`
- `SyncFrameSink`
- `RgbaExportQueue`
- `CanvasExportQueue`
- `WebGL2ExportQueue`
- `WebGPUExportQueue`
- `createDiagnosticSnapshot`
- `SYNC_SDK_VERSION`
- protocol helpers and enums
- typed Sync error classes and `SYNC_ERROR_CODE`

Read the [repository developer guide](https://github.com/noisefactorllc/sync/blob/main/docs/developers.md) for queue constraints.
Read the [protocol v1 reference](https://github.com/noisefactorllc/sync/blob/main/docs/protocol-v1.md) for the wire format.
