import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { ProtocolSoak } from '../soak/engine.mjs';
import { residentKbAsync, footprintKbAsync } from '../soak/lib/process-metrics.mjs';

const ROOT = fileURLToPath(new URL('../../', import.meta.url));
// The daemon's built location is generator-dependent, not just
// platform-dependent: Ninja and Make put it at build/syncd, the Visual Studio
// generator at build/Release/syncd.exe. Checking only the first meant both
// tests SKIPPED on Windows and the run reported "skipped 2" — which reads as
// deliberate, so nobody looks. They pass there when actually pointed at a
// build. A path-shape assumption wearing a platform guard's clothes.
const DAEMON_CANDIDATES = ['build/syncd', 'build/syncd.exe',
  'build/Release/syncd.exe', 'build/Debug/syncd.exe'];
function resolveDaemon() {
  if (process.env.SYNC_DAEMON_PATH) return path.resolve(ROOT, process.env.SYNC_DAEMON_PATH);
  for (const candidate of DAEMON_CANDIDATES) {
    const resolved = path.resolve(ROOT, candidate);
    if (existsSync(resolved)) return resolved;
  }
  return path.resolve(ROOT, DAEMON_CANDIDATES[0]);
}
const DAEMON = resolveDaemon();
// CI must never turn a missing build into a green skipped suite.
assert.ok(existsSync(DAEMON), `build syncd first or set SYNC_DAEMON_PATH: ${DAEMON}`);
const requiresDaemon = { timeout: 25_000 };

async function shortRun(t, { delayedHealthFailure = false, fps = 60, cycle = 60 } = {}) {
  const args = ['--input-type=module', '-e', `
    import { registerHooks } from 'node:module';
    // These cases test duration and health completion ordering. Slow
    // inspectors can consume the entire five-second window or mask a missing
    // health join. Real process inspection is checked separately below.
    registerHooks({ load(url, context, nextLoad) {
      if (url === ${JSON.stringify(new URL('../soak/lib/process-metrics.mjs', import.meta.url).href)}) {
        return { format: 'module', shortCircuit: true, source:
          'export const residentKb = () => 1024; ' +
          'export const footprintKb = () => 1024; ' +
          'export const residentKbAsync = async () => 1024; ' +
          'export const footprintKbAsync = async () => 1024; ' +
          'export const runLeaks = () => { throw new Error("unexpected leak scan"); };' };
      }
      return nextLoad(url, context);
    } });
    if (${delayedHealthFailure}) {
      const healthModule = ${JSON.stringify(new URL('../soak/lib/health.mjs', import.meta.url).href)};
      registerHooks({ load(url, context, nextLoad) {
        if (url === healthModule) {
          return { format: 'module', shortCircuit: true, source: ${JSON.stringify(`
            let previous;
            export function probeHealth() {
              if (previous) {
                clearTimeout(previous.timer);
                previous.resolve();
              }
              return new Promise((resolve, reject) => {
                previous = { resolve, timer: setTimeout(() => {
                  reject(new Error('late health probe failure'));
                }, 1800) };
              });
            }
          `)} };
        }
        return nextLoad(url, context);
      } });
    }
    await import(${JSON.stringify(new URL('../acceptance/daemon-memory-soak.mjs', import.meta.url).href)});
  `];
  const child = spawn(process.execPath, args, {
    cwd: ROOT,
    env: { ...process.env, SYNC_DAEMON_PATH: DAEMON, SYNC_SOAK_SECONDS: '5',
      SYNC_SOAK_CYCLE: String(cycle), SYNC_SOAK_WIDTH: '8', SYNC_SOAK_FPS: String(fps),
      SYNC_SOAK_HEIGHT: '8', SYNC_SOAK_GROWTH_KB: '1000000',
      SYNC_SOAK_LEAKS: '0' },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let stdout = '';
  let stderr = '';
  let timedOut = false;
  let daemonPid;
  child.stdout.on('data', (chunk) => {
    stdout += chunk;
    daemonPid ??= Number(/syncd pid=(\d+)/.exec(stdout)?.[1]) || undefined;
  });
  child.stderr.on('data', (chunk) => { stderr += chunk; });
  const terminate = () => {
    if (child.exitCode !== null || child.signalCode !== null) return;
    child.kill('SIGKILL');
    if (daemonPid !== undefined) {
      try { process.kill(daemonPid, 'SIGKILL'); } catch {}
    }
  };
  // Includes bounded startup, final asynchronous inspection and shutdown;
  // still fails well before the sixty-second cycle of the starvation bug.
  const timer = setTimeout(() => { timedOut = true; terminate(); }, 20_000);
  t.after(() => { clearTimeout(timer); terminate(); });
  const result = await new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('close', (code, signal) => resolve({ code, signal }));
  });
  clearTimeout(timer);
  assert.equal(timedOut, false, `five-second soak exceeded twenty seconds:\n${stdout}\n${stderr}`);
  return { ...result, stdout, stderr };
}

