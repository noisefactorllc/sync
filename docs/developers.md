# Sync developer guide

Sync lets a web application publish rendered frames to native video tools on the same computer.
The browser SDK supports direct RGBA bytes, Canvas 2D, WebGL2, and WebGPU.

The SDK package is `@noisefactor/sync` 0.2.0.
The SDK and native companion have separate versions.

## Install the SDK

Install the released tarball in your application:

```bash
npm install https://github.com/noisefactorllc/sync/releases/download/sdk-v0.2.0/noisefactor-sync-0.2.0.tgz
```

You can then import from `@noisefactor/sync`.
This command installs the GitHub release asset directly.
It does not require an npm account.

For direct browser imports, download the [modules ZIP](https://github.com/noisefactorllc/sync/releases/download/sdk-v0.2.0/sync-sdk-0.2.0-modules.zip).
Extract `modules/` into your application's static assets.
Import `index.js` from that directory.
Keep the complete directory because its files use relative imports.
The [release page](https://github.com/noisefactorllc/sync/releases/tag/sdk-v0.2.0) includes SHA-256 checksums for both downloads.

## Build the SDK locally

Build the distribution from the repository root:

```bash
npm run package:sdk
```

The command creates these versioned files:

```text
dist/sdk/0.2.0/modules/
dist/sdk/0.2.0/noisefactor-sync-0.2.0.tgz
dist/sdk/0.2.0/SHA256SUMS
```

Install the tarball from the path that applies to your application:

```bash
npm install /absolute/path/to/sync/dist/sdk/0.2.0/noisefactor-sync-0.2.0.tgz
```

You can then import from `@noisefactor/sync`.
You can also copy `modules/` into your application's static assets.
Import `index.js` from the copied directory when you use this vendored option.

Keep the complete module directory.
Its files use relative imports between the client, protocol, diagnostics, and queue modules.

## Start a compatible companion

The SDK does not install or update the native Sync companion.
SDK 0.2.0 targets native preview 0.2.66.
That companion version adds third-party `app://` origins and native statistics.
Check the [download page](https://sync.noisedeck.app/#download) for available installers.

For source testing, build and run the daemon from the same source checkout.
Follow the [native build instructions](../README.md#building-the-native-daemon).

## Pair from a user action

Serve this response header from the top-level application:

```http
Permissions-Policy: loopback-network=(self)
```

A cross-origin parent must name the child origin in its response header:

```http
Permissions-Policy: loopback-network=(self "https://visuals.example")
```

The parent must also add `allow="loopback-network"` to the application iframe.

Call `pair()` directly from a deliberate user action.
Sync shows the application name and exact origin before it creates a token.

```js
import { SyncBridgeClient } from '@noisefactor/sync';

connectButton.addEventListener('click', async () => {
  const pairingClient = new SyncBridgeClient();
  const { token } = await pairingClient.pair('My visual app');
  pairingClient.close();

  const client = new SyncBridgeClient({ token });
  await client.connect();
});
```

The pairing client does not store the token.
It does not put the token in a URL or a WebSocket subprotocol.
The application owns any token persistence.
Create a new client with the approved token.

`probe()` and `connect()` are passive operations.
They return or throw a permission result instead of starting pairing.

The default daemon endpoint is `http://127.0.0.1:53979`.
The `endpoint` option also accepts an explicit IPv4 or IPv6 loopback URL with a port.
It rejects remote hosts, credentials, paths, queries, and fragments.

## Configure a sender

Each sender needs this descriptor:

```js
const descriptor = {
  width: 1280,
  height: 720,
  format: 'rgba8unorm',
  colorSpace: 'srgb',
  alphaMode: 'opaque',
  fps: 60,
};
```

The width and height must be integers from 1 through 4096.
The frame payload must not exceed 64 MiB.
The `format` value is `rgba8unorm`.
The `colorSpace` value is `srgb` or `display-p3`.
The `alphaMode` value is `opaque`, `straight`, or `premultiplied`.
The `fps` value must be positive and finite.

Call `configure()` before the first submission.
Call it again after each output-size or color change.

```js
sender.configure(descriptor);
sender.submit(source, performance.now());
```

The timestamp uses the same clock as `performance.now()`.
The SDK encodes `Math.round((performance.timeOrigin + timestamp) * 1000)` microseconds.

`submit()` never waits for the native receiver.
It returns `true` when the queue accepts the source.
It returns `false` after a queue, transport, or backpressure drop.
Read `sender.stats` for local accepted, sent, busy, backpressure, and failed counters.

Local `sent` means that the browser passed the frame to its WebSocket.
Native `accepted` means that the daemon providers accepted the frame.
Neither counter proves that a receiving application showed the frame.

The application owns the render loop and each source resource.
The export queue only borrows a source during `enqueue()`.
Each queue emits top-down RGBA bytes in a reusable `Uint8Array`.
The frame callback borrows these bytes until it returns.

## Direct RGBA

`createRgbaSender()` creates an `RgbaExportQueue` for you.
It uses `maxBufferedFrames: 2` when you do not supply a buffer limit.

```js
const sender = await client.createRgbaSender('My RGBA output');
sender.configure(descriptor);

sender.submit({
  width: descriptor.width,
  height: descriptor.height,
  rowStride: descriptor.width * 4,
  data: rgbaBytes,
}, performance.now());
```

`data` can be an `ArrayBuffer` or an array view.
`rowStride` must contain at least four bytes for each pixel.
The queue copies each used row before `submit()` returns.
The caller can then change or release the source bytes.

## Canvas 2D

Create the queue with the configured source canvas:

```js
const exportQueue = new CanvasExportQueue({ canvas });
const sender = await client.createSender('My canvas', {
  exportQueue,
  maxBufferedFrames: 2,
});
sender.configure(descriptor);

function render(timestamp) {
  drawCanvas(timestamp);
  sender.submit(canvas, timestamp);
  requestAnimationFrame(render);
}
```

The source canvas dimensions must equal the descriptor dimensions.
The queue copies through a separate 2D canvas and emits top-down RGBA bytes.
It applies the selected alpha mode to the copied output.
This queue performs a synchronous 2D readback for each accepted submission.
Measure its render-loop cost at your target size and frame rate.

## WebGL2

Create the queue with the WebGL2 context:

```js
const exportQueue = new WebGL2ExportQueue({ gl, slots: 3 });
const sender = await client.createSender('My WebGL output', {
  exportQueue,
  maxBufferedFrames: 2,
});
sender.configure(descriptor);

function render(timestamp) {
  renderWebGL();
  sender.submit(null, timestamp);
  requestAnimationFrame(render);
}
```

The `slots` value can be an integer from 1 through 8.
The default is 3.
Pass `null` to read the default framebuffer.
Its drawing-buffer dimensions must equal the descriptor dimensions.
You can also pass a complete framebuffer object with a color attachment.
The queue restores the framebuffer, read-buffer, pixel-pack buffer, and pack state after each submission.
It flips the WebGL rows into top-down order.
It does not convert color spaces or alpha representation.
Set the descriptor to match the bytes in the WebGL source.

Treat a `webglcontextlost` event as a sender loss.
Stop the sender and release your renderer resources.

## WebGPU

The source texture must include `GPUTextureUsage.COPY_SRC`.
It must be a single 2D texture without multisampling.
Its dimensions must equal the descriptor dimensions.

The queue accepts `rgba8unorm`, `rgba8unorm-srgb`, `bgra8unorm`, and `bgra8unorm-srgb` textures.
It converts BGRA data to RGBA during readback.
It does not convert color spaces or alpha representation.
Set the descriptor to match the bytes in the WebGPU texture.

```js
context.configure({
  device,
  format,
  alphaMode: 'opaque',
  usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC,
});

const exportQueue = new WebGPUExportQueue({ device, slots: 3 });
const sender = await client.createSender('My WebGPU output', {
  exportQueue,
  maxBufferedFrames: 2,
});
sender.configure(descriptor);

function render(timestamp) {
  const texture = context.getCurrentTexture();
  renderWebGPU(texture);
  sender.submit(texture, timestamp);
  requestAnimationFrame(render);
}
```

The `slots` value can be an integer from 1 through 8.
The default is 3.
The device must support the padded readback buffer size.
Treat `device.lost` and uncaptured errors as sender failures.

## Low-level queue contract

`SyncFrameSink` accepts a WebSocket, an export queue, one buffer limit, and a clock.
Most applications can use `SyncBridgeClient.createSender()` instead.

A custom queue exposes these members:

```text
configure(descriptor)
enqueue(source, timestamp, onFrame, sequence)
poll()
available
close({ backendLost })
```

`enqueue()` returns `true` only when it accepts the source.
It calls `onFrame(frame, timestamp, sequence)` when bytes are ready.
GPU queues call the callback during `poll()`.
The sender calls `poll()` from each `submit()` call.

Set exactly one pressure limit when you call `createSender()`.
Use a positive `maxBufferedFrames` value or a positive `maxBufferedBytes` value.
The queue can also reject work while all readback slots are busy.

## Statistics and diagnostics

`await sender.getStats()` returns native counters:

```js
{
  accepted,
  dropped,
  rejected,
  failed,
  lastSequence,
  lastPresentationTimeUs,
  checksum,
}
```

The six counters are safe nonnegative JavaScript integers.
The checksum is a 16-digit lowercase hexadecimal string.

`createDiagnosticSnapshot({ client, sender, descriptor, error })` combines safe client and native fields.
It includes SDK, daemon, protocol, provider, frame, and counter information.
It excludes credentials, error messages, and error causes.

## Stop and restart

Stop the caller-owned render loop before you close the sender.
Then close the sender and wait for native cleanup:

```js
cancelAnimationFrame(animationFrame);
try {
  sender.close();
  await sender.closed;
} finally {
  client.close();
}
```

`sender.closed` rejects with `SyncSenderLostError` after an unexpected data-socket loss.
The error can include the WebSocket close code and a bounded reason.

Pass `{ backendLost: true }` to `sender.close()` when the renderer backend cannot release queue resources safely.
Release all other resources that your application owns.

## Public exports

SDK version 0.2.0 exports:

- `SyncBridgeClient` and `SYNC_DEFAULT_ENDPOINT`
- `SyncFrameSink`
- `RgbaExportQueue`
- `CanvasExportQueue`
- `WebGL2ExportQueue`
- `WebGPUExportQueue`
- `createDiagnosticSnapshot`
- `SYNC_SDK_VERSION`
- `encodeFrameV1` and `decodeFrameHeaderV1`
- `PIXEL_FORMAT`, `COLOR_SPACE`, and `ALPHA_MODE`
- `SYNC_ERROR_CODE`
- `SyncBridgeError` and the typed Sync error subclasses

`SYNC_SDK_VERSION` is `0.2.0`.
The SDK version and the companion product version are independent.

## Platform outputs

One sender can appear through each selected and available provider.

| Platform | Native outputs |
| --- | --- |
| macOS | Syphon, optional NDI, and Sync Camera |
| Windows | Spout, optional NDI, and Sync Camera on Windows 11 |
| Ubuntu 24.04 | V4L2 Sync Camera and optional NDI |

NDI needs an operator-installed runtime.
The browser SDK does not select or install native providers.

## Errors and framework integration

Client, control, pairing, and sender-lifecycle errors extend `SyncBridgeError`.
These errors include a stable `code`.
Queue and adapter validation can also throw standard `TypeError` and `RangeError` objects.
Handle these groups in the application UI:

| Error group | Application response |
| --- | --- |
| Permission required or denied | Ask the user to use the explicit Connect control or browser settings. |
| Pairing denied, busy, or timed out | Keep output stopped and let the user start a new attempt. |
| Authentication | Remove the stored token and require a new explicit pairing action. |
| Capability | Show that no selected native send provider is available. |
| Sender lost | Stop the render submission and release renderer resources. |
| Protocol or configuration | Report the exact stable error code and fix the integration. |

Create the client and sender in browser-only lifecycle code.
Do not access browser globals during server-side rendering.
Keep one sender controller outside the component render function.
Submit after the framework completes the renderer's frame.
On resize, resize the source first and then call `sender.configure()`.
On component cleanup, stop animation, close the sender, await `sender.closed`, and close the client.

The protocol accepts canonical HTTPS origins, loopback HTTP origins, and lowercase `app://` origins.
Read the [origin rules](protocol-v1.md#origins) before you use a custom application scheme.

Read [protocol v1](protocol-v1.md) before you implement the wire protocol directly.
Use the SDK when your environment supports browser modules.
The [complete example](../examples/) includes Canvas 2D, WebGL2, WebGPU, diagnostics, resize, loss, stop, and restart behavior.
