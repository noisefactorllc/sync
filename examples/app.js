import {
  CanvasExportQueue,
  SyncBridgeClient,
  WebGL2ExportQueue,
  WebGPUExportQueue,
  SYNC_ERROR_CODE,
  createDiagnosticSnapshot,
} from '../browser/index.js';

const APP_NAME = 'Sync Interop Visualizer';
const SENDER_NAME = 'Sync Interop Visualizer';
const FPS = 60;

const elements = {
  connect: document.querySelector('#connect'),
  diagnostics: document.querySelector('#diagnostics'),
  mode: document.querySelector('#mode'),
  stats: document.querySelector('#stats'),
  status: document.querySelector('#status'),
  stop: document.querySelector('#stop'),
  surface: document.querySelector('#surface'),
};

let active = null;
let changing = false;
let generation = 0;
let lastError = null;
let pending = null;
let approvedToken = null;
let pageEpoch = 0;

function clientOptions() {
  const endpoint = new URL(location.href).searchParams.get('endpoint');
  return endpoint === null ? {} : { endpoint };
}

function descriptor(width, height) {
  return {
    width,
    height,
    format: 'rgba8unorm',
    colorSpace: 'srgb',
    alphaMode: 'opaque',
    fps: FPS,
  };
}

function surfaceSize(canvas, maximum = 4096) {
  const scale = Math.min(devicePixelRatio || 1, 2);
  const width = Math.min(maximum, Math.max(2, Math.round(canvas.clientWidth * scale)));
  const height = Math.min(maximum, Math.max(2, Math.round(canvas.clientHeight * scale)));
  return { width, height };
}

function makeCanvas() {
  const canvas = document.createElement('canvas');
  elements.surface.replaceChildren(canvas);
  return canvas;
}

function canvasRenderer() {
  const canvas = makeCanvas();
  const context = canvas.getContext('2d', { alpha: true });
  if (!context) throw new Error('Canvas 2D is unavailable.');
  return {
    exportQueue: new CanvasExportQueue({ canvas }),
    resize() {
      const size = surfaceSize(canvas);
      canvas.width = size.width;
      canvas.height = size.height;
      return descriptor(size.width, size.height);
    },
    render(time) {
      const { width, height } = canvas;
      const phase = time * 0.0004;
      const gradient = context.createLinearGradient(0, 0, width, height);
      gradient.addColorStop(0, `hsl(${(phase * 180) % 360} 85% 52%)`);
      gradient.addColorStop(1, `hsl(${(phase * 180 + 150) % 360} 85% 38%)`);
      context.fillStyle = gradient;
      context.fillRect(0, 0, width, height);
      context.fillStyle = '#10100dcc';
      for (let column = -1; column < 8; column += 1) {
        const x = ((column + phase) % 8) * width / 6;
        context.fillRect(x, 0, Math.max(4, width / 18), height);
      }
      context.fillStyle = '#f5f4ef';
      context.font = `700 ${Math.max(18, width / 18)}px system-ui`;
      context.fillText('SYNC', width * 0.07, height * 0.86);
      return canvas;
    },
    dispose() {
      canvas.width = 0;
      canvas.height = 0;
      canvas.remove();
    },
  };
}

function compileShader(gl, type, source) {
  const shader = gl.createShader(type);
  if (!shader) throw new Error('WebGL2 could not create a shader.');
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const message = gl.getShaderInfoLog(shader) || 'WebGL2 shader compilation failed.';
    gl.deleteShader(shader);
    throw new Error(message);
  }
  return shader;
}

