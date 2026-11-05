import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { access, copyFile, mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { packageSdk } from '../../scripts/package-sdk.mjs';
import { runNpm } from '../../scripts/npm-command.mjs';

const run = promisify(execFile);
const root = fileURLToPath(new URL('../..', import.meta.url));
// The compiler pinned in the root package-lock.json. The consumer project
// gets it by path and installs nothing from the network.
const tsc = path.join(root, 'node_modules/typescript/bin/tsc');

test('packed SDK declarations compile a strict DOM consumer', { timeout: 150000 }, async (t) => {
  await access(tsc).catch(() => assert.fail(
    `no TypeScript compiler at ${tsc}; run npm ci --ignore-scripts --no-bin-links first`));
  const { stdout: version } = await run(process.execPath, [tsc, '--version'], { timeout: 30000 });
  const [major, minor, patch] = (version.match(/Version (\d+)\.(\d+)\.(\d+)/) ?? assert.fail(version))
    .slice(1).map(Number);
  assert.ok(major > 7 || (major === 7 && (minor > 0 || patch >= 2)), `TypeScript 7.0.2 or later, got ${version}`);

  const directory = await mkdtemp(path.join(os.tmpdir(), 'sync-sdk-types-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  const { tarball } = await packageSdk({ outputDirectory: path.join(directory, 'sdk') });
  const project = path.join(directory, 'consumer');
  await mkdir(project);
  await writeFile(path.join(project, 'package.json'), '{"private":true,"type":"module"}');
  await runNpm(['install', '--offline', '--ignore-scripts', '--no-audit', '--no-fund', '--package-lock=false',
    tarball], { cwd: project, timeout: 60000 });
  await copyFile(path.join(root, 'test/packaging/consumer.ts'), path.join(project, 'consumer.ts'));

  const failure = await run(process.execPath, [tsc, '--strict', '--noEmit', '--target', 'es2022',
    '--module', 'nodenext', '--moduleResolution', 'nodenext', '--lib', 'es2022,dom', 'consumer.ts'],
  { cwd: project, timeout: 60000 }).then(() => null, error => error);
  if (failure) assert.fail(`tsc exited ${failure.code ?? failure.signal}:\n${failure.stdout}${failure.stderr}`);
});
