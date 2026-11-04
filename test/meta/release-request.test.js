// The release request logic in scripts/release-request.mjs, which
// .github/workflows/release.yml runs. Real git histories stand in for pushes;
// a scripted GitHub API stands in for the CI, Camera end-to-end and earlier
// release-request runs.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';

import { DISPATCH_STEP, decide } from '../../scripts/release-request.mjs';

const REPOSITORY = 'noisefactorllc/sync';
const CURRENT_RUN = 900;

function repository(t) {
  const directory = mkdtempSync(path.join(os.tmpdir(), 'sync-release-request-'));
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  const git = (...args) => execFileSync('git', args, { cwd: directory, encoding: 'utf8' }).trim();
  git('init', '--quiet', '--initial-branch=main');
  git('config', 'user.email', 'test@example.invalid');
  git('config', 'user.name', 'test');
  const commit = (file, message = file) => {
    mkdirSync(path.dirname(path.join(directory, file)), { recursive: true });
    writeFileSync(path.join(directory, file), `${message}\n`);
    git('add', file);
    git('commit', '--quiet', '-m', message);
    return git('rev-parse', 'HEAD');
  };
  return { directory, git, commit };
}

// gates: { [sha]: { ci, camera, jobs } } with conclusions; requests: earlier
// release runs, newest first, as [sha, dispatched].
function api({ gates, requests = [] }) {
  const runs = {};
  let id = 1;
  const run = (fields) => {
    const value = { id: id++, run_number: id, head_branch: 'main', status: 'completed', ...fields };
    runs[value.id] = value;
    return value;
  };
  const byWorkflow = { 'ci.yml': [], 'camera-e2e.yml': [], 'release.yml': [] };
  for (const [sha, gate] of Object.entries(gates)) {
    if (gate.ci) {
      const ci = run({ head_sha: sha, event: 'push', conclusion: gate.ci,
        status: gate.ci === 'in_progress' ? 'in_progress' : 'completed' });
      ci.jobs = gate.jobs ?? [{ name: 'Classify changed paths', conclusion: 'success' },
        { name: 'Native (macOS)', conclusion: 'success' }];
      byWorkflow['ci.yml'].push(ci);
    }
    if (gate.camera) {
      byWorkflow['camera-e2e.yml'].push(run({ head_sha: sha, event: 'push', conclusion: gate.camera,
        status: gate.camera === 'in_progress' ? 'in_progress' : 'completed' }));
    }
  }
  requests.forEach(([sha, dispatched], index) => {
    const request = run({
      head_sha: 'f'.repeat(40),
      display_title: `Request Sync preview release for ${sha}`,
      created_at: new Date(Date.UTC(2026, 10, 1, 12, 0, 0) - index * 60_000).toISOString(),
      event: 'workflow_run', conclusion: 'success',
    });
    request.jobs = [{ name: 'Request a release of this commit', conclusion: 'success', steps: [
      { name: DISPATCH_STEP, conclusion: dispatched ? 'success' : 'skipped' },
    ] }];
    byWorkflow['release.yml'].push(request);
  });
  return async (url) => {
    const workflow = url.match(/workflows\/([^/]+)\/runs/);
    if (workflow) {
      const sha = new URL(url, 'https://api.github.com').searchParams.get('head_sha');
      return { workflow_runs: byWorkflow[workflow[1]].filter(entry => !sha || entry.head_sha === sha) };
    }
    const jobs = url.match(/runs\/(\d+)\/jobs/);
    if (jobs) return { jobs: runs[Number(jobs[1])].jobs };
    throw new Error(`unexpected ${url}`);
  };
}

const both = { ci: 'success', camera: 'success' };

test('a push of a native change and then a README change requests a release', async (t) => {
  const { directory, commit } = repository(t);
  const requested = commit('native/a.cpp');
  commit('native/b.cpp');
  const head = commit('README.md');
  const result = await decide({ api: api({ gates: { [head]: both }, requests: [[requested, true]] }),
    repository: REPOSITORY, sha: head, currentRunId: CURRENT_RUN, cwd: directory });
  assert.equal(result.release, true, result.reason);
  assert.equal(result.base, requested);
});

test('a push that changes only Markdown and docs requests no release', async (t) => {
  const { directory, commit } = repository(t);
  const requested = commit('native/a.cpp');
  commit('docs/guide.md');
  const head = commit('README.md');
  const result = await decide({ api: api({ gates: { [head]: both }, requests: [[requested, true]] }),
    repository: REPOSITORY, sha: head, currentRunId: CURRENT_RUN, cwd: directory });
  assert.equal(result.release, false, result.reason);
});

