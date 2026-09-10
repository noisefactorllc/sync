import assert from 'node:assert/strict';
import { spawn, execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { mkdtemp, readFile, realpath, rm } from 'node:fs/promises';
import { createServer } from 'node:https';
import { createRequire } from 'node:module';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { startExampleServer } from '../../examples/server.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const require = createRequire(import.meta.url);
const { chromium, _electron } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const electronPath = process.env.ELECTRON_PATH;
if (!electronPath) throw new Error('Set ELECTRON_PATH to the Electron executable.');
const exec = promisify(execFile);
const temporary = await mkdtemp(path.join(await realpath(os.tmpdir()), 'sync-example-acceptance-'));
const pairingServer = path.resolve(process.env.SYNC_PAIRING_SERVER_PATH || path.join(ROOT, 'build/sync_pairing_test_server'));
const children = [];
let browser, electron, httpsServer, httpServer;
const results = [];

async function daemon(mode) {
  const child = spawn(pairingServer, [path.join(temporary, `${mode}.store`), mode], { stdio: ['ignore', 'pipe', 'pipe'] });
  children.push(child);
  let stdout = '', stderr = '';
  child.stderr.on('data', data => { stderr += data; });
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`The pairing fixture did not start. ${stderr}`)), 10000);
    child.once('error', error => { clearTimeout(timer); reject(error); });
    child.once('exit', code => { clearTimeout(timer); reject(new Error(`The pairing fixture exited with ${code}. ${stderr}`)); });
    child.stdout.on('data', data => {
      stdout += data;
      if (stdout.includes('\n')) {
        clearTimeout(timer);
        const ready = JSON.parse(stdout.split('\n')[0]);
        resolve({ child, endpoint: `http://127.0.0.1:${ready.port}` });
      }
    });
  });
}

async function exercise(frame, label, origins) {
  for (const mode of ['canvas', 'webgl2', 'webgpu']) {
    await frame.locator('#mode').selectOption(mode);
    await frame.locator('#connect').click();
    try {
      await frame.waitForFunction(() => document.querySelector('#status').textContent.startsWith('Sending'), null, { timeout: 10000 });
    } catch (cause) {
      throw new Error(`${label} ${mode}: ${await frame.locator('#status').textContent()}`, { cause });
    }
    await frame.waitForFunction(() => Number(document.querySelector('#stats').textContent.match(/sent (\d+)/)?.[1]) >= 5);
    const before = await frame.evaluate(() => {
      const width = document.querySelector('canvas').width;
      document.querySelector('#surface').style.width = '81%';
      dispatchEvent(new Event('resize'));
      return width;
    });
    await frame.waitForFunction(width => document.querySelector('canvas').width !== width, before);
    await frame.waitForFunction(() => Number(document.querySelector('#stats').textContent.match(/sent (\d+)/)?.[1]) >= 10);
    const stats = await frame.locator('#stats').textContent();
    assert.match(stats, /failed 0$/);
    await frame.locator('#stop').click();
    await frame.waitForFunction(() => document.querySelector('#status').textContent === 'Stopped.');
    assert.equal(await frame.locator('#stop').isDisabled(), true);
    await frame.evaluate(() => { document.querySelector('#surface').style.width = ''; });
    results.push({ host: label, mode, stats, origins: [...origins] });
  }
}

