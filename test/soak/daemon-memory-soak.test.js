import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

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
  const args = delayedHealthFailure ? ['--input-type=module', '-e', `
    import { registerHooks } from 'node:module';
    // This case tests health completion ordering. Slow inspectors would give
    // the injected failure time to finish even if the health joins vanished.
    // The separate tiny-frame case retains the real platform inspectors.
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
