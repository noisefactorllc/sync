// This module runs inside a real browser. It does not replace graphics APIs.
import {
  CanvasExportQueue, RgbaExportQueue, WebGL2ExportQueue, WebGPUExportQueue,
  SyncFrameSink, SyncBridgeClient, createDiagnosticSnapshot,
} from '/browser/index.js';

export const expected = [
  255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 128,
  255, 255, 255, 255, 0, 255, 255, 255, 0, 0, 0, 0,
];
const descriptor = {
  width: 3, height: 2, format: 'rgba8unorm',
  colorSpace: 'srgb', alphaMode: 'straight', fps: 60,
};
const pause = () => new Promise(resolve => setTimeout(resolve, 10));
function check(condition, message) { if (!condition) throw new Error(message); }
function equal(actual, wanted, message) {
  check(JSON.stringify(actual) === JSON.stringify(wanted),
    `${message}: ${JSON.stringify(actual)}`);
}

export async function makeSource(kind) {
  if (kind === 'rgba') {
    const data = new Uint8Array(28);
    data.set(expected.slice(0, 12));
    data.set(expected.slice(12), 16);
    return { queue: new RgbaExportQueue(), source: { width: 3, height: 2, rowStride: 16, data }, close() {} };
  }
  if (kind === 'canvas') {
    const canvas = new OffscreenCanvas(3, 2);
    canvas.getContext('2d', { colorSpace: 'srgb' })
      .putImageData(new ImageData(new Uint8ClampedArray(expected), 3, 2), 0, 0);
    return { queue: new CanvasExportQueue({ canvas }), source: canvas, close() {} };
  }
  if (kind.startsWith('webgl2')) {
    const canvas = new OffscreenCanvas(3, 2);
    const gl = canvas.getContext('webgl2', { alpha: true, premultipliedAlpha: false, preserveDrawingBuffer: true });
    check(gl, 'WebGL2 is unavailable');
    gl.disable(gl.DITHER);
    let source = null;
    let texture = null;
    if (kind === 'webgl2-framebuffer') {
      texture = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, 3, 2);
      source = gl.createFramebuffer();
      gl.bindFramebuffer(gl.FRAMEBUFFER, source);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
    }
    gl.enable(gl.SCISSOR_TEST);
    for (let y = 0; y < 2; y += 1) for (let x = 0; x < 3; x += 1) {
      const offset = (y * 3 + x) * 4;
      gl.scissor(x, 1 - y, 1, 1);
      gl.clearColor(...expected.slice(offset, offset + 4).map(value => value / 255));
      gl.clear(gl.COLOR_BUFFER_BIT);
    }
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    const sentinel = gl.createBuffer();
    gl.bindBuffer(gl.PIXEL_PACK_BUFFER, sentinel);
    gl.pixelStorei(gl.PACK_ALIGNMENT, 8);
    gl.pixelStorei(gl.PACK_ROW_LENGTH, 7);
    gl.pixelStorei(gl.PACK_SKIP_ROWS, 1);
    gl.pixelStorei(gl.PACK_SKIP_PIXELS, 2);
    return {
      queue: new WebGL2ExportQueue({ gl }), source,
      checkState() {
        equal([gl.getParameter(gl.PACK_ALIGNMENT), gl.getParameter(gl.PACK_ROW_LENGTH),
          gl.getParameter(gl.PACK_SKIP_ROWS), gl.getParameter(gl.PACK_SKIP_PIXELS)],
        [8, 7, 1, 2], 'WebGL2 pack state changed');
        check(gl.getParameter(gl.PIXEL_PACK_BUFFER_BINDING) === sentinel, 'WebGL2 buffer binding changed');
        check(gl.getParameter(gl.READ_FRAMEBUFFER_BINDING) === null, 'WebGL2 framebuffer binding changed');
      },
      close() {
        gl.deleteBuffer(sentinel);
        if (source) gl.deleteFramebuffer(source);
        if (texture) gl.deleteTexture(texture);
        gl.getExtension('WEBGL_lose_context')?.loseContext();
      },
    };
  }
  check(navigator.gpu, 'WebGPU is unavailable');
  const adapter = await navigator.gpu.requestAdapter();
  check(adapter, 'A WebGPU adapter is unavailable');
  const device = await adapter.requestDevice();
  const format = kind.slice('webgpu-'.length);
  const source = device.createTexture({
    size: [3, 2], format, usage: GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST,
  });
  const bytes = Uint8Array.from(expected);
  if (format.startsWith('bgra')) for (let i = 0; i < bytes.length; i += 4) {
    const red = bytes[i]; bytes[i] = bytes[i + 2]; bytes[i + 2] = red;
  }
  device.queue.writeTexture({ texture: source }, bytes, { bytesPerRow: 12 }, [3, 2]);
  return {
    queue: new WebGPUExportQueue({ device }), source,
    close() { source.destroy(); device.destroy(); },
  };
}

