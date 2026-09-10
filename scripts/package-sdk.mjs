import { createHash } from 'node:crypto';
import { lstat, mkdir, mkdtemp, readFile, rename, rm, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { runNpm } from './npm-command.mjs';
import { SYNC_SDK_VERSION } from '../browser/version.js';

const root = fileURLToPath(new URL('../', import.meta.url));
const browser = path.join(root, 'browser');

export async function packageSdk({ outputDirectory = path.join(root, 'dist/sdk') } = {}) {
  const manifest = JSON.parse(await readFile(path.join(browser, 'package.json'), 'utf8'));
  if (manifest.version !== SYNC_SDK_VERSION) throw new Error('SDK versions do not match');
  const destination = path.join(path.resolve(outputDirectory), manifest.version);
  if (await lstat(destination).then(() => true, error => {
    if (error.code === 'ENOENT') return false;
    throw error;
  })) throw new Error('SDK version directory already exists');
  await mkdir(path.dirname(destination), { recursive: true });
  const staging = await mkdtemp(path.join(path.dirname(destination), '.sdk-'));
  try {
    const files = [...manifest.files, 'package.json'].sort();
    const sums = [];
    for (const name of files) {
      if (!/^[a-zA-Z0-9][a-zA-Z0-9./-]*$/.test(name) || name.split('/').includes('..')) {
        throw new Error('SDK package contains an invalid file path');
      }
      const source = path.join(browser, name);
      if (!(await lstat(source)).isFile()) throw new Error('SDK package inputs must be regular files');
      const bytes = await readFile(source);
      const target = path.join(staging, 'modules', name);
      await mkdir(path.dirname(target), { recursive: true });
      await writeFile(target, bytes, { flag: 'wx' });
      sums.push(`${createHash('sha256').update(bytes).digest('hex')}  modules/${name}`);
    }
    // Pack the frozen module copy so concurrent source edits cannot split the artifacts.
    const { stdout } = await runNpm(['pack', '--ignore-scripts', '--json', '--pack-destination', staging], {
      cwd: path.join(staging, 'modules'), timeout: 20000,
    });
    const [packed] = JSON.parse(stdout);
    if (!packed || path.basename(packed.filename) !== packed.filename ||
        JSON.stringify(packed.files.map(file => file.path).sort()) !== JSON.stringify(files)) {
      throw new Error('SDK package file list does not match');
    }
    const tarball = await readFile(path.join(staging, packed.filename));
    sums.push(`${createHash('sha256').update(tarball).digest('hex')}  ${packed.filename}`);
    await writeFile(path.join(staging, 'SHA256SUMS'), sums.join('\n') + '\n', { flag: 'wx' });
    await rename(staging, destination);
    return { directory: destination, tarball: path.join(destination, packed.filename), version: manifest.version };
  } catch (error) {
    await rm(staging, { recursive: true, force: true });
    throw error;
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  packageSdk({ outputDirectory: process.argv[2] }).then(
    result => process.stdout.write(JSON.stringify(result) + '\n'),
    error => { process.stderr.write(error.message + '\n'); process.exitCode = 1; },
  );
}
