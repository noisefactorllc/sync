import assert from 'node:assert/strict';
import test from 'node:test';

import {
  assertLatencyAcceptance,
  normalizeAcceptanceBackend,
} from './latency-acceptance-contract.js';

const REQUIREMENTS = Object.freeze({
  width: 1024,
  height: 1024,
  fps: 60,
  backend: 'glsl',
  durationMs: 20_000,
  minimumObservedRenderFps: 55,
  minimumSamples: 120,
  maximumP95LatencyUs: 66_000,
});

function passingResult() {
  return {
    configuration: {
      width: 1024,
      height: 1024,
      fps: 60,
      backend: 'glsl',
      actualBackend: 'WebGL2',
      durationMs: 20_000,
    },
    browser: {
      stats: {
        accepted: 400,
        droppedBusy: 0,
        droppedBackpressure: 800,
        sent: 399,
        failed: 0,
      },
      telemetry: {
        sampleCount: 80,
        observedDurationMs: 19_750,
        observedRenderFps: 60.01,
      },
      consoleErrors: [],
      pageErrors: [],
    },
    receiver: {
      probeExitCode: 0,
      probeSignal: null,
      serverFound: true,
      framesSeen: 399,
      markers: 398,
      duplicateMarkers: 0,
      probeDropped: 0,
      invalidMarkers: 0,
      dimensionMismatches: 0,
      commandErrors: 0,
      latencyUs: { p95: 60_148 },
    },
  };
}

test('acceptance contract passes a sustained 1024x1024 60 fps run under 66 ms p95', () => {
  assert.doesNotThrow(() => assertLatencyAcceptance(
    passingResult(),
    REQUIREMENTS,
    { transportOnly: false },
  ));
});

test('acceptance backend accepts only explicit WebGL2 and WebGPU modes', () => {
  assert.equal(normalizeAcceptanceBackend('glsl'), 'glsl');
  assert.equal(normalizeAcceptanceBackend('wgsl'), 'wgsl');
  assert.throws(() => normalizeAcceptanceBackend(undefined), /glsl or wgsl/);
  assert.throws(() => normalizeAcceptanceBackend('webgpu'), /glsl or wgsl/);
});

test('acceptance contract rejects every shortcut that could make one bad frame green', async (t) => {
  const mutations = [
    ['wrong width', (result) => { result.configuration.width = 640; }, /width/],
    ['wrong height', (result) => { result.configuration.height = 480; }, /height/],
    ['wrong declared fps', (result) => { result.configuration.fps = 30; }, /fps/],
    ['wrong requested backend', (result) => { result.configuration.backend = 'wgsl'; }, /backend/],
    ['backend fallback', (result) => { result.configuration.actualBackend = 'WebGPU'; }, /actual backend/],
    ['short duration', (result) => { result.browser.telemetry.observedDurationMs = 1_000; }, /duration/],
    ['slow renderer', (result) => { result.browser.telemetry.observedRenderFps = 12; }, /render fps/],
    ['one sample', (result) => { result.receiver.markers = 1; }, /samples/],
    ['slow p95', (result) => { result.receiver.latencyUs.p95 = 66_001; }, /p95/],
    ['dimension mismatch', (result) => { result.receiver.dimensionMismatches = 1; }, /dimension/],
    ['Metal command failure', (result) => { result.receiver.commandErrors = 1; }, /command/],
    ['browser send failure', (result) => { result.browser.stats.failed = 1; }, /failed/],
  ];

  for (const [name, mutate, expected] of mutations) {
    await t.test(name, () => {
      const result = passingResult();
      mutate(result);
      assert.throws(
        () => assertLatencyAcceptance(result, REQUIREMENTS, { transportOnly: false }),
        expected,
      );
    });
  }
});

test('transport-only acceptance still requires sustained sent frames', () => {
  const result = passingResult();
  result.browser.stats.sent = 0;
  result.receiver = { skipped: true };

  assert.throws(
    () => assertLatencyAcceptance(result, REQUIREMENTS, { transportOnly: true }),
    /sent frames/,
  );
});