function webgl2Renderer(onLost) {
  const canvas = makeCanvas();
  const gl = canvas.getContext('webgl2', { alpha: false, antialias: false });
  if (!gl) throw new Error('WebGL2 is unavailable.');
  const vertex = compileShader(gl, gl.VERTEX_SHADER, `#version 300 es
    const vec2 points[3] = vec2[3](vec2(-1.0,-1.0), vec2(3.0,-1.0), vec2(-1.0,3.0));
    void main() { gl_Position = vec4(points[gl_VertexID], 0.0, 1.0); }
  `);
  const fragment = compileShader(gl, gl.FRAGMENT_SHADER, `#version 300 es
    precision highp float;
    uniform vec2 resolution;
    uniform float time;
    out vec4 color;
    void main() {
      vec2 uv = gl_FragCoord.xy / resolution;
      float bars = step(0.48, fract((uv.x + uv.y * 0.25 + time * 0.08) * 8.0));
      vec3 a = 0.5 + 0.5 * cos(time + uv.xyx * 5.0 + vec3(0.0, 2.1, 4.2));
      color = vec4(mix(a * 0.28, a, bars), 1.0);
    }
  `);
  const program = gl.createProgram();
  if (!program) throw new Error('WebGL2 could not create a program.');
  gl.attachShader(program, vertex);
  gl.attachShader(program, fragment);
  gl.linkProgram(program);
  gl.deleteShader(vertex);
  gl.deleteShader(fragment);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    const message = gl.getProgramInfoLog(program) || 'WebGL2 program linking failed.';
    gl.deleteProgram(program);
    throw new Error(message);
  }
  const resolution = gl.getUniformLocation(program, 'resolution');
  const time = gl.getUniformLocation(program, 'time');
  const lost = (event) => {
    event.preventDefault();
    onLost(new Error('The WebGL2 context was lost.'));
  };
  canvas.addEventListener('webglcontextlost', lost);
  return {
    exportQueue: new WebGL2ExportQueue({ gl, slots: 3 }),
    resize() {
      const size = surfaceSize(canvas);
      canvas.width = size.width;
      canvas.height = size.height;
      return descriptor(size.width, size.height);
    },
    render(timestamp) {
      gl.viewport(0, 0, canvas.width, canvas.height);
      gl.useProgram(program);
      gl.uniform2f(resolution, canvas.width, canvas.height);
      gl.uniform1f(time, timestamp * 0.001);
      gl.drawArrays(gl.TRIANGLES, 0, 3);
      return null;
    },
    dispose() {
      canvas.removeEventListener('webglcontextlost', lost);
      if (!gl.isContextLost()) gl.deleteProgram(program);
      canvas.width = 0;
      canvas.height = 0;
      canvas.remove();
    },
  };
}

