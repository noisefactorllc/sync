// Plane 1 of the soak gauntlet: the multi-hour protocol engine. Streams
// real-size frames through a --test-receiver syncd for an arbitrary
// duration, cycles the sender/session lifecycle, samples memory, and checks
// for leaks at intervals. The streaming loop, session open/close, and stats
// round trip mirror test/acceptance/daemon-memory-soak.mjs; the differences
// here are that duration is a parameter, samples go to a callback instead of
// an in-memory array, and leaks run periodically rather than once at exit.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setImmediate as yieldToEventLoop } from 'node:timers/promises';
import { upgrade } from './lib/ws.mjs';
import { createFrameBuffer, stampFrame } from './lib/frame.mjs';
import { residentKb, footprintKb, runLeaks } from './lib/process-metrics.mjs';

// How many samples at each end of the warm window vote on the endpoint
// footprint. Growth used to be one sample minus one sample, which made the
// whole eight-hour verdict hostage to two readings.
//
// The sampler is periodic and the sender lifecycle is not, so a sample
// occasionally lands in the brief gap between one sender closing and the next
// opening — where the daemon has released its per-sender buffers and reads
// about 8 MB lighter than it does while streaming. Observed on LARGEBOI: two
// samples out of 3210, both on cycle boundaries, reading 3056 KiB against a
// p50 of 11168. Rare, but if one lands on an endpoint it moves growth by the
// full 8 MB — a phantom leak at the start of the window, or a phantom
// improvement at the end, on a run that was flat throughout.
//
// Five is enough to outvote the at most one gap sample a cycle can produce,
// and short enough to stay a genuine endpoint over a multi-hour window.
const ENDPOINT_WINDOW = 5;
const FRAMES_PER_TURN = 64;

function median(values) {
  const sorted = [...values].sort((a, b) => a - b);
  const middle = sorted.length >> 1;
  return sorted.length % 2 === 1
    ? sorted[middle]
    : (sorted[middle - 1] + sorted[middle]) / 2;
}

export function summarise(samples, { warmupFraction = 0.25 } = {}) {
  if (!Array.isArray(samples) || samples.length < 2) {
    throw new RangeError('summarise needs at least two samples');
  }
  const span = samples[samples.length - 1].t - samples[0].t;
  const cutoff = samples[0].t + span * warmupFraction;
  let warm = samples.filter((sample) => sample.t >= cutoff);
  if (warm.length < 2) warm = samples.slice(-2);
  const first = warm[0];
  const last = warm[warm.length - 1];

  // Never let the two ends overlap: on a short series that would compare a
  // window against itself and report a flat line no matter what happened.
  const endpointWindow = Math.max(1, Math.min(ENDPOINT_WINDOW, warm.length >> 1));
  const footprints = warm.map((sample) => sample.footprint);
  const firstKb = median(footprints.slice(0, endpointWindow));
  const lastKb = median(footprints.slice(-endpointWindow));

  return {
    first,
    last,
    // The medians the verdict actually rests on, reported so a surprising
    // growth number can be checked against them without the raw series.
    firstKb,
    lastKb,
    endpointWindow,
    peak: Math.max(...footprints),
    growthKb: lastKb - firstKb,
    sampleCount: warm.length,
  };
}

export class ProtocolSoak {
  constructor({ daemonPath, origin, token, width = 1920, height = 1080,
                cycleMs = 60_000, leaksEveryMs = 900_000, onSample = () => {},
                startupTimeoutMs = 5_000,
                stopTermTimeoutMs = 5_000, stopKillTimeoutMs = 2_000 }) {
    Object.assign(this, { daemonPath, origin, token, width, height, cycleMs,
                          leaksEveryMs, onSample, startupTimeoutMs,
                          stopTermTimeoutMs, stopKillTimeoutMs });
    this.frame = createFrameBuffer({ width, height });
    this.maxBuffered = 3 * this.frame.length;
    this.state = { sentFrames: 0, cycles: 0, accepted: 0, dropped: 0,
                   sequence: 0, reconnects: 0, stderr: '' };
    this.stopped = false;
    this._port = null;
  }

