import assert from 'node:assert/strict';
import test from 'node:test';
import { EventEmitter } from 'node:events';
import { summarise, ProtocolSoak } from './engine.mjs';

const sample = (t, footprint) => ({ t, footprint, rss: footprint, frames: t * 60 });

test('summarise ignores the warm-up window when measuring growth', () => {
  const samples = [sample(0, 50_000), sample(1, 90_000), sample(2, 20_000),
    sample(3, 20_100), sample(4, 20_200)];
  const result = summarise(samples, { warmupFraction: 0.25 });
  assert.equal(result.first.t, 1);
  assert.equal(result.last.t, 4);
  assert.equal(result.growthKb, 20_200 - 90_000);
  assert.equal(result.peak, 90_000);
  assert.equal(result.sampleCount, 4);
});

test('summarise keeps at least two samples even for a tiny series', () => {
  const result = summarise([sample(0, 10), sample(1, 20)], { warmupFraction: 0.9 });
  assert.equal(result.sampleCount, 2);
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
