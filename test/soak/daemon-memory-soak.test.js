import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { ProtocolSoak } from './engine.mjs';
import { residentKbAsync, footprintKbAsync } from './lib/process-metrics.mjs';

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
const SCRIPT = path.join(ROOT, 'test/acceptance/daemon-memory-soak.mjs');
if (process.env.SYNC_DAEMON_PATH) {
  assert.ok(existsSync(DAEMON), `SYNC_DAEMON_PATH does not exist: ${DAEMON}`);
}
const requiresDaemon = { skip: !existsSync(DAEMON), timeout: 25_000 };

async function shortRun(t, { width = 8, delayedHealthFailure = false } = {}) {
  const args = delayedHealthFailure || process.platform === 'win32' ? ['--input-type=module', '-e', `
    import { registerHooks } from 'node:module';
    // These cases test timer fairness and health completion ordering. Slow
    // inspectors can consume the entire five-second window or mask a missing
    // health join. Real Windows commands are checked separately below.
    registerHooks({ load(url, context, nextLoad) {
      if (url === ${JSON.stringify(new URL('./lib/process-metrics.mjs', import.meta.url).href)}) {
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
    const realFetch = globalThis.fetch;
    let previous;
    globalThis.fetch = (...args) => {
      if (previous) {
        clearTimeout(previous.timer);
        previous.resolve(previous.response);
        previous = null;
      }
      return realFetch(...args).then(async response => {
        // Consume the real body while its request deadline is active. The
        // synthetic completion delay must not race that body's abort signal.
        const body = await response.arrayBuffer();
        const completed = new Response(body, {
          status: response.status, headers: response.headers,
        });
        return new Promise((resolve, reject) => {
          previous = { response: completed, resolve, timer: setTimeout(() => {
            reject(new Error('late health probe failure'));
          }, 1800) };
        });
      });
    };
    }
    await import(${JSON.stringify(new URL('../acceptance/daemon-memory-soak.mjs', import.meta.url).href)});
  `] : process.platform === 'darwin' ? ['--input-type=module', '-e', `
    import { registerHooks } from 'node:module';
    // vmmap suspends a busy target and does not support ASan heaps. This
    // lifecycle regression measures real RSS without inspecting its heap;
    // physical-footprint acceptance is a separate, unsanitized soak.
    const metrics = ${JSON.stringify(new URL('./lib/process-metrics.mjs', import.meta.url).href)};
    registerHooks({ load(url, context, nextLoad) {
      if (url === metrics) {
        const original = JSON.stringify(metrics + '?resident-fixture');
        return { format: 'module', shortCircuit: true, source:
          'export * from ' + original + '; ' +
          'export { residentKbAsync as footprintKbAsync } from ' + original + ';' };
      }
      return nextLoad(url, context);
    } });
    await import(${JSON.stringify(new URL('../acceptance/daemon-memory-soak.mjs', import.meta.url).href)});
  `] : [SCRIPT];
  const child = spawn(process.execPath, args, {
    cwd: ROOT,
    env: { ...process.env, SYNC_DAEMON_PATH: DAEMON, SYNC_SOAK_SECONDS: '5',
      SYNC_SOAK_CYCLE: '60', SYNC_SOAK_WIDTH: String(width),
      SYNC_SOAK_HEIGHT: String(width), SYNC_SOAK_GROWTH_KB: '1000000',
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

test('a five-second soak with tiny frames ends before its sixty-second sender cycle',
  requiresDaemon, async (t) => {
    const result = await shortRun(t);
    assert.equal(result.code, 0, `${result.stdout}\n${result.stderr}`);
    assert.match(result.stdout, /cycles=1 sent=/);
  });

test('a final pending health failure prevents a successful soak verdict',
  requiresDaemon, async (t) => {
    const result = await shortRun(t, { width: 1024, delayedHealthFailure: true });
    assert.equal(result.code, 1, result.stdout);
    assert.match(result.stderr, /late health probe failure/, result.stdout);
  });

test('Windows inspectors read real daemon resident and private memory', {
  ...requiresDaemon,
  skip: process.platform !== 'win32' ? 'requires the Windows process memory APIs' : requiresDaemon.skip,
}, async (t) => {
  const lifecycle = new ProtocolSoak({ daemonPath: DAEMON, width: 8, height: 8,
    origin: 'https://soak.example', token: 'soak-token-123' });
  t.after(() => lifecycle.stop());
  await lifecycle.start();
  // Inspect an idle real daemon without coupling PowerShell startup to a
  // five-second streaming window. Join both commands before cleanup, and do
  // not let a private-byte failure pass by falling back to resident memory.
  // A GENEROUS BUDGET, BECAUSE THIS TEST ASKS A DIFFERENT QUESTION THAN A SOAK.
  //
  // The module's 10s default protects a sampler that must keep pace with a
  // stream: there, a slow shell should be abandoned. Here the question is
  // whether the inspectors can read a real daemon at all, and a cold CI runner
  // spawning two PowerShells at once has twice exceeded 10s — 10,445ms and
  // 10,564ms — turning a working inspector into a red build. Abandoning the
  // read is the wrong answer to this test's question.
  const timeoutMs = 60_000;
  const results = await Promise.allSettled([
    residentKbAsync(lifecycle.daemon.pid, { timeoutMs }),
    footprintKbAsync(lifecycle.daemon.pid, { timeoutMs, fallback: () => {
      throw new Error('the real Windows private-memory inspection failed');
    } }),
  ]);
  // Whether the daemon outlived the inspection is reported as its own fact —
  // not because a dead pid can produce the rejection above (it cannot:
  // Get-Process on a missing pid exits zero, so execFile resolves with empty
  // stdout and the Number.isFinite check below is what fails), but because a
  // daemon that dies mid-inspection makes every reading describe a corpse, and
  // that deserves to be stated rather than inferred.
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
