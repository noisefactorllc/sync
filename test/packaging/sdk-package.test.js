import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { runNpm } from '../../scripts/npm-command.mjs';
import { createHash } from 'node:crypto';

const run = promisify(execFile);
const root = fileURLToPath(new URL('../..', import.meta.url));

test('packed SDK installs in an external project and emits literal frame bytes', { timeout: 30000 }, async (t) => {
  const directory = await mkdtemp(path.join(os.tmpdir(), 'sync-sdk-consumer-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const { stdout } = await runNpm(['pack', '--ignore-scripts', '--json', '--pack-destination', directory], {
    cwd: path.join(root, 'browser'), timeout: 20000,
  });
  const [artifact] = JSON.parse(stdout);
  assert.equal(artifact.name, '@noisefactor/sync');
  assert.equal(artifact.version, '0.2.0');
  const manifest = JSON.parse(await readFile(path.join(root, 'browser/package.json'), 'utf8'));
  assert.deepEqual(artifact.files.map(file => file.path).sort(), [...manifest.files, 'package.json'].sort());
  await writeFile(path.join(directory, 'package.json'), '{"private":true,"type":"module"}');
  await runNpm(['install', '--ignore-scripts', '--no-audit', '--no-fund', '--package-lock=false',
    path.join(directory, artifact.filename)], { cwd: directory, timeout: 20000 });
  const sample = `
    import assert from 'node:assert/strict';
    import { SyncFrameSink, RgbaExportQueue, SyncBridgeClient, SYNC_SDK_VERSION } from '@noisefactor/sync';
    let packet;
    const socket = { readyState: 1, bufferedAmount: 0, close() {}, send(bytes) { packet = bytes.slice(); } };
    const sink = new SyncFrameSink({socket, exportQueue: new RgbaExportQueue(),
      maxBufferedFrames: 2, clock: {timeOrigin: 0}});
    sink.configure({width:1,height:1,format:'rgba8unorm',colorSpace:'srgb',alphaMode:'straight',fps:60});
    const pixels = new Uint8Array([255,0,32,255]);
    assert.equal(sink.submit({width:1,height:1,rowStride:4,data:pixels}, 1), true);
    pixels.fill(0);
    assert.deepEqual([...packet.slice(64)], [255,0,32,255]);
    assert.equal(SYNC_SDK_VERSION, '0.2.0');
    assert.equal(typeof SyncBridgeClient.prototype.createRgbaSender, 'function');
    sink.close();
  `;
  await run(process.execPath, ['--input-type=module', '-e', sample], { cwd: directory, timeout: 5000 });
});

test('SDK distribution includes matching modules and rejects an existing version', { timeout: 30000 }, async (t) => {
  const { packageSdk } = await import('../../scripts/package-sdk.mjs');
  const directory = await mkdtemp(path.join(os.tmpdir(), 'sync-sdk-distribution-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const result = await packageSdk({ outputDirectory: directory });
  const sums = await readFile(path.join(result.directory, 'SHA256SUMS'), 'utf8');
  for (const entry of sums.trim().split('\n')) {
    const [expected, name] = entry.split('  ');
    const bytes = await readFile(path.join(result.directory, name));
    assert.equal(createHash('sha256').update(bytes).digest('hex'), expected);
  }
  const snapshot = await readFile(path.join(result.directory, 'modules/client.js'));
  assert.deepEqual(snapshot, await readFile(path.join(root, 'browser/client.js')));
  await assert.rejects(packageSdk({ outputDirectory: directory }), /already exists/);
  assert.deepEqual(await readFile(path.join(result.directory, 'modules/client.js')), snapshot);
});
