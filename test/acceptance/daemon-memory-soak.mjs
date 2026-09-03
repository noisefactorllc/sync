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
import { spawn, execFileSync } from 'node:child_process';
import { createHash, randomBytes } from 'node:crypto';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

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

// --- minimal RFC 6455 client over a raw socket ---------------------------

function maskedFrame(opcode, payload) {
  // A zero mask key is a valid key and leaves the payload bytes unchanged, so
  // a multi-megabyte frame needs no per-byte work here.
  const length = payload.length;
  let header;
  if (length <= 125) {
    header = Buffer.from([0x80 | opcode, 0x80 | length, 0, 0, 0, 0]);
  } else if (length <= 65535) {
    header = Buffer.alloc(8);
    header[0] = 0x80 | opcode;
    header[1] = 0x80 | 126;
    header.writeUInt16BE(length, 2);
  } else {
    header = Buffer.alloc(14);
    header[0] = 0x80 | opcode;
    header[1] = 0x80 | 127;
    header.writeBigUInt64BE(BigInt(length), 2);
  }
  return [header, payload];
}

class RawWebSocket {
  constructor(socket, remainder) {
    this.socket = socket;
    this.buffer = remainder;
    this.waiters = [];
    this.messages = [];
    this.closed = false;
    socket.on('data', (chunk) => {
      this.buffer = Buffer.concat([this.buffer, chunk]);
      this.drain();
    });
    socket.on('close', () => {
      this.closed = true;
      for (const waiter of this.waiters.splice(0)) waiter.reject(new Error('socket closed'));
    });
    socket.on('error', () => {});
  }

  drain() {
    for (;;) {
      if (this.buffer.length < 2) return;
      const opcode = this.buffer[0] & 0x0f;
      let length = this.buffer[1] & 0x7f;
      let offset = 2;
      if (length === 126) {
        if (this.buffer.length < 4) return;
        length = this.buffer.readUInt16BE(2);
        offset = 4;
      } else if (length === 127) {
        if (this.buffer.length < 10) return;
        length = Number(this.buffer.readBigUInt64BE(2));
        offset = 10;
      }
      if (this.buffer.length < offset + length) return;
      const payload = this.buffer.subarray(offset, offset + length);
      this.buffer = this.buffer.subarray(offset + length);
      const message = { opcode, payload: Buffer.from(payload) };
      const waiter = this.waiters.shift();
      if (waiter) waiter.resolve(message);
      else this.messages.push(message);
    }
  }

  next(timeoutMs = 5000) {
    if (this.messages.length > 0) return Promise.resolve(this.messages.shift());
    if (this.closed) return Promise.reject(new Error('socket closed'));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiters = this.waiters.filter((w) => w.resolve !== wrapped);
        reject(new Error('timed out waiting for a message'));
      }, timeoutMs);
      const wrapped = (value) => { clearTimeout(timer); resolve(value); };
      this.waiters.push({ resolve: wrapped, reject: (e) => { clearTimeout(timer); reject(e); } });
    });
  }

  async nextJson() {
    for (;;) {
      const message = await this.next();
      if (message.opcode === 0x1) return JSON.parse(message.payload.toString('utf8'));
      if (message.opcode === 0x8) throw new Error(`peer closed: ${message.payload.toString('utf8', 2)}`);
    }
  }

  sendText(text) {
    const [header, payload] = maskedFrame(0x1, Buffer.from(text));
    this.socket.write(Buffer.concat([header, payload]));
  }

  sendBinary(payload) {
    const [header] = maskedFrame(0x2, payload);
    this.socket.write(header);
    return new Promise((resolve) => this.socket.write(payload, resolve));
  }

  close() {
    if (this.closed) return Promise.resolve();
    const [header, payload] = maskedFrame(0x8, Buffer.from([0x03, 0xe8]));
    this.socket.write(Buffer.concat([header, payload]));
    return new Promise((resolve) => {
      const done = () => resolve();
      this.socket.once('close', done);
      setTimeout(() => { this.socket.destroy(); }, 1000).unref();
    });
  }
}

function connect(port) {
  return new Promise((resolve, reject) => {
    const socket = net.connect({ host: '127.0.0.1', port }, () => resolve(socket));
    socket.setNoDelay(true);
    socket.once('error', reject);
  });
}