  get port() {
    return this._port;
  }

  async start() {
    this.daemon = spawn(this.daemonPath,
      ['--port', '0', '--test-origin', this.origin, '--test-token', this.token,
       '--test-receiver'], { stdio: ['ignore', 'pipe', 'pipe'] });
    this.daemon.stderr.setEncoding('utf8');
    this.daemon.stderr.on('data', (chunk) => {
      this.state.stderr = (this.state.stderr + chunk).slice(-8192);
    });
    try {
      const ready = await new Promise((resolve, reject) => {
        let stdout = '';
        const finish = (error, record) => {
          clearTimeout(timer);
          this.daemon.stdout.off('data', onStdout);
          this.daemon.stdout.off('end', onEnd);
          this.daemon.off('exit', onExit);
          this.daemon.off('error', onError);
          // Continue draining diagnostics without retaining them after ready.
          this.daemon.stdout.resume();
          if (error) reject(error);
          else resolve(record);
        };
        const onStdout = (chunk) => {
          stdout += chunk;
          const newline = stdout.indexOf('\n');
          const line = newline < 0 ? stdout : stdout.slice(0, newline);
          if (Buffer.byteLength(line) > 64 * 1024) {
            finish(new Error('syncd readiness record is too large'));
            return;
          }
          if (newline < 0) return;
          try {
            const record = JSON.parse(line);
            if (record?.type !== 'ready' || !Number.isInteger(record.port)
                || record.port < 1 || record.port > 65535) {
              throw new Error('invalid type or port');
            }
            finish(null, record);
          } catch (error) {
            finish(new Error(`invalid syncd readiness: ${error.message}`));
          }
        };
        const onError = (error) => finish(error);
        const onExit = (code) => finish(new Error(
          `syncd exited before readiness with ${code}: ${this.state.stderr}`));
        const onEnd = () => finish(new Error('syncd stdout ended before readiness'));
        const timer = setTimeout(() => finish(new Error('syncd readiness timed out')),
          this.startupTimeoutMs);
        this.daemon.stdout.setEncoding('utf8');
        this.daemon.stdout.on('data', onStdout);
        this.daemon.stdout.once('end', onEnd);
        this.daemon.once('exit', onExit);
        this.daemon.once('error', onError);
      });
      this._port = ready.port;
      return this._port;
    } catch (error) {
      // Failure must settle only after the spawned child is stopped; callers
      // cannot otherwise clean up malformed output or a failed exec reliably.
      await this.stop();
      throw error;
    }
  }

  async _openSession() {
    this.control = await upgrade(this.port, '/control', { origin: this.origin });
    this.control.sendText(JSON.stringify({
      type: 'hello', token: this.token, protocolVersions: [1] }));
    const welcome = await this.control.nextJson();
    assert.equal(welcome.type, 'welcome');
    this.control.sendText(JSON.stringify({
      type: 'createSender', name: `Soak ${this.state.cycles}` }));
    const created = await this.control.nextJson();
    assert.equal(created.type, 'senderCreated', JSON.stringify(created));
    this.senderId = created.id;
    this.data = await upgrade(this.port, created.path,
      { origin: this.origin, subprotocol: `sync.sender.${created.ticket}` });
    this.state.cycles += 1;
  }

  async _closeSession() {
    if (this.control && !this.control.closed) {
      this.control.sendText(JSON.stringify({
        type: 'closeSender', senderId: this.senderId }));
      await this.control.nextJson().catch(() => null);
      await this.control.close();
    }
    if (this.data && !this.data.closed) await this.data.close();
    this.control = null;
    this.data = null;
  }

  async _stats() {
    if (!this.control || this.control.closed) return;
    this.control.sendText(JSON.stringify({
      type: 'getStats', senderId: this.senderId }));
    const reply = await this.control.nextJson();
    if (reply.type === 'stats') {
      this.state.accepted = reply.accepted;
      this.state.dropped = reply.dropped;
    }
  }

