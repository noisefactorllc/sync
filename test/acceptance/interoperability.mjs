import assert from 'node:assert/strict';
import { spawn, execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const require = createRequire(import.meta.url);
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const exec = promisify(execFile);
const daemonPath = path.resolve(process.env.SYNC_DAEMON_PATH || path.join(ROOT, 'build/syncd'));
const framework = process.env.SYPHON_FRAMEWORK_PATH;
const browserOnly = process.env.SYNC_ACCEPTANCE_BROWSER_ONLY === '1';
if (!browserOnly && !framework) {
  throw new Error('Set SYPHON_FRAMEWORK_PATH, or select browser-only acceptance with SYNC_ACCEPTANCE_BROWSER_ONLY=1.');
}
const token = 'isolated-interoperability-fixture';
const server = createServer(async (request, response) => {
  const pathname = new URL(request.url, 'http://localhost').pathname;
  if (pathname === '/') {
    response.setHeader('Content-Type', 'text/html');
    response.end('<!doctype html><title>Sync interoperability acceptance</title>');
    return;
  }
  const file = pathname === '/acceptance/interoperability-browser.mjs'
    ? path.join(ROOT, 'test/acceptance/interoperability-browser.mjs')
    : /^\/browser\/(?:adapters\/)?[a-z0-9-]+\.js$/.test(pathname)
      ? path.join(ROOT, pathname) : null;
  if (!file) { response.writeHead(404).end(); return; }
  try { response.setHeader('Content-Type', 'text/javascript'); response.end(await readFile(file)); }
  catch { response.writeHead(404).end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
let browser;
let daemon;
let daemonExit;
const output = { scope: browserOnly ? 'browser-only' : 'browser-and-syphon', browser: null, adapters: [], native: [], soak: null };

async function startDaemon() {
  daemon = spawn(daemonPath, ['--port', '0', '--test-origin', origin, '--test-token', token,
    '--publisher', 'syphon', '--syphon-framework', framework], { stdio: ['ignore', 'pipe', 'pipe'] });
  daemonExit = new Promise(resolve => daemon.once('close', resolve));
  let stdout = '', stderr = '';
  daemon.stderr.on('data', data => { stderr += data; });
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`The isolated daemon did not start. ${stderr}`)), 10000);
    const fail = error => { clearTimeout(timer); reject(error); };
    daemon.once('error', fail);
    daemon.once('exit', code => fail(new Error(`The isolated daemon exited with ${code}. ${stderr}`)));
    daemon.stdout.on('data', data => {
      stdout += data;
      const line = stdout.split('\n')[0];
      if (!stdout.includes('\n')) return;
      try {
        const ready = JSON.parse(line);
        assert.equal(ready.type, 'ready');
        clearTimeout(timer);
        resolve(`http://127.0.0.1:${ready.port}`);
      } catch (error) { fail(error); }
    });
  });
}

try {
  browser = await chromium.launch({ headless: process.env.SYNC_ACCEPTANCE_HEADFUL !== '1' });
  const page = await browser.newPage();
  // Model a user's permission approval for this isolated origin only.
  await page.context().grantPermissions(['local-network-access'], { origin });
  await page.goto(origin);
  output.browser = await page.evaluate(async () => {
    const gl = new OffscreenCanvas(1, 1).getContext('webgl2');
    const debug = gl?.getExtension('WEBGL_debug_renderer_info');
    const adapter = await navigator.gpu?.requestAdapter();
    return {
      userAgent: navigator.userAgent,
      renderer: debug && gl.getParameter(debug.UNMASKED_RENDERER_WEBGL),
      webgpu: adapter ? { vendor: adapter.info.vendor, architecture: adapter.info.architecture,
        device: adapter.info.device, description: adapter.info.description } : null,
    };
  });
  output.adapters = await page.evaluate(async () =>
    (await import('/acceptance/interoperability-browser.mjs')).checkAdapters());
  if (!browserOnly) {
    const endpoint = await startDaemon();
    for (const { kind } of output.adapters) {
      const name = `Sync Interop ${kind} ${process.pid}`;
      await page.evaluate(async options =>
        (await import('/acceptance/interoperability-browser.mjs')).startNative(options),
      { endpoint, token, kind, name });
      const { stdout } = await exec(path.join(path.dirname(daemonPath), 'sync_syphon_pixels_probe'),
        [framework, name], { timeout: 10000 });
      const receiver = JSON.parse(stdout);
      assert.equal(receiver.exact, true);
      const diagnostic = await page.evaluate(async () =>
        (await import('/acceptance/interoperability-browser.mjs')).stopNative());
      assert.ok(diagnostic.native.accepted > 0);
      assert.equal(diagnostic.native.failed, 0);
      assert.equal(diagnostic.local.failed, 0);
      output.native.push({ kind, receiver, diagnostic });
    }
    const name = `Sync Interop Markers ${process.pid}`;
    await page.evaluate(async options =>
      (await import('/acceptance/interoperability-browser.mjs')).startMarkers(options), { endpoint, token, name });
    const rssKb = [];
    const rssErrors = [];
    const sample = async () => {
      const { stdout } = await exec('ps', ['-o', 'rss=', '-p', String(daemon.pid)]);
      rssKb.push(Number(stdout.trim()));
    };
    await sample();
    const sampler = setInterval(() => sample().catch(error => rssErrors.push(error.message)), 1000);
    let measured;
    try {
      measured = await exec(path.join(path.dirname(daemonPath), 'sync_syphon_receiver_probe'),
        [framework, name, '12000', '5000', '64', '32'], { timeout: 20000 });
    } finally { clearInterval(sampler); }
    const receiver = JSON.parse(measured.stdout);
    assert.equal(rssErrors.length, 0, 'Every RSS sample must complete.');
    assert.ok(rssKb.length >= 10, 'RSS measurement must cover the soak.');
    assert.ok(rssKb.every(value => Number.isFinite(value) && value > 0), 'RSS samples must contain valid sizes.');
    assert.ok(receiver.markers >= 120, 'The receiver must observe changing frames.');
    assert.equal(receiver.invalidMarkers, 0);
    assert.equal(receiver.dimensionMismatches, 0);
    assert.equal(receiver.commandErrors, 0);
    assert.ok(Math.max(...rssKb) - Math.min(...rssKb) < 64 * 1024, 'The daemon RSS growth exceeds the smoke-test bound.');
    const diagnostic = await page.evaluate(async () =>
      (await import('/acceptance/interoperability-browser.mjs')).stopNative());
    output.soak = { durationMs: 12000, receiver, rssKb, diagnostic };
  }
  console.log(JSON.stringify(output, null, 2));
} finally {
  await browser?.close();
  if (daemon && daemon.exitCode === null) {
    daemon.kill('SIGTERM');
    const timer = setTimeout(() => daemon.kill('SIGKILL'), 3000);
    await daemonExit;
    clearTimeout(timer);
  }
  await new Promise(resolve => server.close(resolve));
}
