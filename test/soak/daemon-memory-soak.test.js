import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

const ROOT = fileURLToPath(new URL('../../', import.meta.url));
const DAEMON = path.resolve(ROOT, process.env.SYNC_DAEMON_PATH || 'build/syncd');
const SCRIPT = path.join(ROOT, 'test/acceptance/daemon-memory-soak.mjs');
if (process.env.SYNC_DAEMON_PATH) {
  assert.ok(existsSync(DAEMON), `SYNC_DAEMON_PATH does not exist: ${DAEMON}`);
}
const requiresDaemon = { skip: !existsSync(DAEMON), timeout: 20_000 };

async function shortRun(t, { width = 8, delayedHealthFailure = false } = {}) {
  const args = delayedHealthFailure ? ['--input-type=module', '-e', `
    const realFetch = globalThis.fetch;
    let previous;
    globalThis.fetch = (...args) => {
      if (previous) {
        clearTimeout(previous.timer);
        previous.resolve(previous.response);
        previous = null;
      }
      return realFetch(...args).then(response => new Promise((resolve, reject) => {
        previous = { response, resolve, timer: setTimeout(() => {
          reject(new Error('late health probe failure'));
        }, 1800) };
      }));
    };
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
  const timer = setTimeout(() => { timedOut = true; terminate(); }, 12_000);
  t.after(() => { clearTimeout(timer); terminate(); });
  const result = await new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('close', (code, signal) => resolve({ code, signal }));
  });
  clearTimeout(timer);
  assert.equal(timedOut, false, `five-second soak exceeded twelve seconds:\n${stdout}\n${stderr}`);
  return { ...result, stdout, stderr };
}

test('a five-second soak with tiny frames ends before its sixty-second sender cycle',
  requiresDaemon, async (t) => {
    const result = await shortRun(t);
    assert.equal(result.code, 0, result.stderr);
    assert.match(result.stdout, /cycles=1 sent=/);
  });

test('a final pending health failure prevents a successful soak verdict',
  requiresDaemon, async (t) => {
    const result = await shortRun(t, { width: 1024, delayedHealthFailure: true });
    assert.equal(result.code, 1, result.stdout);
    assert.match(result.stderr, /late health probe failure/);
  });
