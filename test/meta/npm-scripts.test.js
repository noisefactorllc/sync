// Every test file must be reachable from an npm script.
//
// Three times during the soak work we found a test that could not fail. Twice
// it was the test's own fault — a seam injected so the real code path never
// ran. The third time nothing was wrong with the test at all:
// `test:soak:unit` globbed `test/soak/lib/*.test.js`, so `test/soak/engine.test.js`
// — every test of the growth arithmetic, and the whole Windows
// TerminateProcess shutdown path — sat in no script, ran in no CI job, and
// guarded nothing. It had been passing locally for as long as anyone had
// bothered to invoke it by hand.
//
// A directory glob silently stops covering a file the moment someone adds one
// beside it rather than inside it. This asserts the property that matters —
// every test file is in some script's argument list — so the next file added
// in the wrong place fails here instead of quietly guarding nothing.
import assert from 'node:assert/strict';
import { globSync, readFileSync } from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const repoRoot = fileURLToPath(new URL('../..', import.meta.url));
const rel = (file) => path.relative(repoRoot, file).split(path.sep).join('/');

// Test files, by the two naming conventions this repo uses.
const testFiles = ['test/**/*.test.js', 'test/**/*.test.mjs']
  .flatMap((pattern) => globSync(pattern, { cwd: repoRoot })
    .map((file) => rel(path.resolve(repoRoot, file))))
  .sort();

// What the scripts actually run: every glob argument, expanded the same way
// npm's shell would expand it. A script that names a file directly and one
// that globs a directory both land here as concrete paths.
const scripts = JSON.parse(readFileSync(path.join(repoRoot, 'package.json'), 'utf8')).scripts ?? {};
const covered = new Set(Object.values(scripts)
  .flatMap((command) => command.split(/\s+/))
  .filter((token) => token.endsWith('.js') || token.endsWith('.mjs'))
  .flatMap((token) => globSync(token, { cwd: repoRoot })
    .map((file) => rel(path.resolve(repoRoot, file)))));

test('the repo has test files to check, so an empty glob cannot pass this vacuously', () => {
  assert.ok(testFiles.length >= 10, `expected the test suite to be found, saw ${testFiles.length} files`);
});

test('every test file is run by at least one npm script', () => {
  const orphans = testFiles.filter((file) => !covered.has(file));
  assert.deepEqual(orphans, [],
    `these test files are in no npm script, so nothing runs them and they guard nothing:\n` +
    orphans.map((file) => `  ${file}`).join('\n') +
    '\n\nAdd them to an existing script or give them their own.');
});

test('this guard is itself run by a script, so it cannot be the orphan it looks for', () => {
  assert.ok(covered.has(rel(fileURLToPath(import.meta.url))),
    'test/meta/npm-scripts.test.js is not in any npm script; the coverage guard is unguarded');
});
