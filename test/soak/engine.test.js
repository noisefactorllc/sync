import assert from 'node:assert/strict';
import test from 'node:test';
import { EventEmitter } from 'node:events';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { summarise, ProtocolSoak } from './engine.mjs';

const sample = (t, footprint) => ({ t, footprint, rss: footprint, frames: t * 60 });

async function startupFixture(t, body) {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'sync-startup-test-'));
  // A plain script run by THIS node, not a shebang file with an exec bit.
  // Windows honours neither, which is why these tests used to be skipped
  // there — and startup-cleanup behaviour is exactly what should not be
  // skipped on the platform with the different process model. Spawning
  // node explicitly is uniform across platforms and drops the chmod entirely.
  const script = path.join(directory, 'daemon.mjs');
  await writeFile(script, `${body}\n`);
  const soak = new ProtocolSoak({ daemonPath: process.execPath, daemonArgs: [script],
    width: 8, height: 8,
    startupTimeoutMs: 1000, stopTermTimeoutMs: 200, stopKillTimeoutMs: 200 });
  t.after(async () => {
    if (soak.daemon) await soak.stop();
    await rm(directory, { recursive: true, force: true });
  });
  return soak;
}

// These fixtures execute real children. The outer deadline makes a broken
// readiness wait fail here rather than wedging the test runner itself.
async function boundedStart(soak) {
  let timer;
  try {
    return await Promise.race([soak.start(), new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('outer test deadline')), 4000);
    })]);
  } finally {
    clearTimeout(timer);
  }
}

test('startup rejects a missing executable through its promise', async () => {
  const soak = new ProtocolSoak({ daemonPath: path.join(os.tmpdir(),
    'sync-nonexistent-directory', 'syncd'), width: 8, height: 8 });
  await assert.rejects(boundedStart(soak), /ENOENT/);
});

for (const [name, body, expected] of [
  ['malformed readiness', "process.stdout.write('not json\\n'); setInterval(() => {}, 1000)", /invalid syncd readiness/i],
  ['silent daemon', 'setInterval(() => {}, 1000)', /readiness.*timed out/i],
  ['oversized readiness', "process.stdout.write('x'.repeat(70 * 1024)); setInterval(() => {}, 1000)", /readiness.*too large/i],
  ['invalid port', "process.stdout.write(JSON.stringify({type:'ready',port:0})+'\\n'); setInterval(() => {}, 1000)", /invalid syncd readiness/i],
]) {
  test(`startup cleans up ${name} before rejecting`, async (t) => {
    const soak = await startupFixture(t, body);
    await assert.rejects(boundedStart(soak), expected);
    assert.ok(soak.daemon.exitCode !== null || soak.daemon.signalCode !== null,
      'readiness failure must reap the child before the caller continues');
  });
}

test('startup accepts fragmented readiness and drains later stdout', async (t) => {
    const soak = await startupFixture(t,
      `process.stdout.write('{"type":"ready",');
       setTimeout(() => process.stdout.write('"port":12345}\\n'), 10);
       setInterval(() => process.stdout.write('diagnostic\\n'), 10);`);
    assert.equal(await boundedStart(soak), 12345);
    assert.equal(soak.port, 12345);
    assert.equal(soak.daemon.listenerCount('error'), 0);
  });

test('fast completed writes yield so a queued stop can interrupt streaming', async (t) => {
  const soak = new ProtocolSoak({ daemonPath: '', width: 8, height: 8 });
  soak.data = { closed: false, socket: { writableLength: 0, destroyed: false },
    sendBinary: async () => {} };
  soak._openSession = async () => {};
  soak._closeSession = async () => {};
  soak._stats = async () => {};
  // Keep the loop bound independent of host load, and queue the stop before
  // the loop's own setImmediate so one event-loop turn suffices to observe it.
  let clockReads = 0;
  t.mock.method(Date, 'now', () => Math.floor(clockReads++ / 16));
  const stop = setImmediate(() => { soak.stopped = true; });
  t.after(() => clearImmediate(stop));

  await soak.run(100);

  assert.equal(soak.stopped, true, 'the queued stop must run before the streaming loop completes');
  assert.ok(soak.state.sentFrames > 0);
});

test('summarise ignores the warm-up window when measuring growth', () => {
  const samples = [sample(0, 50_000), sample(1, 90_000), sample(2, 20_000),
    sample(3, 20_100), sample(4, 20_200)];
  const result = summarise(samples, { warmupFraction: 0.25 });
  assert.equal(result.first.t, 1);
  assert.equal(result.last.t, 4);
  assert.equal(result.peak, 90_000);
  assert.equal(result.sampleCount, 4);
  // The warm window is [90_000, 20_000, 20_100, 20_200]: a series sitting flat
  // at ~20 MB whose first reading is a 90 MB spike. Taking that one sample as
  // the baseline would report -69,800 KiB of "shrinkage" on a flat run.
  assert.equal(result.endpointWindow, 2);
  assert.equal(result.growthKb, 20_150 - 55_000);
});