  async run(durationMs) {
    const started = Date.now();
    let lastCycle = started;
    let lastSample = 0;
    let lastLeaks = started;
    await this._openSession();
    while (!this.stopped && Date.now() - started < durationMs) {
      const now = Date.now();
      if (now - lastCycle >= this.cycleMs) {
        await this._stats();
        await this._closeSession();
        await this._openSession();
        lastCycle = now;
      }
      const elapsed = Math.round((now - started) / 1000);
      if (elapsed > lastSample) {
        lastSample = elapsed;
        await this._stats();
        let leaks = null;
        if (now - lastLeaks >= this.leaksEveryMs) {
          leaks = runLeaks(this.daemon.pid);
          lastLeaks = now;
        }
        this.onSample({
          plane: 'protocol', t: elapsed, pid: this.daemon.pid,
          footprint: footprintKb(this.daemon.pid), rss: residentKb(this.daemon.pid),
          frames: this.state.sentFrames, accepted: this.state.accepted,
          dropped: this.state.dropped, cycles: this.state.cycles,
          reconnects: this.state.reconnects,
          leaks: leaks && leaks.leaks,
        });
      }
      // A dead data socket must never look like a healthy stream: `close`
      // may not have fired yet and a destroyed socket reads writableLength
      // 0, which would otherwise slip straight past the backpressure check
      // below and spin, incrementing counters against nothing until the next
      // scheduled cycle happens to repair it. Recycle the session the moment
      // either signal shows up, rather than waiting for that timer.
      if (!this.data || this.data.closed || this.data.socket.destroyed) {
        this.state.reconnects += 1;
        await this._closeSession();
        await this._openSession();
        lastCycle = Date.now();
        continue;
      }
      if (this.data.socket.writableLength > this.maxBuffered) {
        await new Promise((resolve) => setTimeout(resolve, 2));
        continue;
      }
      this.state.sequence += 1;
      try {
        await this.data.sendBinary(
          stampFrame(this.frame, this.state.sequence, BigInt(Date.now()) * 1000n));
        this.state.sentFrames += 1;
        // Small writes may complete entirely through nextTick callbacks.
        // Give timers and shutdown signals a turn even without backpressure.
        if (this.state.sentFrames % FRAMES_PER_TURN === 0) await yieldToEventLoop();
      } catch {
        // The write callback reported an error (e.g. ECONNRESET) after the
        // proactive check above passed — the socket died mid-send. Recycle
        // rather than spin retrying against a socket that will not recover.
        this.state.reconnects += 1;
        await this._closeSession();
        await this._openSession();
        lastCycle = Date.now();
      }
    }
    await this._stats();
    await this._closeSession();
  }

  // Waits up to timeoutMs for the daemon's 'exit' event. Resolves with the
  // exit code on exit, or the ABANDONED sentinel if the bound elapses first
  // — never hangs past timeoutMs regardless of what the child does.
  //
  // A child's 'exit' event fires exactly once in its lifetime. If it already
  // fired in an earlier wait window — e.g. the daemon died right at the
  // boundary of the previous timeout, an instant before that timer's own
  // callback ran and detached its listener — a fresh `.once('exit', ...)`
  // registered here would never fire again, and this would wrongly wait out
  // the full timeout before reporting ABANDONED, on a process that is
  // actually already gone. `exitCode`/`signalCode` are set synchronously at
  // the same moment the event is emitted, so check those first rather than
  // trusting only a live event to observe an exit that already happened.
  _waitForExit(timeoutMs) {
    if (this.daemon.exitCode !== null || this.daemon.signalCode !== null) {
      return Promise.resolve(this.daemon.exitCode);
    }
    return new Promise((resolve) => {
      const onExit = (code) => {
        clearTimeout(timer);
        resolve(code);
      };
      const timer = setTimeout(() => {
        this.daemon.off('exit', onExit);
        resolve(ProtocolSoak.ABANDONED);
      }, timeoutMs);
      this.daemon.once('exit', onExit);
    });
  }