export async function checkAdapters() {
  const results = [];
  for (const kind of ['rgba', 'canvas', 'webgl2-default', 'webgl2-framebuffer',
    'webgpu-rgba8unorm', 'webgpu-bgra8unorm', 'webgpu-rgba8unorm-srgb', 'webgpu-bgra8unorm-srgb']) {
    const fixture = await makeSource(kind);
    const packets = [];
    const sink = new SyncFrameSink({
      socket: { readyState: 1, bufferedAmount: 0, send: bytes => packets.push(new Uint8Array(bytes).slice()), close() {} },
      exportQueue: fixture.queue, maxBufferedFrames: 2, clock: { timeOrigin: 0 },
    });
    try {
      sink.configure(descriptor);
      check(sink.submit(fixture.source, 1), `${kind} rejected the frame`);
      const deadline = performance.now() + 5000;
      while (!packets.length && performance.now() < deadline) { await pause(); fixture.queue.poll(); }
      check(packets.length === 1, `${kind} did not complete one frame`);
      equal([...packets[0].subarray(64)], expected, `${kind} changed pixels`);
      fixture.checkState?.();
      results.push({ kind, bytesCompared: expected.length, exact: true });
    } finally { sink.close(); fixture.close(); }
  }
  return results;
}

let active;
export async function startNative({ endpoint, token, kind, name }) {
  await stopNative();
  const client = new SyncBridgeClient({ endpoint, token });
  await client.connect();
  const fixture = await makeSource(kind);
  let sender;
  try {
    sender = await client.createSender(name, { exportQueue: fixture.queue, maxBufferedFrames: 2 });
    sender.configure(descriptor);
    const timer = setInterval(() => sender.submit(fixture.source, performance.now()), 16);
    active = { client, fixture, sender, timer, descriptor };
    return { origin: location.origin };
  } catch (error) { fixture.queue.close(); fixture.close(); client.close(); throw error; }
}
export async function stopNative() {
  if (!active) return null;
  const { client, fixture, sender, timer, descriptor } = active;
  active = null;
  clearInterval(timer);
  const diagnostic = await createDiagnosticSnapshot({ client, sender, descriptor });
  sender.close();
  await sender.closed.catch(() => {});
  client.close();
  fixture.close();
  return diagnostic;
}

export async function startMarkers({ endpoint, token, name }) {
  await stopNative();
  const client = new SyncBridgeClient({ endpoint, token });
  await client.connect();
  const sender = await client.createRgbaSender(name);
  const width = 64, height = 32;
  const markerDescriptor = { ...descriptor, width, height };
  sender.configure(markerDescriptor);
  const data = new Uint8Array(width * height * 4);
  const view = new DataView(data.buffer);
  let sequence = 0;
  const timer = setInterval(() => {
    data.fill(++sequence % 256);
    data.set([83, 89, 78, 67]);
    const time = performance.now();
    const timeUs = BigInt(Math.round((performance.timeOrigin + time) * 1000));
    view.setBigUint64(4, BigInt(sequence), true);
    view.setBigUint64(12, timeUs, true);
    view.setBigUint64(20, timeUs, true);
    sender.submit({ width, height, rowStride: width * 4, data }, time);
  }, 16);
  active = { client, sender, timer, descriptor: markerDescriptor, fixture: { close() {} } };
}