test('one cycle-gap sample on an endpoint cannot move the growth verdict', () => {
  // Reproduces what LARGEBOI's Windows run actually produced: a daemon flat at
  // 11_180 KiB for the whole window, with single samples at two cycle
  // boundaries reading 3056 KiB because the sampler landed between one sender
  // closing and the next opening. One of them falls on the very last sample.
  const flat = [];
  for (let t = 0; t < 40; t += 1) flat.push(sample(t, 11_180));
  flat[20] = sample(20, 3056);
  flat[39] = sample(39, 3056);
  const result = summarise(flat, { warmupFraction: 0.25 });

  assert.equal(result.endpointWindow, 5);
  assert.equal(result.last.footprint, 3056, 'the raw endpoint sample is still reported');
  assert.equal(result.lastKb, 11_180, 'but the verdict uses the median of the last five');
  assert.equal(result.growthKb, 0, 'a flat daemon must report flat, not -8124 KiB');
});

test('summarise keeps at least two samples even for a tiny series', () => {
  const result = summarise([sample(0, 10), sample(1, 20)], { warmupFraction: 0.9 });
  assert.equal(result.sampleCount, 2);
  // Two samples leave one at each end; the endpoints cannot overlap.
  assert.equal(result.endpointWindow, 1);
  assert.equal(result.growthKb, 10);
});

test('summarise rejects an empty series rather than reporting a false flat line', () => {
  assert.throws(() => summarise([], { warmupFraction: 0.25 }), /at least two samples/);
});

// stop() is invoked unconditionally from run.mjs's finally, including on
// failure paths where the daemon's state is least predictable. A fake child
// that ignores SIGTERM stands in for a wedged real daemon (stuck in a
// syscall, or otherwise not honouring the signal) without needing an actual
// unkillable process for the test to spawn.
class DeafChild extends EventEmitter {
  constructor() {
    super();
    this.pid = 999999;
    this.exitCode = null;
    this.signalCode = null;
    this.signals = [];
  }

  kill(signal) {
    this.signals.push(signal);
    if (signal === 'SIGKILL') {
      // Only SIGKILL "works" here — real SIGKILL cannot be ignored by a
      // process either, which is exactly why stop() must escalate to it.
      setImmediate(() => {
        this.exitCode = null;
        this.signalCode = 'SIGKILL';
        this.emit('exit', null, 'SIGKILL');
      });
    }
    return true;
  }
}

// A fake stand-in for a ChildProcess's piped stdout/stderr, real enough to
// prove _abandonDaemon() actually lets go of it: start() attaches a 'data'
// listener to the daemon's stderr for the whole run, and a real Readable
// piped stream keeps its handle (and this process's event loop) referenced
// for as long as something is listening to it.
function fakePipe() {
  const pipe = new EventEmitter();
  pipe.destroyed = false;
  pipe.destroy = () => { pipe.destroyed = true; };
  return pipe;
}

class UnkillableChild extends EventEmitter {
  constructor() {
    super();
    this.pid = 1;
    this.exitCode = null;
    this.signalCode = null;
    this.signals = [];
    this.unrefCalled = false;
    this.stdout = fakePipe();
    this.stderr = fakePipe();
  }

  kill(signal) {
    this.signals.push(signal);
    return true; // never exits, no matter what it is sent
  }

  unref() {
    this.unrefCalled = true;
  }
}

// Models the boundary race the reviewer flagged: the process actually exits
// partway through the SIGTERM wait, but — as if its 'exit' event and the
// wait's own timeout callback contended for the same event-loop turn and the
// timeout won — no 'exit' event is ever observed here, only the
// exitCode/signalCode fields land (the same way a real child sets them
// synchronously with the event it emits, but the emission itself is what
// gets missed by an already-detached listener).
class MutedExitChild extends EventEmitter {
  constructor() {
    super();
    this.pid = 4;
    this.exitCode = null;
    this.signalCode = null;
    this.signals = [];
  }

  kill(signal) {
    this.signals.push(signal);
    if (signal === 'SIGTERM') {
      setTimeout(() => {
        this.exitCode = 0;
        this.signalCode = null;
      }, 5); // lands inside the 20ms SIGTERM wait, well before it times out
    }
    return true;
  }
}

// A fake stand-in for the net.Socket backing a RawWebSocket (this.control /
// this.data). Live, non-destroyed sockets are exactly what the reviewer
// confirmed keep the event loop alive indefinitely on their own, same as an
// abandoned child handle or its piped stdio.
function fakeConnection() {
  const socket = new EventEmitter();
  socket.destroyed = false;
  socket.destroy = () => { socket.destroyed = true; };
  return { socket, closed: false };
}

