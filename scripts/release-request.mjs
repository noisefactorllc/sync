#!/usr/bin/env node
// Decides whether "Request Sync preview release" asks the release harness to
// build one main commit. The commit must have passed two gates at that exact
// SHA: the CI workflow, with every product job successful, and the Camera
// end-to-end workflow. Whichever of the two completes last makes the request,
// a commit gets at most one request, and a commit older than one that already
// got a request gets none. The product-path decision reads every commit since
// the last commit that got a request, so a docs commit at the head of a push
// does not hide the product commits under it.
//
// Usage (in the workflow, from a full-history checkout of the commit):
//   node scripts/release-request.mjs <sha> <current-run-id>
// Reads GITHUB_REPOSITORY and GH_TOKEN; writes release, base and reason to
// GITHUB_OUTPUT.
import { execFileSync } from 'node:child_process';
import { appendFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';

export const DISPATCH_STEP = 'Dispatch the release harness for this exact commit';
export const CLASSIFY_JOB = 'Classify changed paths';
// The render helper under render/ ships inside the macOS app, so it is product
// code like native/. A docs-only range must not mint a new signed build, burn
// a notarization round trip, or advance the public preview version.
export const PRODUCT_PATHS =
  /^(native\/|browser\/|packaging\/|scripts\/|test\/|render\/|CMakeLists\.txt$|package\.json$)/;
const SHA = /\b[0-9a-f]{40}\b/;

function newest(runs) {
  return [...runs].sort((a, b) =>
    (b.run_number - a.run_number) || (b.run_attempt ?? 1) - (a.run_attempt ?? 1))[0];
}

async function gateRun(api, repository, workflow, sha, events) {
  const { workflow_runs: runs } = await api(
    `/repos/${repository}/actions/workflows/${workflow}/runs?head_sha=${sha}&branch=main&per_page=50`);
  return newest(runs.filter(run => run.head_sha === sha && run.head_branch === 'main' &&
    events.includes(run.event)));
}

// Both gates at `sha`, or the reason they do not both hold yet.
export async function gatesPassed(api, repository, sha) {
  const ci = await gateRun(api, repository, 'ci.yml', sha, ['push']);
  if (!ci || ci.status !== 'completed' || ci.conclusion !== 'success') {
    return { passed: false, reason: `CI at ${sha} has not succeeded` };
  }
  const { jobs } = await api(`/repos/${repository}/actions/runs/${ci.id}/jobs?filter=latest&per_page=100`);
  const product = jobs.filter(job => job.name !== CLASSIFY_JOB);
  if (product.length === 0 || !product.every(job => job.conclusion === 'success')) {
    return { passed: false, reason: `CI at ${sha} skipped or failed a product job` };
  }
  const camera = await gateRun(api, repository, 'camera-e2e.yml', sha, ['push', 'workflow_dispatch']);
  if (!camera || camera.status !== 'completed' || camera.conclusion !== 'success') {
    return { passed: false, reason: `Camera end-to-end at ${sha} has not succeeded` };
  }
  return { passed: true, reason: `CI ${ci.id} and Camera end-to-end ${camera.id} succeeded at ${sha}` };
}

// The commit of the newest earlier run of this workflow whose dispatch step ran.
export async function lastRequested(api, repository, currentRunId) {
  const { workflow_runs: runs } = await api(
    `/repos/${repository}/actions/workflows/release.yml/runs?branch=main&status=completed&per_page=100`);
  const earlier = runs.filter(run => run.id !== currentRunId)
    .sort((a, b) => Date.parse(b.created_at) - Date.parse(a.created_at));
  for (const run of earlier) {
    const { jobs } = await api(`/repos/${repository}/actions/runs/${run.id}/jobs?filter=latest&per_page=100`);
    const dispatched = jobs.some(job => (job.steps ?? []).some(step =>
      step.name === DISPATCH_STEP && step.conclusion === 'success'));
    if (dispatched) return run.display_title?.match(SHA)?.[0] ?? run.head_sha;
  }
  return null;
}

function git(args, cwd) {
  return execFileSync('git', args, { encoding: 'utf8', cwd, stdio: ['ignore', 'pipe', 'pipe'] });
}

function isAncestor(ancestor, descendant, cwd) {
  try {
    git(['merge-base', '--is-ancestor', ancestor, descendant], cwd);
    return true;
  } catch {
    return false;
  }
}

// Paths that changed from `base` to `sha`, or in `sha` alone when no earlier
// request is known.
export function changedPaths(sha, base, cwd) {
  if (base) return git(['diff', '--name-only', base, sha], cwd).split('\n').filter(Boolean);
  try {
    git(['rev-parse', '--verify', '--quiet', `${sha}~1`], cwd);
    return git(['diff', '--name-only', `${sha}~1`, sha], cwd).split('\n').filter(Boolean);
  } catch {
    return git(['ls-tree', '-r', '--name-only', sha], cwd).split('\n').filter(Boolean);
  }
}

export async function decide({ api, repository, sha, currentRunId, cwd }) {
  const gates = await gatesPassed(api, repository, sha);
  if (!gates.passed) return { release: false, base: '', reason: gates.reason };
  const base = await lastRequested(api, repository, currentRunId);
  if (base === sha) return { release: false, base, reason: `${sha} already has a release request` };
  if (base && !isAncestor(base, sha, cwd)) {
    return { release: false, base, reason: `${sha} does not descend from the last requested ${base}` };
  }
  const changed = changedPaths(sha, base, cwd);
  const product = changed.filter(file => PRODUCT_PATHS.test(file));
  const range = base ? `${base}..${sha}` : sha;
  return product.length > 0
    ? { release: true, base: base ?? '', reason: `product paths changed in ${range}: ${product.join(' ')}` }
    : { release: false, base: base ?? '', reason: `no product paths changed in ${range}` };
}

function githubApi(token) {
  return async (path) => {
    const response = await fetch(`https://api.github.com${path}`, {
      headers: {
        Accept: 'application/vnd.github+json',
        Authorization: `Bearer ${token}`,
        'X-GitHub-Api-Version': '2022-11-28',
      },
    });
    if (!response.ok) throw new Error(`GET ${path}: HTTP ${response.status}`);
    return response.json();
  };
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  const [sha, runId] = process.argv.slice(2);
  if (!SHA.test(sha ?? '') || !/^\d+$/.test(runId ?? '')) {
    console.error('usage: release-request.mjs <sha> <current-run-id>');
    process.exit(2);
  }
  const result = await decide({
    api: githubApi(process.env.GH_TOKEN),
    repository: process.env.GITHUB_REPOSITORY,
    sha,
    currentRunId: Number(runId),
  });
  console.log(result.reason);
  if (process.env.GITHUB_OUTPUT) {
    appendFileSync(process.env.GITHUB_OUTPUT,
      `release=${result.release}\nbase=${result.base}\n`);
  }
}
