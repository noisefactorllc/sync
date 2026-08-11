import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { assertLatencyAcceptance } from '../browser/latency-acceptance-contract.js';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const NOISEDECK_ROOT = path.resolve(
  process.env.NOISEDECK_ROOT || path.join(ROOT, '..', 'noisedeck'),
);
const NOISEDECK_URL = process.env.NOISEDECK_URL || 'http://127.0.0.1:8000/';
const SYPHON_FRAMEWORK_PATH = process.env.SYPHON_FRAMEWORK_PATH;
const ACCEPTANCE_TOKEN = process.env.SYNC_ACCEPTANCE_TOKEN;
const TRANSPORT_ONLY = process.env.SYNC_ACCEPTANCE_TRANSPORT_ONLY === '1';
const RECEIVER = process.env.SYNC_SYPHON_RECEIVER_PATH ||
  path.join(ROOT, 'build', 'sync_syphon_receiver_probe');
const SENDER_NAME = process.env.SYNC_SENDER_NAME || 'Sync Latency Acceptance';
const DURATION_MS = Number(process.env.SYNC_LATENCY_DURATION_MS || 20_000);
const DISCOVERY_TIMEOUT_MS = Number(process.env.SYNC_DISCOVERY_TIMEOUT_MS || 5_000);
const EXPECTED_WIDTH = Number(process.env.SYNC_EXPECTED_WIDTH || 1024);
const EXPECTED_HEIGHT = Number(process.env.SYNC_EXPECTED_HEIGHT || 1024);
const EXPECTED_FPS = Number(process.env.SYNC_EXPECTED_FPS || 60);
const MINIMUM_OBSERVED_RENDER_FPS = Number(
  process.env.SYNC_MINIMUM_OBSERVED_RENDER_FPS || 55,
);
const MINIMUM_SAMPLES = Number(process.env.SYNC_MINIMUM_LATENCY_SAMPLES || 120);
const MAXIMUM_P95_LATENCY_US = Number(process.env.SYNC_MAXIMUM_P95_LATENCY_US || 66_000);
const HEADFUL = process.env.SYNC_ACCEPTANCE_HEADFUL === '1';
const INCLUDE_SAMPLES = process.env.SYNC_ACCEPTANCE_INCLUDE_SAMPLES === '1';
const BUFFER_LIMIT_OVERRIDE = process.env.SYNC_ACCEPTANCE_MAX_BUFFERED_BYTES === undefined
  ? null
  : Number(process.env.SYNC_ACCEPTANCE_MAX_BUFFERED_BYTES);

if (!TRANSPORT_ONLY && !SYPHON_FRAMEWORK_PATH) {
  throw new Error('SYPHON_FRAMEWORK_PATH must point to Syphon.framework');
}
if (!ACCEPTANCE_TOKEN) {
  throw new Error('SYNC_ACCEPTANCE_TOKEN must match the isolated acceptance daemon token');
}
for (const [name, value] of [
  ['SYNC_LATENCY_DURATION_MS', DURATION_MS],
  ['SYNC_DISCOVERY_TIMEOUT_MS', DISCOVERY_TIMEOUT_MS],
  ['SYNC_EXPECTED_WIDTH', EXPECTED_WIDTH],
  ['SYNC_EXPECTED_HEIGHT', EXPECTED_HEIGHT],
  ['SYNC_EXPECTED_FPS', EXPECTED_FPS],
  ['SYNC_MINIMUM_LATENCY_SAMPLES', MINIMUM_SAMPLES],
  ['SYNC_MAXIMUM_P95_LATENCY_US', MAXIMUM_P95_LATENCY_US],
]) {
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new Error(`${name} must be a positive safe integer`);
  }
}
if (!Number.isFinite(MINIMUM_OBSERVED_RENDER_FPS) ||
    MINIMUM_OBSERVED_RENDER_FPS <= 0) {
  throw new Error('SYNC_MINIMUM_OBSERVED_RENDER_FPS must be positive and finite');
}
if (BUFFER_LIMIT_OVERRIDE !== null &&
    (!Number.isSafeInteger(BUFFER_LIMIT_OVERRIDE) || BUFFER_LIMIT_OVERRIDE < 0)) {
  throw new Error('SYNC_ACCEPTANCE_MAX_BUFFERED_BYTES must be a non-negative safe integer');
}

const requireFromNoisedeck = createRequire(path.join(NOISEDECK_ROOT, 'package.json'));
const { chromium } = requireFromNoisedeck('@playwright/test');
const noisedeckOrigin = new URL(NOISEDECK_URL).origin;