test('a five-second paced soak ends before its sixty-second sender cycle',
  requiresDaemon, async (t) => {
    const result = await shortRun(t);
    assert.equal(result.code, 0, `${result.stdout}\n${result.stderr}`);
    assert.match(result.stdout, /cycles=1 sent=/);
  });

test('a final pending health failure prevents a successful soak verdict',
  requiresDaemon, async (t) => {
    const result = await shortRun(t, { delayedHealthFailure: true });
    assert.equal(result.code, 1, result.stdout);
    assert.match(result.stderr, /late health probe failure/, result.stdout);
  });

test('a paced soak cycles senders and stops while waiting for its next frame',
  requiresDaemon, async (t) => {
    const result = await shortRun(t, { fps: 0.01, cycle: 2 });
    assert.equal(result.code, 0, `${result.stdout}\n${result.stderr}`);
    assert.match(result.stdout, /cycles=[2-9]\d* sent=1 /);
  });

test('process inspectors read real daemon resident memory and Windows private memory', {
  // Includes the 60s command budget, startup and joined child cleanup.
  timeout: 75_000,
}, async (t) => {
  const lifecycle = new ProtocolSoak({ daemonPath: DAEMON, width: 8, height: 8,
    origin: 'https://soak.example', token: 'soak-token-123' });
  t.after(() => lifecycle.stop());
  await lifecycle.start();
  // Keep real inspection independent of streaming. Concurrent cold PowerShell
  // startups have exceeded 10s in CI; this fixture has a 60s command budget
  // inside its 75s test. Join both reads before cleanup and reject a missing
  // private-memory reading instead of silently substituting resident memory.
  const timeoutMs = 60_000;
  const reads = [
    residentKbAsync(lifecycle.daemon.pid, { timeoutMs }),
  ];
  if (process.platform === 'win32') {
    reads.push(footprintKbAsync(lifecycle.daemon.pid, { timeoutMs, fallback: () => {
      throw new Error('the real Windows private-memory inspection failed');
    } }));
  }
  const results = await Promise.allSettled(reads);
  // Report target lifetime separately from inspector failure. Empty output
  // is not a valid measurement even if the inspector process exits zero.
  const exited = lifecycle.daemon.exitCode !== null || lifecycle.daemon.signalCode !== null;
  const daemonNote = exited
    ? ` (daemon ALREADY EXITED before inspection: code=${lifecycle.daemon.exitCode}, ` +
      `signal=${lifecycle.daemon.signalCode} — the inspector was asked to measure a dead pid)`
    : ' (daemon was still alive, so this is the inspector, not a dead target)';
  for (const [index, result] of results.entries()) {
    const label = index === 0 ? 'resident' : 'private';
    if (result.status === 'rejected') {
      throw new Error(`${label} memory inspection failed${daemonNote}: ${result.reason?.message}`);
    }
    assert.ok(Number.isFinite(result.value) && result.value > 0,
      `${label} memory must be a positive real reading${daemonNote}`);
  }
  assert.equal(exited, false,
    'the daemon must outlive its own inspection, or the readings describe a corpse');
});
