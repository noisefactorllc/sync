// Streams real-size frames through a test-receiver syncd for a while and
// watches its memory. A daemon that leaks per frame, per sender, per
// connection, or per health probe shows up here as a rising line; a sound one
// settles after warm-up and stays flat.
//
// On macOS the number watched is the physical footprint, not RSS. Freed
// multi-megabyte payload buffers are handed back to the kernel as reusable
// pages, and those still count toward RSS until the kernel takes them, so RSS
// climbs by one frame's worth every few sender cycles while nothing is held.
// Physical footprint excludes them and is what Activity Monitor reports.
//
//   SYNC_DAEMON_PATH=build/syncd node test/acceptance/daemon-memory-soak.mjs
//
// Environment:
//   SYNC_SOAK_SECONDS   total run length (default 30)
//   SYNC_SOAK_CYCLE     seconds between sender close/recreate cycles (default 5)
//   SYNC_SOAK_WIDTH     frame width (default 1920)
//   SYNC_SOAK_HEIGHT    frame height (default 1080)
//   SYNC_SOAK_GROWTH_KB allowed footprint growth after warm-up, KiB (default 8192)
//   SYNC_SOAK_LEAKS     1 to run macOS `leaks` against the daemon before it
//                       exits and fail on any unreachable block
//
// Every cycle closes the sender and its data socket, drops the control socket,
// and builds all three again, so the run covers the lifecycle paths as well as
// the frame path. A /health probe and a getStats round trip run every second.

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { upgrade } from '../soak/lib/ws.mjs';
import { createFrameBuffer, stampFrame } from '../soak/lib/frame.mjs';
import { residentKb, footprintKb, runLeaks } from '../soak/lib/process-metrics.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const SYNCD = process.env.SYNC_DAEMON_PATH
  ? path.resolve(ROOT, process.env.SYNC_DAEMON_PATH)
  : path.join(ROOT, 'build', 'syncd');
const SECONDS = Number(process.env.SYNC_SOAK_SECONDS || 30);
const CYCLE_SECONDS = Number(process.env.SYNC_SOAK_CYCLE || 5);
const WIDTH = Number(process.env.SYNC_SOAK_WIDTH || 1920);
const HEIGHT = Number(process.env.SYNC_SOAK_HEIGHT || 1080);
const GROWTH_KB = Number(process.env.SYNC_SOAK_GROWTH_KB || 8192);
const CHECK_LEAKS = process.env.SYNC_SOAK_LEAKS === '1';
const ORIGIN = 'https://soak.example';
const TOKEN = 'soak-token-123';
const MAX_BUFFERED = 3 * (64 + WIDTH * HEIGHT * 4);

for (const [name, value] of [['seconds', SECONDS], ['cycle', CYCLE_SECONDS],
  ['width', WIDTH], ['height', HEIGHT], ['growth', GROWTH_KB]]) {
  assert.ok(Number.isFinite(value) && value > 0, `${name} must be positive`);
}
assert.ok(SECONDS >= 5, 'a run shorter than five seconds has no post-warm-up window');

async function health(port) {
  const response = await fetch(`http://127.0.0.1:${port}/health`, {
    headers: { Origin: ORIGIN },
  });
  assert.equal(response.status, 200);
  await response.arrayBuffer();
}

// One frame buffer for the run: the header's sequence and timestamp and one
// payload byte change per frame, so the sender side does no per-frame
// allocation and the daemon, not this script, sets the pace.
const FRAME = createFrameBuffer({ width: WIDTH, height: HEIGHT });

// --- run ------------------------------------------------------------------

