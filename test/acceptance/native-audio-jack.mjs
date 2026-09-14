import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const noisedeckRoot = path.resolve(process.env.NOISEDECK_ROOT || path.join(root, '../noisedeck'));
const noisemakerRoot = path.resolve(process.env.NOISEMAKER_ROOT || path.join(root, '../noisemaker'));
const endpoint = process.env.SYNC_AUDIO_TEST_ENDPOINT || 'http://127.0.0.1:48991';
const token = 'audio-test-token';
const require = createRequire(path.join(noisedeckRoot, 'package.json'));
const { chromium } = require('@playwright/test');

// The isolated daemon accepts this fixed origin and test token. Native capture,
// the protocol, SDK, worklets, AudioState, and channel choices run unchanged.
const browser = await chromium.launch();
const page = await browser.newPage();
const result = {
  recordedAtUtc: new Date().toISOString(),
  browser: browser.version(),
  route: 'JACK source → RtAudio → production syncd → SDK → Noisedeck AudioWorklet → channel choices and raw values',
  physicalHardwareTested: false,
  durationSeconds: 30,
};

try {
  const panel = await readFile(path.join(noisedeckRoot, 'app/js/ui/automationPanel.js'), 'utf8');
  await page.route('**/js/ui/automationPanel.js', route => route.fulfill({
    contentType: 'text/javascript', body: panel + '\nexport { _audioChannelChoices };',
  }));
  await page.route('**/js/sync/audio.js', route => route.fulfill({
    contentType: 'text/javascript',
    body: `
      import { SyncBridgeClient as Base } from '/js/sync/sdk/0.3.0/browser/index.js';
      window.nativePackets = { packets: 0, frames: 0, validatedFrames: 0,
        droppedFrames: '0', mismatchedPackets: 0, verified: false };
      export class SyncBridgeClient extends Base {
        constructor(options) {
          super({ ...options, endpoint: ${JSON.stringify(endpoint)},
            permissions: { query: async () => ({ state: 'granted' }) } });
        }
        async pair() { return { token: ${JSON.stringify(token)} }; }
        async readAudioSource(id) {
          const packet = await super.readAudioSource(id);
          const evidence = window.nativePackets;
          if (packet.frameCount) {
            evidence.packets++;
            evidence.frames += packet.frameCount;
            evidence.droppedFrames = String(packet.droppedFrames);
            const exact = packet.channelCount === 32 && packet.sampleRate === 48000 &&
              packet.planes.every((plane, channel) => plane.every(value =>
                value === (channel + 1) / 64 * (channel % 2 ? -1 : 1)));
            if (exact) {
              evidence.verified = true;
              evidence.validatedFrames += packet.frameCount;
            } else if (evidence.verified) evidence.mismatchedPackets++;
          }
          return packet;
        }
      }`,
  }));
  await page.route('**/__audio-source.js', route => route.fulfill({
    path: path.join(noisemakerRoot, 'shaders/src/runtime/external-input.js'),
    contentType: 'text/javascript',
  }));
  await page.goto('http://127.0.0.1:8000/does-not-exist.html');
  result.connection = await page.evaluate(async () => {
    navigator.mediaDevices.enumerateDevices = async () => [];
    navigator.mediaDevices.getUserMedia = async () => {
      throw new Error('Native audio must bypass getUserMedia');
    };
    const { AudioState } = await import('/__audio-source.js');
    window.audioState = new AudioState();
    let requirements = { needsLegacy: false, needsLegacyRaw: false, selected: [] };
    const renderer = {
      setAudioState: () => window.audioState,
      compile: async () => ({ getAudioInputRequirements: () => requirements }),
    };
    window.audio = await import('/js/features/audioInput.js');
    window.audio.initAudioInput({ renderer });
    window.select = async selected => {
      requirements = { ...requirements, selected };
      await renderer.compile(selected.length ? 'audio()' : '');
    };
    return {
      connected: await window.audio.requestSyncAudioInputAccess(),
      snapshot: window.audio.getAudioInputSnapshot(),
    };
  });
  assert.equal(result.connection.connected, true, JSON.stringify(result.connection.snapshot));
  const source = result.connection.snapshot.devices.find(device =>
    device.name === 'SyncTest32 (Jack) · Sync');
  assert.ok(source, 'RtAudio must discover the isolated 32-channel JACK source');
  assert.equal(source.channelCount, 32);
  assert.equal(source.sampleRate, 48000);
  result.source = source;
  await page.evaluate(source => window.select(Array.from({ length: 32 }, (_, channel) => ({
    id: source.id, name: source.name, channel: channel + 1, needsRaw: true,
  }))), source);
  await page.waitForFunction(id => Array.from({ length: 32 }, (_, c) =>
    window.audioState.getDeviceChannelState({ id, channel: c + 1 })?.raw).every((value, c) =>
    Math.abs(value - (c + 1) / 64 * (c % 2 ? -1 : 1)) < 0.00001), source.id,
  { timeout: 12000 });
  result.choices = await page.evaluate(async source =>
    (await import('/js/ui/automationPanel.js'))._audioChannelChoices(source)
      .map(choice => choice.value), source);
  assert.deepEqual(result.choices, Array.from({ length: 32 }, (_, c) => String(c + 1)));
  result.rawValues = await page.evaluate(id => Array.from({ length: 32 }, (_, c) =>
    window.audioState.getDeviceChannelState({ id, channel: c + 1 })?.raw), source.id);
  for (let i = 0; i < result.durationSeconds; i++) {
    await page.waitForTimeout(1000);
    const capture = await page.evaluate(() => window.audio.getAudioInputSnapshot().inputs[0]);
    assert.equal(capture.captureState, 'active', JSON.stringify(capture));
    const raw = await page.evaluate(id => Array.from({ length: 32 }, (_, c) =>
      window.audioState.getDeviceChannelState({ id, channel: c + 1 })?.raw), source.id);
    assert.deepEqual(raw, result.rawValues, 'All signed channel values must remain correct');
  }
  result.nativePackets = await page.evaluate(() => window.nativePackets);
  assert.equal(result.nativePackets.mismatchedPackets, 0);
  assert.ok(result.nativePackets.validatedFrames >= 48000 * 25);
  await page.evaluate(() => window.select([]));
  assert.deepEqual(await page.evaluate(() => window.audio.getAudioInputSnapshot().inputs), []);
  result.status = 'passed';
} catch (error) {
  result.status = 'failed';
  result.error = String(error);
  result.snapshot = await page.evaluate(() => ({
    audio: window.audio?.getAudioInputSnapshot(), packets: window.nativePackets,
  })).catch(() => null);
  process.exitCode = 1;
} finally {
  console.log(JSON.stringify(result, null, 2));
  await browser.close();
}