function fakeSoak(overrides = {}) {
  return new ProtocolSoak({
    daemonPath: '/does/not/matter', origin: 'https://soak.example', token: 'soak-token-123',
    stopTermTimeoutMs: 20, stopKillTimeoutMs: 50, ...overrides,
  });
}

test('stop() escalates to SIGKILL and still returns promptly when SIGTERM is ignored', async () => {
  const soak = fakeSoak();
  soak.daemon = new DeafChild();
  const started = Date.now();
  const result = await soak.stop();
  const elapsed = Date.now() - started;
  assert.deepEqual(soak.daemon.signals, ['SIGTERM', 'SIGKILL']);
  assert.equal(result.reaped, true);
  assert.equal(result.exitCode, null);
  // Bounded by stopTermTimeoutMs (20) + stopKillTimeoutMs (50), not by any
  // unbounded await — well under a real SIGTERM grace period.
  assert.ok(elapsed < 1000, `stop() took ${elapsed}ms, expected it to resolve promptly`);
  // The caller (run.mjs's finally) unconditionally spreads this result into
  // its JSONL summary line right after awaiting stop() — proving stop()
  // resolves with a well-formed, spreadable object is what guarantees that
  // write, and the flush after it, is actually reached.
  const summaryLine = { plane: 'protocol', type: 'summary', ...result };
  assert.doesNotThrow(() => JSON.stringify(summaryLine));
});

test('stop() gives up and still returns a usable result when the child never dies', async () => {
  const soak = fakeSoak();
  soak.daemon = new UnkillableChild();
  soak.daemon.stdout.on('data', () => {}); // the long-lived listeners start() attaches
  soak.daemon.stderr.on('data', () => {});
  // this.control/this.data are the live sockets _openSession() leaves open;
  // _closeSession() is the only other thing that ever closes them, and it
  // never runs if run() threw its way here (e.g. a nextJson() timeout
  // because the same daemon that won't die also stopped answering them).
  const control = fakeConnection();
  const data = fakeConnection();
  soak.control = control;
  soak.data = data;
  const started = Date.now();
  const result = await soak.stop();
  const elapsed = Date.now() - started;
  assert.deepEqual(soak.daemon.signals, ['SIGTERM', 'SIGKILL']);
  assert.equal(result.reaped, false);
  assert.equal(result.exitCode, null);
  assert.ok(elapsed < 1000, `stop() took ${elapsed}ms, expected it to give up promptly`);
  const summaryLine = { plane: 'protocol', type: 'summary', ...result };
  assert.doesNotThrow(() => JSON.stringify(summaryLine));
  // The hang this round closes moved up a layer: a genuinely unkillable
  // daemon must not keep this process's event loop alive via the abandoned
  // child handle, its piped stdio, or any socket still connected to it,
  // once the flush this result feeds is done.
  assert.equal(soak.daemon.unrefCalled, true, 'stop() must unref an unkillable child');
  assert.equal(soak.daemon.stdout.destroyed, true);
  assert.equal(soak.daemon.stderr.destroyed, true);
  assert.equal(soak.daemon.stdout.listenerCount('data'), 0);
  assert.equal(soak.daemon.stderr.listenerCount('data'), 0);
  assert.equal(control.socket.destroyed, true, 'stop() must destroy the control socket');
  assert.equal(data.socket.destroyed, true, 'stop() must destroy the data socket');
  assert.equal(soak.control, null, 'stop() must dereference control after abandoning it');
  assert.equal(soak.data, null, 'stop() must dereference data after abandoning it');
});

test('stop() observes an exit that landed during a previous wait instead of racing past it', async () => {
  const soak = fakeSoak(); // stopTermTimeoutMs: 20, stopKillTimeoutMs: 50
  soak.daemon = new MutedExitChild();
  const started = Date.now();
  const result = await soak.stop();
  const elapsed = Date.now() - started;
  // The SIGTERM wait times out at 20ms since no 'exit' event ever fires, so
  // a SIGKILL still gets sent (harmless, and the reviewer flagged this as an
  // acceptable side effect) — but the following wait must recognize the
  // exit that actually landed at 5ms via exitCode/signalCode, rather than
  // waiting out its own full 50ms stopKillTimeoutMs and wrongly reporting a
  // false reaped:false on a process that was already gone.
  assert.deepEqual(soak.daemon.signals, ['SIGTERM', 'SIGKILL']);
  assert.equal(result.reaped, true);
  assert.equal(result.exitCode, 0);
  assert.ok(elapsed < 45,
    `stop() took ${elapsed}ms; expected it to short-circuit well before the 50ms SIGKILL bound`);
});