async function webgpuRenderer(onLost) {
  if (!navigator.gpu) throw new Error('WebGPU is unavailable.');
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) throw new Error('WebGPU could not find an adapter.');
  const device = await adapter.requestDevice();
  const canvas = makeCanvas();
  const context = canvas.getContext('webgpu');
  if (!context) {
    device.destroy();
    throw new Error('WebGPU could not create a canvas context.');
  }
  const format = navigator.gpu.getPreferredCanvasFormat();
  const module = device.createShaderModule({ code: `
    struct Scene { size: vec2f, time: f32, pad: f32 }
    @group(0) @binding(0) var<uniform> scene: Scene;
    @vertex fn vertex(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
      var points = array<vec2f, 3>(vec2f(-1,-1), vec2f(3,-1), vec2f(-1,3));
      return vec4f(points[index], 0, 1);
    }
    @fragment fn fragment(@builtin(position) position: vec4f) -> @location(0) vec4f {
      let uv = position.xy / scene.size;
      let bands = step(0.48, fract((uv.x + uv.y * 0.25 + scene.time * 0.08) * 8.0));
      let wave = 0.5 + 0.5 * cos(scene.time + vec3f(uv.x, uv.y, uv.x) * 5.0 + vec3f(0,2.1,4.2));
      return vec4f(mix(wave * 0.28, wave, bands), 1);
    }
  ` });
  const pipeline = device.createRenderPipeline({
    layout: 'auto',
    vertex: { module, entryPoint: 'vertex' },
    fragment: { module, entryPoint: 'fragment', targets: [{ format }] },
    primitive: { topology: 'triangle-list' },
  });
  const uniform = device.createBuffer({
    size: 16,
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
  });
  const bindGroup = device.createBindGroup({
    layout: pipeline.getBindGroupLayout(0),
    entries: [{ binding: 0, resource: { buffer: uniform } }],
  });
  let lost = false;
  const deviceLost = device.lost.then((info) => {
    lost = true;
    onLost(new Error(`The WebGPU device was lost: ${info.message || info.reason}.`), true);
  });
  deviceLost.catch(() => {});
  const uncaptured = (event) => onLost(event.error ?? new Error('WebGPU reported an uncaptured error.'), false);
  device.addEventListener('uncapturederror', uncaptured);
  let maximum = Math.min(4096, device.limits.maxTextureDimension2D);
  return {
    exportQueue: new WebGPUExportQueue({ device, slots: 3 }),
    resize() {
      const size = surfaceSize(canvas, maximum);
      canvas.width = size.width;
      canvas.height = size.height;
      context.configure({
        device,
        format,
        alphaMode: 'opaque',
        usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC,
      });
      return descriptor(size.width, size.height);
    },
    render(timestamp) {
      const source = context.getCurrentTexture();
      device.queue.writeBuffer(uniform, 0, new Float32Array([
        canvas.width, canvas.height, timestamp * 0.001, 0,
      ]));
      const encoder = device.createCommandEncoder();
      const pass = encoder.beginRenderPass({
        colorAttachments: [{
          view: source.createView(),
          clearValue: { r: 0.02, g: 0.02, b: 0.02, a: 1 },
          loadOp: 'clear',
          storeOp: 'store',
        }],
      });
      pass.setPipeline(pipeline);
      pass.setBindGroup(0, bindGroup);
      pass.draw(3);
      pass.end();
      device.queue.submit([encoder.finish()]);
      return source;
    },
    dispose({ backendLost = false } = {}) {
      device.removeEventListener('uncapturederror', uncaptured);
      if (!backendLost && !lost) {
        context.unconfigure();
        uniform.destroy();
        device.destroy();
      }
      canvas.width = 0;
      canvas.height = 0;
      canvas.remove();
    },
  };
}

function updateControls() {
  elements.connect.disabled = changing;
  elements.connect.textContent = active ? 'Restart' : 'Connect';
  elements.stop.disabled = changing || !active;
  elements.diagnostics.disabled = !active;
  elements.mode.disabled = changing;
}

function cleanupAttempt(attempt, { backendLost = false } = {}) {
  if (!attempt) return;
  const pairingClient = attempt.pairingClient;
  const sender = attempt.sender;
  const client = attempt.client;
  const renderer = attempt.renderer;
  attempt.pairingClient = null;
  attempt.sender = null;
  attempt.client = null;
  attempt.renderer = null;
  try { pairingClient?.close(); } catch {}
  try { sender?.close({ backendLost }); } catch {}
  client?.close();
  renderer?.dispose({ backendLost });
}

function showStats(stats = {}) {
  const dropped = (stats.droppedBusy ?? 0) + (stats.droppedBackpressure ?? 0);
  elements.stats.textContent = `accepted ${stats.accepted ?? 0} · sent ${stats.sent ?? 0} · dropped ${dropped} · failed ${stats.failed ?? 0}`;
}

async function stop({ backendLost = false, message = 'Stopped.' } = {}) {
  const attempt = pending;
  pending = null;
  if (attempt) {
    attempt.canceled = true;
    cleanupAttempt(attempt, { backendLost });
  }
  const session = active;
  active = null;
  generation += 1;
  if (!session) {
    updateControls();
    return;
  }
  cancelAnimationFrame(session.animationFrame);
  try {
    session.sender.close({ backendLost });
    await session.sender.closed;
  } catch (error) {
    if (!lastError) lastError = error;
  }
  session.client.close();
  session.renderer.dispose({ backendLost });
  elements.status.textContent = message;
  updateControls();
}

async function fail(sessionGeneration, error, backendLost = false) {
  if (!active || active.generation !== sessionGeneration) return;
  lastError = error;
  await stop({ backendLost, message: error.message || 'The output stopped.' });
}

function render(session) {
  if (active !== session) return;
  try {
    const source = session.renderer.render(performance.now());
    session.sender.submit(source, performance.now());
    showStats(session.sender.stats);
    session.animationFrame = requestAnimationFrame(() => render(session));
  } catch (error) {
    void fail(session.generation, error);
  }
}