  // A daemon that survived SIGTERM and SIGKILL is wedged (e.g. stuck in an
  // uninterruptible syscall) and will never emit 'exit'. Node keeps this
  // process's own event loop alive for as long as the child handle, its
  // piped stdio, or any other socket connected to it remain referenced and
  // open — start() attaches a 'data' listener to the daemon's stderr for the
  // whole run, and _openSession() leaves this.control/this.data connected
  // for as long as the daemon keeps servicing them. _closeSession() is the
  // only other thing that ever closes those two, and it only runs at run()'s
  // clean tail or from the in-loop recycle branches — never reached if run()
  // threw its way here (e.g. a control nextJson() timing out because the
  // daemon stopped responding, which is exactly the daemon most likely to
  // also ignore SIGKILL). So without this, the harness process hangs forever
  // on a daemon it has already given up on, even after its own JSONL output
  // is flushed and correct. Let go of everything still attached to it here,
  // in one place, so a caller has nothing left keeping the event loop open.
  _abandonDaemon() {
    this.daemon.unref?.();
    for (const pipe of [this.daemon.stdout, this.daemon.stderr]) {
      pipe?.removeAllListeners?.('data');
      pipe?.destroy?.();
    }
    for (const conn of [this.control, this.data]) {
      conn?.socket?.destroy?.();
    }
    this.control = null;
    this.data = null;
  }

  async stop() {
    this.stopped = true;
    if (!this.daemon) {
      return { ...this.state, exitCode: null, signalCode: null, reaped: true };
    }
    // If the daemon already exited (e.g. it crashed and that is why run()
    // threw), `exitCode`/`signalCode` are set synchronously when the 'exit'
    // event fires, so a fresh `.once('exit', ...)` here would never resolve
    // — that event already happened. Short-circuit instead of hanging.
    if (this.daemon.exitCode !== null || this.daemon.signalCode !== null) {
      return { ...this.state, exitCode: this.daemon.exitCode,
        signalCode: this.daemon.signalCode, reaped: true };
    }
    // stop() used to be reached only on the happy path, where an await with
    // no timeout was safe because the daemon was known-alive and responsive.
    // It is now called unconditionally from run.mjs's finally, including on
    // failure paths where the daemon's state is least predictable — a wedged
    // or signal-ignoring process must never make this hang, because that
    // would swallow the error record, the summary, and the exit code it
    // exists to guarantee. So: bound SIGTERM, escalate to a bounded SIGKILL,
    // and if the process still will not die, give up and say so rather than
    // block the caller forever.
    // `signalCode` is reported alongside `exitCode` because on Windows they are
    // the only way to tell the two apart: Node maps kill('SIGTERM') onto
    // TerminateProcess, so a daemon that stopped exactly as asked reports
    // exitCode null and signalCode 'SIGTERM'. A caller checking only
    // `exitCode !== 0` would fail every healthy Windows run.
    this.daemon.kill('SIGTERM');
    let exitCode = await this._waitForExit(this.stopTermTimeoutMs);
    if (exitCode !== ProtocolSoak.ABANDONED) {
      return { ...this.state, exitCode, signalCode: this.daemon.signalCode, reaped: true };
    }

    this.daemon.kill('SIGKILL');
    exitCode = await this._waitForExit(this.stopKillTimeoutMs);
    if (exitCode !== ProtocolSoak.ABANDONED) {
      return { ...this.state, exitCode, signalCode: this.daemon.signalCode, reaped: true };
    }

    // Genuinely unkillable (or already reaped by something else out from
    // under us). The flush still happens — run.mjs writes the summary and
    // ends its stream from the result this returns — this only stops the
    // abandoned child from keeping that process alive afterward.
    this._abandonDaemon();
    return { ...this.state, exitCode: null, signalCode: null, reaped: false };
  }
}

ProtocolSoak.ABANDONED = Symbol('stop-timed-out');