function runReceiver() {
  if (TRANSPORT_ONLY) {
    return new Promise((resolve) => {
      setTimeout(() => resolve({ skipped: true }), DURATION_MS);
    });
  }
  const child = spawn(RECEIVER, [
    SYPHON_FRAMEWORK_PATH,
    SENDER_NAME,
    String(DURATION_MS),
    String(DISCOVERY_TIMEOUT_MS),
    String(EXPECTED_WIDTH),
    String(EXPECTED_HEIGHT),
  ], {
    cwd: ROOT,
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let stdout = '';
  let stderr = '';
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', (chunk) => { stdout += chunk; });
  child.stderr.on('data', (chunk) => { stderr += chunk; });
  return new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('close', (code, signal) => {
      try {
        resolve({
          ...JSON.parse(stdout.trim()),
          probeExitCode: code,
          probeSignal: signal,
          probeStderr: stderr,
        });
      } catch (error) {
        reject(new Error(
          `Invalid receiver output: code=${code} signal=${signal} stdout=${stdout} stderr=${stderr}`,
          { cause: error },
        ));
      }
    });
  });
}

const browser = await chromium.launch({ headless: !HEADFUL });
const context = await browser.newContext({ viewport: { width: 1280, height: 720 } });
await context.grantPermissions(['local-network-access'], { origin: noisedeckOrigin });
const page = await context.newPage();
await page.route('**/deployment-meta.json', async (route) => {
  if (new URL(route.request().url()).origin === noisedeckOrigin) {
    await route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ git_hash: 'acceptance', date: 0 }),
    });
  } else {
    await route.continue();
  }
});
const consoleErrors = [];
const pageErrors = [];
page.on('console', (message) => {
  if (message.type() === 'error') consoleErrors.push(message.text());
});
page.on('pageerror', (error) => pageErrors.push(error.message));

async function waitForStatus(text, timeout = 10_000) {
  try {
    await page.locator('#syncOutputStateText').filter({ hasText: text }).waitFor({
      state: 'visible',
      timeout,
    });
  } catch (cause) {
    const diagnostic = await page.evaluate(async ({ consoleErrors, pageErrors }) => {
      const { syncOutputController } = await import('/js/app.js');
      return {
        state: syncOutputController.state,
        stateText: document.getElementById('syncOutputStateText')?.textContent,
        recovery: document.getElementById('syncOutputRecovery')?.textContent,
        action: document.getElementById('syncOutputAction')?.textContent,
        origin: location.origin,
        networkLog: window.__syncAcceptanceNetworkLog,
        stateLog: window.__syncAcceptanceStateLog,
        consoleErrors,
        pageErrors,
      };
    }, { consoleErrors, pageErrors });
    throw new Error(`Timed out waiting for Sync status ${text}: ${JSON.stringify(diagnostic)}`, {
      cause,
    });
  }
}

await page.addInitScript(() => {
  localStorage.setItem(
    'noisedeck-app-settings',
    JSON.stringify({ showStartupDialog: false }),
  );
  const originalFetch = globalThis.fetch;
  globalThis.__syncAcceptanceNetworkLog = [];
  globalThis.fetch = async (...args) => {
    const url = String(args[0]);
    try {
      const response = await originalFetch(...args);
      globalThis.__syncAcceptanceNetworkLog.push({ url, status: response.status });
      return response;
    } catch (error) {
      globalThis.__syncAcceptanceNetworkLog.push({
        url,
        error: String(error),
        name: error?.name,
        message: error?.message,
      });
      throw error;
    }
  };
  const originalSend = WebSocket.prototype.send;
  WebSocket.prototype.send = function sendWithSyncLatencyMarker(data) {
    if (data instanceof ArrayBuffer && data.byteLength >= 92) {
      const bytes = new Uint8Array(data);
      if (bytes[0] === 0x53 && bytes[1] === 0x59 &&
          bytes[2] === 0x4e && bytes[3] === 0x43) {
        bytes.set([0x53, 0x59, 0x4e, 0x43], 64);
        bytes.copyWithin(68, 36, 44);
        bytes.copyWithin(76, 44, 52);
        new DataView(data).setBigUint64(
          84,
          BigInt(Math.round((performance.timeOrigin + performance.now()) * 1000)),
          true,
        );
      }
    }
    return originalSend.call(this, data);
  };
});