function resize(session) {
  if (active !== session) return;
  try {
    session.descriptor = session.renderer.resize();
    session.sender.configure(session.descriptor);
  } catch (error) {
    void fail(session.generation, error);
  }
}

async function connect() {
  if (changing) return;
  changing = true;
  updateControls();
  const connectEpoch = pageEpoch;
  await stop({ message: 'Restarting.' });
  if (connectEpoch !== pageEpoch) {
    changing = false;
    updateControls();
    return;
  }
  const sessionGeneration = ++generation;
  const attempt = {
    canceled: false,
    client: null,
    generation: sessionGeneration,
    pairingClient: null,
    renderer: null,
    sender: null,
  };
  pending = attempt;
  const requireCurrent = () => {
    if (pending !== attempt || attempt.canceled || generation !== sessionGeneration) {
      throw new DOMException('The start operation was canceled.', 'AbortError');
    }
  };
  try {
    if (approvedToken === null) {
      elements.status.textContent = 'Approve this app in Sync.';
      attempt.pairingClient = new SyncBridgeClient(clientOptions());
      const paired = await attempt.pairingClient.pair(APP_NAME);
      requireCurrent();
      approvedToken = paired.token;
      attempt.pairingClient.close();
      attempt.pairingClient = null;
    }

    attempt.client = new SyncBridgeClient({ ...clientOptions(), token: approvedToken });
    await attempt.client.connect();
    requireCurrent();
    const onLost = (error, backendLost = true) => void fail(sessionGeneration, error, backendLost);
    if (elements.mode.value === 'canvas') attempt.renderer = canvasRenderer();
    if (elements.mode.value === 'webgl2') attempt.renderer = webgl2Renderer(onLost);
    if (elements.mode.value === 'webgpu') attempt.renderer = await webgpuRenderer(onLost);
    requireCurrent();
    if (!attempt.renderer) throw new Error('Select a renderer.');

    attempt.sender = await attempt.client.createSender(SENDER_NAME, {
      exportQueue: attempt.renderer.exportQueue,
      maxBufferedFrames: 2,
    });
    requireCurrent();
    const session = {
      animationFrame: 0,
      client: attempt.client,
      descriptor: null,
      generation: sessionGeneration,
      renderer: attempt.renderer,
      sender: attempt.sender,
    };
    active = session;
    pending = null;
    resize(session);
    session.sender.closed.catch((error) => void fail(sessionGeneration, error, false));
    elements.status.textContent = `Sending ${elements.mode.selectedOptions[0].text}.`;
    lastError = null;
    render(session);
  } catch (error) {
    const canceled = attempt.canceled || error?.name === 'AbortError';
    lastError = error;
    if (error?.code === SYNC_ERROR_CODE.AUTHENTICATION) approvedToken = null;
    if (pending === attempt) pending = null;
    cleanupAttempt(attempt);
    if (!canceled) {
      elements.status.textContent = error?.code === SYNC_ERROR_CODE.AUTHENTICATION
        ? 'The approval is no longer valid. Click Connect to pair again.'
        : error.message || 'Sync could not start.';
    }
  } finally {
    if (pending === attempt) pending = null;
    changing = false;
    updateControls();
  }
}

elements.connect.addEventListener('click', () => void connect());
elements.stop.addEventListener('click', () => void stop());
elements.diagnostics.addEventListener('click', async () => {
  if (!active) return;
  const snapshot = await createDiagnosticSnapshot({
    client: active.client,
    sender: active.sender,
    descriptor: active.descriptor,
    error: lastError,
  });
  try {
    await navigator.clipboard.writeText(JSON.stringify(snapshot, null, 2));
    elements.status.textContent = 'Diagnostics copied.';
  } catch {
    elements.status.textContent = 'The browser did not allow clipboard access.';
  }
});
window.addEventListener('resize', () => {
  if (active) resize(active);
});
window.addEventListener('pagehide', () => {
  pageEpoch += 1;
  void stop();
});
updateControls();