async function upgrade(port, route, subprotocol) {
  const socket = await connect(port);
  const key = randomBytes(16).toString('base64');
  socket.write(
    `GET ${route} HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\nUpgrade: websocket\r\n` +
    `Connection: Upgrade\r\nOrigin: ${ORIGIN}\r\nSec-WebSocket-Version: 13\r\n` +
    `Sec-WebSocket-Key: ${key}\r\n` +
    (subprotocol ? `Sec-WebSocket-Protocol: ${subprotocol}\r\n` : '') + '\r\n',
  );
  let bytes = Buffer.alloc(0);
  for (;;) {
    const chunk = await new Promise((resolve, reject) => {
      socket.once('data', resolve);
      socket.once('error', reject);
    });
    bytes = Buffer.concat([bytes, chunk]);
    const marker = bytes.indexOf('\r\n\r\n');
    if (marker >= 0) {
      const head = bytes.subarray(0, marker).toString('latin1');
      assert.match(head, /^HTTP\/1\.1 101 /, `upgrade ${route}: ${head.split('\r\n')[0]}`);
      const expected = createHash('sha1')
        .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
      assert.ok(head.includes(`Sec-WebSocket-Accept: ${expected}`), 'accept key');
      return new RawWebSocket(socket, bytes.subarray(marker + 4));
    }
  }
}

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
const FRAME = (() => {
  const payloadBytes = WIDTH * HEIGHT * 4;
  const frame = Buffer.alloc(64 + payloadBytes);
  frame.writeUInt32LE(0x434e5953, 0);
  frame.writeUInt16LE(1, 4);
  frame.writeUInt16LE(64, 6);
  frame.writeUInt32LE(1, 8);
  frame.writeUInt16LE(1, 12);
  frame.writeUInt16LE(1, 14);
  frame.writeUInt16LE(3, 16);
  frame.writeUInt32LE(WIDTH, 20);
  frame.writeUInt32LE(HEIGHT, 24);
  frame.writeUInt32LE(WIDTH * 4, 28);
  frame.writeUInt32LE(payloadBytes, 32);
  return frame;
})();

function frameBytes(sequence) {
  FRAME.writeBigUInt64LE(BigInt(sequence), 36);
  FRAME.writeBigUInt64LE(BigInt(Date.now()) * 1000n, 44);
  // A varying payload keeps the receiver's checksum honest across frames.
  FRAME[64] = sequence & 0xff;
  return FRAME;
}

function residentKb(pid) {
  const text = execFileSync('ps', ['-o', 'rss=', '-p', String(pid)], { encoding: 'utf8' });
  return Number(text.trim());
}

// vmmap is synchronous and takes a few hundred milliseconds, which shows in
// the table as a small dip in frames sent around each sample. Without the
// developer tools it is absent, and RSS stands in.
function footprintKb(pid) {
  if (process.platform !== 'darwin') return residentKb(pid);
  let text;
  try {
    text = execFileSync('vmmap', ['-summary', String(pid)], { encoding: 'utf8' });
  } catch {
    return residentKb(pid);
  }
  const match = /Physical footprint:\s+([\d.]+)([KMG])/.exec(text);
  if (!match) return residentKb(pid);
  const scale = { K: 1, M: 1024, G: 1024 * 1024 }[match[2]];
  return Math.round(Number(match[1]) * scale);
}

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
  control = await upgrade(port, '/control');
  control.sendText(JSON.stringify({ type: 'hello', token: TOKEN, protocolVersions: [1] }));
  const welcome = await control.nextJson();
  assert.equal(welcome.type, 'welcome');
  control.sendText(JSON.stringify({ type: 'createSender', name: `Soak ${cycles}` }));
  const created = await control.nextJson();
  assert.equal(created.type, 'senderCreated', JSON.stringify(created));
  senderId = created.id;
  data = await upgrade(port, created.path, `sync.sender.${created.ticket}`);
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
      await data.sendBinary(frameBytes(sequence));
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
  try {
    leaksReport = execFileSync('leaks', ['--quiet', String(daemon.pid)], { encoding: 'utf8' });
  } catch (error) {
    // leaks exits 1 when it finds something; the report is still on stdout.
    leaksReport = error.stdout ?? String(error);
  }
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
  const summary = leaksReport.split('\n').find((line) => /leaks for .* total leaked bytes/.test(line));
  console.log(summary ?? leaksReport.trim().split('\n').slice(-3).join('\n'));
}

assert.equal(runError, null, runError?.stack);
assert.equal(exitCode, 0, 'syncd exits cleanly on SIGTERM');
assert.ok(sentFrames > SECONDS * 2, 'the stream actually ran');
assert.ok(cycles >= Math.floor(SECONDS / CYCLE_SECONDS), 'sender lifecycle cycled');
if (leaksReport !== null) {
  const match = /(\d+) leaks for (\d+) total leaked bytes/.exec(leaksReport);
  assert.ok(match, `leaks produced no summary:\n${leaksReport}`);
  assert.equal(Number(match[1]), 0, `leaks found ${match[1]} unreachable blocks`);
}
assert.ok(growth <= GROWTH_KB,
  `memory footprint grew ${growth} KiB after warm-up (limit ${GROWTH_KB})`);