test('_waitForExit\'s pre-check catches signalCode alone, without exitCode also being set', async () => {
  // A child killed by a signal (real or SIGKILL) always has exitCode === null
  // and signalCode === the signal name — never the reverse. This is the
  // exact shape of the trap that has bitten this project three times: code
  // that only checks exitCode, or only reaches the live-event path, misses a
  // signal-killed child. Exercise the pre-check directly (not via stop()'s
  // top-of-function short-circuit, and not via a live 'exit' event) so a
  // regression that dropped the signalCode half of the OR fails loudly here.
  const soak = fakeSoak();
  soak.daemon = new EventEmitter();
  soak.daemon.exitCode = null;
  soak.daemon.signalCode = 'SIGTERM';
  const started = Date.now();
  const exitCode = await soak._waitForExit(50);
  const elapsed = Date.now() - started;
  assert.equal(exitCode, null);
  assert.ok(elapsed < 10,
    `_waitForExit took ${elapsed}ms; the pre-check should resolve immediately, not wait out the 50ms bound`);
});

// Windows has no POSIX signals: Node maps kill('SIGTERM') onto
// TerminateProcess, so a daemon that stopped exactly as asked reports exitCode
// null and signalCode 'SIGTERM'. stop() must surface the signal, or a caller
// checking only `exitCode !== 0` fails every healthy Windows run — which is
// precisely what run.mjs was doing.
test('stop() reports signalCode alongside exitCode for a signal-terminated daemon', async () => {
  const soak = new ProtocolSoak({ daemonPath: '/nonexistent' });
  soak.state = { sentFrames: 1, dropped: 0, reconnects: 0 };
  // A daemon that has already gone, the way TerminateProcess leaves it.
  soak.daemon = { exitCode: null, signalCode: 'SIGTERM', pid: 4242 };
  const result = await soak.stop();
  assert.equal(result.exitCode, null);
  assert.equal(result.signalCode, 'SIGTERM');
  assert.equal(result.reaped, true);
});

test('stop() reports a clean exit with no signal on the ordinary path', async () => {
  const soak = new ProtocolSoak({ daemonPath: '/nonexistent' });
  soak.state = { sentFrames: 1, dropped: 0, reconnects: 0 };
  soak.daemon = { exitCode: 0, signalCode: null, pid: 4242 };
  const result = await soak.stop();
  assert.equal(result.exitCode, 0);
  assert.equal(result.signalCode, null);
  assert.equal(result.reaped, true);
});

// Geometry churn on the protocol plane. The browser soak's format leg rotates
// the canvas and a 4.2x throughput collapse followed it; whether that lives in
// the daemon or above it cannot be answered from a browser host, because both
// have a renderer, a camera sink and a GL readback in the path. This plane has
// none of those.
test('ProtocolSoak rotates geometry and reuses one buffer per size', () => {
  const soak = new ProtocolSoak({
    daemonPath: 'unused', origin: 'https://soak.example', token: 't',
    geometries: [[1280, 720], [1920, 1080], [2560, 1440]], geometryEveryMs: 90_000,
  });
  assert.deepEqual(soak.geometry, { width: 1280, height: 720 });
  const first = soak.frame;
  assert.deepEqual(soak.advanceGeometry(), { width: 1920, height: 1080 });
  assert.notEqual(soak.frame, first, 'a different size gets a different buffer');
  soak.advanceGeometry();
  assert.deepEqual(soak.advanceGeometry(), { width: 1280, height: 720 }, 'wraps');
  assert.equal(soak.frame, first, 'and returns the SAME buffer, not a fresh allocation');
});

// A budget that shrank with the current frame would throttle the small
// geometries differently from the large ones, and the rotation would then be
// measuring the harness's own backpressure policy rather than the daemon.
test('ProtocolSoak sizes its buffer budget by the largest geometry in the rotation', () => {
  const soak = new ProtocolSoak({
    daemonPath: 'unused', origin: 'https://soak.example', token: 't',
    geometries: [[1280, 720], [2560, 1440]], geometryEveryMs: 1000,
  });
  const largest = soak._frameFor({ width: 2560, height: 1440 }).length;
  assert.equal(soak.maxBuffered, 3 * largest);
  soak.advanceGeometry();
  assert.equal(soak.maxBuffered, 3 * largest, 'and it does not move when geometry does');
});

test('ProtocolSoak without a rotation behaves exactly as it always did', () => {
  const soak = new ProtocolSoak({
    daemonPath: 'unused', origin: 'https://soak.example', token: 't',
    width: 1920, height: 1080,
  });
  assert.deepEqual(soak.geometry, { width: 1920, height: 1080 });
  assert.equal(soak.maxBuffered, 3 * soak.frame.length);
  // advanceGeometry on a single-entry rotation is a no-op, not a crash: the
  // run loop guards on length > 1, and the method must agree with that guard.
  assert.deepEqual(soak.advanceGeometry(), { width: 1920, height: 1080 });
});