const daemon = spawn(SYNCD, ['--port', '0', '--test-origin', ORIGIN, '--test-token', TOKEN,
  '--test-receiver'], { cwd: ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
let stderr = '';
daemon.stderr.setEncoding('utf8');
daemon.stderr.on('data', (chunk) => { stderr = (stderr + chunk).slice(-4096); });
const ready = await new Promise((resolve, reject) => {
  let stdout = '';
  daemon.stdout.setEncoding('utf8');
  daemon.stdout.on('data', (chunk) => {
    stdout += chunk;
    const newline = stdout.indexOf('\n');
    if (newline >= 0) resolve(JSON.parse(stdout.slice(0, newline)));
  });
  daemon.once('exit', (code) => reject(new Error(`syncd exited early with ${code}: ${stderr}`)));
});
assert.equal(ready.type, 'ready');
const port = ready.port;
console.log(`syncd pid=${daemon.pid} port=${port}`);

const samples = [];
const started = Date.now();
let sentFrames = 0;
let acceptedFrames = 0;
let droppedFrames = 0;
let cycles = 0;
let control = null;
let data = null;
let senderId = null;
let sequence = 0;
let stop = false;
let runError = null;

async function openSession() {
  control = await upgrade(port, '/control', { origin: ORIGIN });
  control.sendText(JSON.stringify({ type: 'hello', token: TOKEN, protocolVersions: [1] }));
  const welcome = await control.nextJson();
  assert.equal(welcome.type, 'welcome');
  control.sendText(JSON.stringify({ type: 'createSender', name: `Soak ${cycles}` }));
  const created = await control.nextJson();
  assert.equal(created.type, 'senderCreated', JSON.stringify(created));
  senderId = created.id;
  data = await upgrade(port, created.path, { origin: ORIGIN, subprotocol: `sync.sender.${created.ticket}` });
  cycles += 1;
}

async function closeSession() {
  if (control && !control.closed) {
    control.sendText(JSON.stringify({ type: 'closeSender', senderId }));
    const closed = await control.nextJson().catch(() => null);
    if (closed) assert.equal(closed.type, 'senderClosed', JSON.stringify(closed));
    await control.close();
  }
  if (data && !data.closed) await data.close();
  control = null;
  data = null;
}

async function stats() {
  if (!control || control.closed) return;
  control.sendText(JSON.stringify({ type: 'getStats', senderId }));
  const reply = await control.nextJson();
  if (reply.type === 'stats') {
    acceptedFrames = reply.accepted;
    droppedFrames = reply.dropped;
  }
}

const streamer = (async () => {
  try {
    await openSession();
    let lastCycle = Date.now();
    while (!stop) {
      if (Date.now() - lastCycle >= CYCLE_SECONDS * 1000) {
        await stats();
        await closeSession();
        await openSession();
        lastCycle = Date.now();
      }
      if (data.socket.writableLength > MAX_BUFFERED) {
        await new Promise((resolve) => setTimeout(resolve, 2));
        continue;
      }
      sequence += 1;
      await data.sendBinary(stampFrame(FRAME, sequence, BigInt(Date.now()) * 1000n));
      sentFrames += 1;
    }
    await stats();
    await closeSession();
  } catch (error) {
    runError = error;
  }
})();

const sampler = setInterval(() => {
  try {
    samples.push({ t: Math.round((Date.now() - started) / 1000), rss: residentKb(daemon.pid),
      footprint: footprintKb(daemon.pid), frames: sentFrames });
  } catch (error) {
    runError ??= error;
  }
  health(port).catch((error) => { runError ??= error; });
}, 1000);

await new Promise((resolve) => setTimeout(resolve, SECONDS * 1000));
stop = true;
await streamer;
clearInterval(sampler);
samples.push({ t: Math.round((Date.now() - started) / 1000), rss: residentKb(daemon.pid),
  footprint: footprintKb(daemon.pid), frames: sentFrames });

let leaksReport = null;
if (CHECK_LEAKS) {
  // `leaks` walks the live heap for blocks nothing references. It tells a
  // true leak apart from memory the allocator merely keeps cached.
  leaksReport = runLeaks(daemon.pid);
}

daemon.kill('SIGTERM');
const exitCode = await new Promise((resolve) => daemon.once('exit', resolve));

console.log('t(s)  footprint(KiB)  rss(KiB)  frames');
for (const sample of samples) {
  console.log(`${String(sample.t).padStart(4)}  ${String(sample.footprint).padStart(14)}  ` +
    `${String(sample.rss).padStart(8)}  ${sample.frames}`);
}
const warm = samples.filter((sample) => sample.t >= Math.max(3, SECONDS * 0.25));
const first = warm[0];
const last = warm[warm.length - 1];
const peak = Math.max(...warm.map((sample) => sample.footprint));
const growth = last.footprint - first.footprint;
console.log(`cycles=${cycles} sent=${sentFrames} fps=${(sentFrames / SECONDS).toFixed(1)} ` +
  `accepted=${acceptedFrames} dropped=${droppedFrames} ` +
  `footprint_after_warmup=${first.footprint}KiB footprint_end=${last.footprint}KiB ` +
  `footprint_peak=${peak}KiB growth=${growth}KiB rss_end=${last.rss}KiB exit=${exitCode}`);
if (stderr.trim()) console.log(`syncd stderr:\n${stderr}`);
if (leaksReport !== null) {
  const summary = leaksReport.raw.split('\n').find((line) => /leaks for .* total leaked bytes/.test(line));
  console.log(summary ?? leaksReport.raw.trim().split('\n').slice(-3).join('\n'));
}

assert.equal(runError, null, runError?.stack);
assert.equal(exitCode, 0, 'syncd exits cleanly on SIGTERM');
assert.ok(sentFrames > SECONDS * 2, 'the stream actually ran');
assert.ok(cycles >= Math.floor(SECONDS / CYCLE_SECONDS), 'sender lifecycle cycled');
if (leaksReport !== null) {
  assert.ok(leaksReport.leaks !== null, `leaks produced no summary:\n${leaksReport.raw}`);
  assert.equal(leaksReport.leaks, 0, `leaks found ${leaksReport.leaks} unreachable blocks`);
}
assert.ok(growth <= GROWTH_KB,
  `memory footprint grew ${growth} KiB after warm-up (limit ${GROWTH_KB})`);