test('a pull-request merge decides from the merged changes, as before', async (t) => {
  for (const [file, expected] of [['native/feature.cpp', true], ['docs/feature.md', false]]) {
    const { directory, git, commit } = repository(t);
    const requested = commit('native/a.cpp');
    git('checkout', '--quiet', '-b', 'feature');
    commit(file);
    git('checkout', '--quiet', 'main');
    commit('README.md', 'main docs');
    git('merge', '--quiet', '--no-ff', '-m', 'merge', 'feature');
    const head = git('rev-parse', 'HEAD');
    // The earlier rule: the merge commit against its first parent.
    const firstParent = git('diff', '--name-only', 'HEAD~1', 'HEAD').split('\n');
    assert.equal(firstParent.includes(file), true);
    const result = await decide({ api: api({ gates: { [head]: both }, requests: [[requested, true]] }),
      repository: REPOSITORY, sha: head, currentRunId: CURRENT_RUN, cwd: directory });
    assert.equal(result.release, expected, `${file}: ${result.reason}`);
  }
});

test('the base is the newest earlier request whose dispatch step ran', async (t) => {
  const { directory, commit } = repository(t);
  const requested = commit('native/a.cpp');
  const skipped = commit('docs/a.md');
  const head = commit('README.md');
  const result = await decide({
    api: api({ gates: { [head]: both }, requests: [[skipped, false], [requested, true]] }),
    repository: REPOSITORY, sha: head, currentRunId: CURRENT_RUN, cwd: directory });
  assert.equal(result.base, requested);
  assert.equal(result.release, false, result.reason);
});

test('a successful CI run with a failed Camera end-to-end run requests no release', async (t) => {
  const { directory, commit } = repository(t);
  const head = commit('native/a.cpp');
  for (const camera of ['failure', 'cancelled', 'in_progress', undefined]) {
    const result = await decide({ api: api({ gates: { [head]: { ci: 'success', camera } } }),
      repository: REPOSITORY, sha: head, currentRunId: CURRENT_RUN, cwd: directory });
    assert.equal(result.release, false, String(camera));
    assert.match(result.reason, /Camera end-to-end/);
  }
});

test('either completion order of the two gates gives exactly one request', async (t) => {
  const { directory, commit } = repository(t);
  const previous = commit('native/a.cpp');
  const head = commit('native/b.cpp');
  // CI completes first while the camera gate still runs: no request yet.
  const first = await decide({
    api: api({ gates: { [head]: { ci: 'success', camera: 'in_progress' } }, requests: [[previous, true]] }),
    repository: REPOSITORY, sha: head, currentRunId: CURRENT_RUN, cwd: directory });
  assert.equal(first.release, false);
  // The camera gate completes: one request.
  const second = await decide({ api: api({ gates: { [head]: both }, requests: [[previous, true]] }),
    repository: REPOSITORY, sha: head, currentRunId: CURRENT_RUN, cwd: directory });
  assert.equal(second.release, true, second.reason);
  // A run for the other workflow after that request: no second request.
  const third = await decide({
    api: api({ gates: { [head]: both }, requests: [[head, true], [previous, true]] }),
    repository: REPOSITORY, sha: head, currentRunId: CURRENT_RUN, cwd: directory });
  assert.equal(third.release, false);
  assert.match(third.reason, /already has a release request/);
});

test('a commit older than the last requested commit gets no request', async (t) => {
  const { directory, commit } = repository(t);
  const older = commit('native/a.cpp');
  const newer = commit('native/b.cpp');
  const result = await decide({ api: api({ gates: { [older]: both }, requests: [[newer, true]] }),
    repository: REPOSITORY, sha: older, currentRunId: CURRENT_RUN, cwd: directory });
  assert.equal(result.release, false);
  assert.match(result.reason, /does not descend/);
});

test('a CI run that skipped or failed a product job requests no release', async (t) => {
  const { directory, commit } = repository(t);
  const head = commit('native/a.cpp');
  for (const jobs of [
    [{ name: 'Classify changed paths', conclusion: 'success' }],
    [{ name: 'Classify changed paths', conclusion: 'success' }, { name: 'Native (Linux)', conclusion: 'skipped' }],
  ]) {
    const result = await decide({ api: api({ gates: { [head]: { ...both, jobs } } }),
      repository: REPOSITORY, sha: head, currentRunId: CURRENT_RUN, cwd: directory });
    assert.equal(result.release, false);
    assert.match(result.reason, /product job/);
  }
});