try {
  const approved = await daemon('approve');
  const denied = await daemon('deny');
  const hanging = await daemon('hang');
  httpServer = await startExampleServer({ port: 0 });
  const httpOrigin = `http://127.0.0.1:${httpServer.address().port}`;
  await exec('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
    '-keyout', path.join(temporary, 'key.pem'), '-out', path.join(temporary, 'cert.pem'),
    '-subj', '/CN=127.0.0.1']);
  // A temporary certificate keeps this test independent of public hosting.
  httpsServer = createServer({
    key: await readFile(path.join(temporary, 'key.pem')),
    cert: await readFile(path.join(temporary, 'cert.pem')),
  }, async (request, response) => {
    if (request.url === '/embed') {
      response.setHeader('Content-Type', 'text/html');
      response.setHeader('Permissions-Policy', `loopback-network=(self "${httpOrigin}")`);
      response.end(`<iframe style="width:900px;height:900px" allow="loopback-network; local-network-access" src="${httpOrigin}/?endpoint=${approved.endpoint}"></iframe>`);
      return;
    }
    try {
      const upstream = await fetch(new URL(request.url, httpOrigin));
      response.writeHead(upstream.status, { 'Content-Type': upstream.headers.get('Content-Type'),
        'Permissions-Policy': 'loopback-network=(self)' });
      response.end(Buffer.from(await upstream.arrayBuffer()));
    } catch { response.writeHead(500).end(); }
  });
  await new Promise(resolve => httpsServer.listen(0, '127.0.0.1', resolve));
  const httpsOrigin = `https://127.0.0.1:${httpsServer.address().port}`;
  browser = await chromium.launch({ headless: false });
  const context = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 960, height: 800 } });
  for (const origin of [httpOrigin, httpsOrigin]) await context.grantPermissions(['local-network-access'], { origin });
  const page = await context.newPage();
  const origins = new Set();
  const captures = [];
  page.on('request', request => {
    if (request.url().startsWith(approved.endpoint)) {
      captures.push(request.allHeaders().then(headers => {
        if (headers.origin) origins.add(headers.origin);
      }));
    }
  });
  for (const origin of [httpOrigin, httpsOrigin]) {
    origins.clear();
    await page.goto(`${origin}/?endpoint=${approved.endpoint}`);
    await exercise(page, origin.startsWith('https:') ? 'https' : 'http-loopback', origins);
    await Promise.all(captures);
    assert.deepEqual([...origins], [origin]);
  }
  origins.clear();
  await page.goto(`${httpsOrigin}/embed`);
  await page.waitForFunction(() => document.querySelector('iframe')?.contentWindow !== null);
  const frame = await (await page.locator('iframe').elementHandle()).contentFrame();
  await frame.locator('#connect').waitFor();
  await exercise(frame, 'delegated-iframe', origins);
  await Promise.all(captures);
  assert.deepEqual([...origins], [httpOrigin]);

  await page.goto(`${httpOrigin}/?endpoint=${denied.endpoint}`);
  await page.locator('#connect').click();
  await page.waitForFunction(() => /denied/i.test(document.querySelector('#status').textContent));
  assert.equal(await page.locator('#stop').isDisabled(), true);
  results.push({ host: 'http-loopback', denial: await page.locator('#status').textContent() });

  await page.goto(`${httpOrigin}/?endpoint=${hanging.endpoint}`);
  await page.locator('#connect').click();
  await page.evaluate(() => dispatchEvent(new PageTransitionEvent('pagehide')));
  await page.waitForFunction(() => !document.querySelector('#connect').disabled);
  assert.equal(await page.locator('#stop').isDisabled(), true);
  assert.equal(await page.locator('canvas').count(), 0);
  results.push({ host: 'http-loopback', pendingStartCanceled: true });

  electron = await _electron.launch({ executablePath: electronPath,
    args: [path.join(ROOT, 'examples/electron/main.mjs'), `--user-data-dir=${path.join(temporary, 'electron')}`] });
  const window = await electron.firstWindow();
  await window.waitForLoadState('domcontentloaded');
  await window.locator('#connect').waitFor({ timeout: 10000 });
  assert.equal(await window.evaluate(() => location.origin), 'app://com.example.visualizer');
  await electron.evaluate(async ({ BrowserWindow }, endpoint) =>
    BrowserWindow.getAllWindows()[0].loadURL(`app://com.example.visualizer/?endpoint=${endpoint}`), approved.endpoint);
  const appOrigins = new Set();
  const appCaptures = [];
  window.on('request', request => {
    if (request.url().startsWith(approved.endpoint)) {
      appCaptures.push(request.allHeaders().then(headers => {
        if (headers.origin) appOrigins.add(headers.origin);
      }));
    }
  });
  await exercise(window, 'electron', appOrigins);
  await Promise.all(appCaptures);
  assert.deepEqual([...appOrigins], ['app://com.example.visualizer']);
  const prefs = await electron.evaluate(({ BrowserWindow }) => {
    const { nodeIntegration, contextIsolation, sandbox } = BrowserWindow.getAllWindows()[0].webContents.getLastWebPreferences();
    return { nodeIntegration, contextIsolation, sandbox };
  });
  assert.deepEqual(prefs, { nodeIntegration: false, contextIsolation: true, sandbox: true });
  await window.locator('#mode').selectOption('webgl2');
  await window.locator('#connect').click();
  await window.waitForFunction(() => document.querySelector('#status').textContent.startsWith('Sending'));
  await window.evaluate(() => document.querySelector('canvas').getContext('webgl2').getExtension('WEBGL_lose_context').loseContext());
  await window.waitForFunction(() => document.querySelector('#stop').disabled && /lost/.test(document.querySelector('#status').textContent));
  results.push({ host: 'electron', contextLoss: await window.locator('#status').textContent() });
  await window.locator('#mode').selectOption('webgpu');
  await window.locator('#connect').click();
  await window.waitForFunction(() => document.querySelector('#status').textContent.startsWith('Sending'));
  if (process.env.SYNC_EXAMPLE_SCREENSHOT) await window.screenshot({ path: process.env.SYNC_EXAMPLE_SCREENSHOT });
  approved.child.kill('SIGTERM');
  await window.waitForFunction(() => document.querySelector('#stop').disabled && !document.querySelector('#status').textContent.startsWith('Sending'));
  results.push({ host: 'electron', daemonLoss: await window.locator('#status').textContent() });
  console.log(JSON.stringify({ browser: await browser.version(), electron: await electron.evaluate(() => process.versions.electron),
    permissions: 'Test-origin grants in Chrome; scoped app policy in Electron; native approval fixture.', results }, null, 2));
} finally {
  await electron?.close();
  await browser?.close();
  for (const child of children) {
    if (child.exitCode !== null || child.signalCode !== null) continue;
    const exited = new Promise(resolve => child.once('exit', resolve));
    child.kill('SIGTERM');
    const timer = setTimeout(() => child.kill('SIGKILL'), 2000);
    await exited;
    clearTimeout(timer);
  }
  if (httpsServer) await new Promise(resolve => httpsServer.close(resolve));
  if (httpServer) await new Promise(resolve => httpServer.close(resolve));
  await rm(temporary, { recursive: true, force: true });
}
