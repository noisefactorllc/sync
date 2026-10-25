import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtemp, mkdir, readFile, readdir, rm } from 'node:fs/promises';
import { createRequire } from 'node:module';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { test } from 'node:test';

const directory = new URL('../workflows/', import.meta.url);
const name = (await readdir(directory)).find(name => /windows.*containment\.yml$/.test(name));
const workflow = await readFile(new URL(name, directory), 'utf8');
const script = [...workflow.matchAll(/            const fs = require\('node:fs\/promises'\);[\s\S]*?(?=\n\n      - name:)/g)]
  .map(match => match[0]).find(script => script.includes('const files = ['))
  .split('\n').map(line => line.slice(12)).join('\n');
const seeds = [...script.match(/const files = \[([\s\S]*?)\];/)[1].matchAll(/'([^']+)'/g)].map(match => match[1]);
const prefix = seeds[0].split('/').slice(0, -1).join('/');
const sourceVariable = script.match(/sha = process\.env\.([A-Z_]+)/)[1];
const rootVariable = script.match(/const root = process\.env\.([A-Z_]+)/)[1];
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
const execute = new AsyncFunction('require', 'process', 'github', script);
const blob = content => createHash('sha1').update(`blob ${Buffer.byteLength(content)}\0`).update(content).digest('hex');

async function fixture(t, extra = {}) {
  const root = await mkdtemp(path.join(tmpdir(), 'source-download-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const target = path.join(root, 'invocation');
  await mkdir(path.join(target, 'evidence'), { recursive: true });
  const files = new Map(seeds.map(file => [file, file.endsWith('.mjs') ? 'export {};\n' : 'fixture\n']));
  for (const [file, content] of Object.entries(extra)) files.set(file, content);
  const tree = [...files].map(([file, content]) => ({ path: file, type: 'blob', mode: '100644', sha: blob(content) }));
  const fetched = [];
  const github = { rest: { repos: {
    get: async () => ({ data: { default_branch: 'main', permissions: { push: false } } }),
    getCommit: async () => ({ data: { sha: 'a'.repeat(40), commit: { tree: { sha: 'b'.repeat(40) } } } }),
  }, git: {
    getTree: async () => ({ data: { truncated: false, tree } }),
    getBlob: async ({ file_sha }) => {
      const entry = tree.find(entry => entry.sha === file_sha);
      fetched.push(entry.path);
      const content = files.get(entry.path);
      return { data: { encoding: 'base64', size: Buffer.byteLength(content), content: Buffer.from(content).toString('base64') } };
    },
  } } };
  const run = () => execute(createRequire(import.meta.url), { env: {
    [sourceVariable]: 'a'.repeat(40), [rootVariable]: target, RUNNER_TEMP: root,
  } }, github);
  return { target, files, tree, github, fetched, run };
}

test('the reviewed source download includes transitive local module imports once', async t => {
  const dependency = `${prefix}/lib/added-dependency.mjs`;
  const leaf = `${prefix}/lib/leaf.mjs`;
  const f = await fixture(t, {
    [seeds[0]]: "import './lib/added-dependency.mjs';\n",
    [dependency]: "export { value } from './leaf.mjs';\nawait import('./leaf.mjs');\n",
    [leaf]: "import '../" + path.posix.basename(seeds[0]) + "';\nexport const value = true;\n",
  });
  await f.run();
  for (const file of [dependency, leaf]) {
    assert.equal(await readFile(path.join(f.target, file), 'utf8'), f.files.get(file));
    assert.equal(f.fetched.filter(name => name === file).length, 1);
  }
  const manifest = JSON.parse(await readFile(path.join(f.target, 'evidence/source-manifest.json'), 'utf8'));
  assert.equal(manifest.files.length, seeds.length + 2);
  for (const entry of manifest.files) assert.equal(entry.sha256,
    createHash('sha256').update(f.files.get(entry.path)).digest('hex'));
});

test('a local import cannot extend the reviewed source area', async t => {
  const escaped = path.posix.normalize(`${prefix}/../../outside.mjs`);
  const f = await fixture(t, { [seeds[0]]: "import '../../outside.mjs';\n", [escaped]: 'export {};\n' });
  await assert.rejects(f.run(), /source area/i);
  assert.ok(!f.fetched.includes(escaped));
});

test('a local import must name a regular blob in the exact source tree', async t => {
  const dependency = `${prefix}/lib/added-dependency.mjs`;
  const f = await fixture(t, { [seeds[0]]: "import './lib/added-dependency.mjs';\n", [dependency]: 'export {};\n' });
  f.tree.find(entry => entry.path === dependency).mode = '120000';
  await assert.rejects(f.run(), /regular files/);
});

test('downloaded module bytes must match the source blob identity', async t => {
  const f = await fixture(t);
  const original = f.github.rest.git.getBlob;
  f.github.rest.git.getBlob = async input => {
    const result = await original(input);
    const content = 'different!';
    return { data: { ...result.data, size: Buffer.byteLength(content), content: Buffer.from(content).toString('base64') } };
  };
  await assert.rejects(f.run(), /blob identity mismatch/);
});

test('case-distinct module imports cannot overwrite each other on the host', async t => {
  const f = await fixture(t, {
    [seeds[0]]: "import './lib/Case.mjs';\nimport './lib/case.mjs';\n",
    [`${prefix}/lib/Case.mjs`]: 'export const value = true;\n',
    [`${prefix}/lib/case.mjs`]: 'export const value = false;\n',
  });
  await assert.rejects(f.run(), /collide/);
});

test('an import inventory cannot grow without a bound', async t => {
  const additions = Object.fromEntries(Array.from({ length: 130 }, (_, i) => [`${prefix}/lib/dependency-${i}.mjs`, 'export {};\n']));
  const f = await fixture(t, {
    ...additions,
    [seeds[0]]: Object.keys(additions).map(file => `import './lib/${path.posix.basename(file)}';`).join('\n'),
  });
  await assert.rejects(f.run(), /inventory exceeded/);
});
