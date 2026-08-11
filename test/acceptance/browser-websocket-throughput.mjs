import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const NOISEDECK_ROOT = path.resolve(
  process.env.NOISEDECK_ROOT || path.join(ROOT, '..', 'noisedeck'),
);
const PAGE_URL = process.env.NOISEDECK_URL || 'http://127.0.0.1:8000/';
const ENDPOINT = process.env.SYNC_ACCEPTANCE_ENDPOINT || 'ws://127.0.0.1:53979';
const TOKEN = process.env.SYNC_ACCEPTANCE_TOKEN;
const WIDTH = Number(process.env.SYNC_ACCEPTANCE_WIDTH || 1024);
const HEIGHT = Number(process.env.SYNC_ACCEPTANCE_HEIGHT || 1024);

if (!TOKEN) throw new Error('SYNC_ACCEPTANCE_TOKEN is required');
for (const [name, value] of [['width', WIDTH], ['height', HEIGHT]]) {
  if (!Number.isSafeInteger(value) || value <= 0) {
    throw new Error(`${name} must be a positive safe integer`);
  }
}

const requireFromNoisedeck = createRequire(path.join(NOISEDECK_ROOT, 'package.json'));
const { chromium } = requireFromNoisedeck('@playwright/test');
const pageOrigin = new URL(PAGE_URL).origin;

const browser = await chromium.launch({ headless: true });
const context = await browser.newContext();
await context.grantPermissions(['local-network-access'], { origin: pageOrigin });
const page = await context.newPage();

let result;
try {
  await page.goto(PAGE_URL, { waitUntil: 'domcontentloaded', timeout: 30_000 });
  await page.setContent('<!doctype html><title>Sync transport probe</title>');
  result = await page.evaluate(async ({ endpoint, token, width, height }) => {
    const openSocket = (url, protocol) => new Promise((resolve, reject) => {
      const socket = protocol === undefined
        ? new WebSocket(url)
        : new WebSocket(url, protocol);
      socket.addEventListener('open', () => resolve(socket), { once: true });
      socket.addEventListener('error', () => reject(new Error(`failed to open ${url}`)), {
        once: true,
      });
    });
    const nextJson = (socket) => new Promise((resolve, reject) => {
      socket.addEventListener('message', (event) => {
        try { resolve(JSON.parse(event.data)); } catch (error) { reject(error); }
      }, { once: true });
    });

    const control = await openSocket(`${endpoint}/control`);
    control.send(JSON.stringify({ type: 'hello', token, protocolVersions: [1] }));
    const welcome = await nextJson(control);
    if (welcome.type !== 'welcome') throw new Error(`unexpected welcome: ${JSON.stringify(welcome)}`);
    control.send(JSON.stringify({ type: 'createSender', name: 'Browser transport probe' }));
    const created = await nextJson(control);
    if (created.type !== 'senderCreated') {
      throw new Error(`unexpected sender response: ${JSON.stringify(created)}`);
    }
    const data = await openSocket(
      `${endpoint}${created.path}`,
      `sync.sender.${created.ticket}`,
    );

    const payloadBytes = width * height * 4;
    const frame = new ArrayBuffer(64 + payloadBytes);
    const view = new DataView(frame);
    view.setUint32(0, 0x434e5953, true);
    view.setUint16(4, 1, true);
    view.setUint16(6, 64, true);
    view.setUint32(8, 1, true);
    view.setUint16(12, 1, true);
    view.setUint16(14, 1, true);
    view.setUint16(16, 3, true);
    view.setUint32(20, width, true);
    view.setUint32(24, height, true);
    view.setUint32(28, width * 4, true);
    view.setUint32(32, payloadBytes, true);
    view.setBigUint64(36, 1n, true);
    view.setBigUint64(44, BigInt(Math.round(Date.now() * 1000)), true);

    const samples = [];
    const start = performance.now();
    data.send(frame);
    const afterSend = performance.now();
    while (data.bufferedAmount > 0 && performance.now() - start < 30_000) {
      samples.push({ elapsedMs: performance.now() - start, bufferedAmount: data.bufferedAmount });
      await new Promise((resolve) => setTimeout(resolve, 25));
    }
    const drained = performance.now();

    let stats;
    while (performance.now() - start < 30_000) {
      control.send(JSON.stringify({ type: 'getStats', senderId: created.id }));
      stats = await nextJson(control);
      if (stats.accepted === 1) break;
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    const accepted = performance.now();
    control.send(JSON.stringify({ type: 'closeSender', senderId: created.id }));
    await nextJson(control);
    control.close();

    return {
      width,
      height,
      frameBytes: frame.byteLength,
      sendCallMs: afterSend - start,
      drainMs: drained - start,
      acceptedMs: accepted - start,
      finalBufferedAmount: data.bufferedAmount,
      stats,
      samples,
    };
  }, { endpoint: ENDPOINT, token: TOKEN, width: WIDTH, height: HEIGHT });
} finally {
  await browser.close();
}

assert.equal(result.finalBufferedAmount, 0, 'browser transport buffer drains');
assert.equal(result.stats.accepted, 1, 'daemon accepts the frame');
console.log(JSON.stringify(result));