let startedState;
let finalState;
let browserSamples = [];
let receiverResult;
let graphics;
let observedDurationMs = 0;
try {
  await page.goto(NOISEDECK_URL, { waitUntil: 'domcontentloaded', timeout: 30_000 });
  await page.waitForFunction(() => {
    const app = document.getElementById('app-container');
    const loading = document.getElementById('loadingOverlay');
    return Boolean(
      window.__i18n &&
      window.__noisemakerUI &&
      window.__noisemakerCanvasRenderer?.pipeline &&
      app &&
      loading &&
      app.style.display !== 'none' &&
      loading.style.display === 'none',
    );
  }, null, {
    timeout: 30_000,
  });
  await page.locator('#menu .hf-menubar-trigger', { hasText: 'view' }).click();
  await page.locator('#syncOutputMenuItem').click();
  await page.locator('#syncOutputAction').filter({ hasText: 'Connect Sync' }).waitFor({
    state: 'visible',
    timeout: 10_000,
  });
  await page.evaluate(async (token) => {
    const { syncOutputController } = await import('/js/app.js');
    window.__syncAcceptanceStateLog = [];
    syncOutputController.subscribe((state) => {
      window.__syncAcceptanceStateLog.push(JSON.parse(JSON.stringify(state)));
    });
    syncOutputController._token = token;
  }, ACCEPTANCE_TOKEN);
  await page.locator('#syncOutputAction').click();
  await waitForStatus('Connected');
  await page.locator('#syncOutputName').fill(SENDER_NAME);
  await page.locator('#syncOutputAction').click();
  await waitForStatus('Sending');
  if (BUFFER_LIMIT_OVERRIDE !== null) {
    await page.evaluate(async (limit) => {
      const { syncOutputController } = await import('/js/app.js');
      syncOutputController._sender._frameSink._maxBufferedBytes = limit;
    }, BUFFER_LIMIT_OVERRIDE);
    await page.waitForFunction(async () => {
      const { syncOutputController } = await import('/js/app.js');
      return syncOutputController._sender?._dataSocket?.bufferedAmount === 0;
    }, null, { timeout: 30_000 });
  }
  startedState = await page.evaluate(async () => {
    const { syncOutputController } = await import('/js/app.js');
    window.__syncAcceptanceSamples = [];
    window.__syncAcceptanceSampleTimer = window.setInterval(() => {
      const sender = syncOutputController._sender;
      window.__syncAcceptanceSamples.push({
        elapsedMs: Math.round(performance.now()),
        accepted: sender?.stats?.accepted ?? null,
        sent: sender?.stats?.sent ?? null,
        droppedBusy: sender?.stats?.droppedBusy ?? null,
        droppedBackpressure: sender?.stats?.droppedBackpressure ?? null,
        failed: sender?.stats?.failed ?? null,
        bufferedAmount: sender?._dataSocket?.bufferedAmount ?? null,
        frameIndex: window.__noisemakerCanvasRenderer?.pipeline?.frameIndex ?? null,
      });
    }, 250);
    return syncOutputController.state;
  });
  assert.equal(startedState.status, 'sending');
  graphics = await page.evaluate(() => {
    const canvas = document.querySelector('canvas');
    const gl = canvas?.getContext('webgl2');
    const debug = gl?.getExtension('WEBGL_debug_renderer_info');
    return {
      userAgent: navigator.userAgent,
      renderer: debug ? gl.getParameter(debug.UNMASKED_RENDERER_WEBGL) : null,
      vendor: debug ? gl.getParameter(debug.UNMASKED_VENDOR_WEBGL) : null,
    };
  });
  const measurementStartedAt = performance.now();
  receiverResult = await runReceiver();
  observedDurationMs = performance.now() - measurementStartedAt;
  const browserResult = await page.evaluate(async () => {
    const { syncOutputController } = await import('/js/app.js');
    const state = syncOutputController.state;
    clearInterval(window.__syncAcceptanceSampleTimer);
    const samples = window.__syncAcceptanceSamples;
    await syncOutputController.stop();
    return { state, samples };
  });
  finalState = browserResult.state;
  browserSamples = browserResult.samples;
} finally {
  await page.evaluate(async () => {
    try {
      const { syncOutputController } = await import('/js/app.js');
      if (syncOutputController.state.status === 'sending') {
        await syncOutputController.stop();
      }
    } catch {
      // Preserve the acceptance failure that triggered cleanup.
    }
  }).catch(() => {});
  await browser.close();
}

const output = {
  configuration: {
    url: NOISEDECK_URL,
    width: startedState.width,
    height: startedState.height,
    fps: startedState.fps,
    durationMs: DURATION_MS,
    maxBufferedBytesOverride: BUFFER_LIMIT_OVERRIDE,
    headful: HEADFUL,
    graphics,
  },
  browser: {
    stats: finalState.stats,
    telemetry: {
      sampleCount: browserSamples.length,
      observedDurationMs,
      maximumBufferedAmount: browserSamples.reduce(
        (maximum, sample) => Math.max(maximum, sample.bufferedAmount ?? 0),
        0,
      ),
      observedRenderFps: browserSamples.length < 2
        ? null
        : (browserSamples.at(-1).frameIndex - browserSamples[0].frameIndex) * 1000 /
          (browserSamples.at(-1).elapsedMs - browserSamples[0].elapsedMs),
    },
    ...(INCLUDE_SAMPLES ? { samples: browserSamples } : {}),
    consoleErrors,
    pageErrors,
  },
  receiver: receiverResult,
};
console.log(JSON.stringify(output));
assertLatencyAcceptance(output, {
  width: EXPECTED_WIDTH,
  height: EXPECTED_HEIGHT,
  fps: EXPECTED_FPS,
  durationMs: DURATION_MS,
  minimumObservedRenderFps: MINIMUM_OBSERVED_RENDER_FPS,
  minimumSamples: MINIMUM_SAMPLES,
  maximumP95LatencyUs: MAXIMUM_P95_LATENCY_US,
}, { transportOnly: TRANSPORT_ONLY });
