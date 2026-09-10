// Compile this consumer against an installed SDK package with strict DOM types.
import {
  SyncBridgeClient, SyncFrameSink, RgbaExportQueue, CanvasExportQueue,
  WebGL2ExportQueue, WebGPUExportQueue, createDiagnosticSnapshot,
  encodeFrameV1, decodeFrameHeaderV1, SYNC_SDK_VERSION,
  type FrameDescriptor, type Sender,
} from '@noisefactor/sync';

const descriptor: FrameDescriptor = {
  width: 2, height: 1, format: 'rgba8unorm',
  colorSpace: 'srgb', alphaMode: 'straight', fps: 60,
};
const client = new SyncBridgeClient({
  fetch: window.fetch, WebSocket: window.WebSocket,
  permissions: navigator.permissions,
});
const canvas = document.createElement('canvas');
const gl = canvas.getContext('webgl2')!;
const pixels = { width: 2, height: 1, rowStride: 8, data: new Uint8Array(8) };

async function useSdk() {
  const raw = await client.createRgbaSender('Example');
  raw.configure(descriptor);
  raw.submit(pixels, performance.now());
  const rgba = await client.createSender('Bytes', {
    exportQueue: new RgbaExportQueue(), maxBufferedFrames: 2,
  });
  rgba.submit(pixels, performance.now());
  const raster = await client.createSender('Canvas', {
    exportQueue: new CanvasExportQueue({ canvas }), maxBufferedFrames: 2,
  });
  raster.submit(canvas, performance.now());
  const gpu = await client.createSender('WebGL2', {
    exportQueue: new WebGL2ExportQueue({ gl }), maxBufferedFrames: 2,
  });
  gpu.submit(null, performance.now());
  gpu.submit(gl.createFramebuffer(), performance.now());
  const checksum: string = (await raw.getStats()).checksum;
  const snapshot = await createDiagnosticSnapshot({ client, sender: raw, descriptor });
  const version: string = SYNC_SDK_VERSION;
  raw.close();
  const closedId: string = (await raw.closed).id;
  return { checksum, snapshot, version, closedId };
}

const sink = new SyncFrameSink({
  socket: new WebSocket('ws://127.0.0.1:53979/'),
  exportQueue: new RgbaExportQueue(), maxBufferedFrames: 2,
});
sink.configure(descriptor);
sink.submit(pixels, 1);
const metadata = {
  width: 2, height: 1, rowStride: 8, pixelFormat: 1 as const,
  colorSpace: 1 as const, alphaMode: 2 as const, sequence: 1, presentationTimeUs: 1000,
};
const buffer: ArrayBuffer = encodeFrameV1(metadata, pixels.data);
const view: Uint8Array = encodeFrameV1(metadata, pixels.data, new ArrayBuffer(72));
const explicitDefault: ArrayBuffer = encodeFrameV1(metadata, pixels.data, undefined);
declare const optionalDestination: ArrayBuffer | undefined;
const optional: ArrayBuffer | Uint8Array = encodeFrameV1(metadata, pixels.data, optionalDestination);
const sequence: number = decodeFrameHeaderV1(view).sequence;

// The SDK can use a GPU type supplied by the host without another dependency.
interface HostTexture { destroy(): void }
declare const hostDevice: object;
declare const hostTexture: HostTexture;
const queue = new WebGPUExportQueue<HostTexture>({ device: hostDevice });
const typed: Promise<Sender<HostTexture>> = client.createSender('WebGPU', {
  exportQueue: queue, maxBufferedFrames: 2,
});
void typed.then(sender => sender.submit(hostTexture, 1));
void [useSdk, buffer, sequence, explicitDefault, optional];
