import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { createHash } from 'node:crypto'
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { createRequire } from 'node:module'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { crc32, deflateRawSync, inflateRawSync } from 'node:zlib'
import { test } from 'node:test'
import { readZip, verifiedSource, checkProbe, isolatedEnvironment, checkContainment, checkPriorEvidence, precedingAttempts,
  PORT_SHA, ENGINE_SHA, RUNTIME_HASHES } from './godot.mjs'
import * as qualification from './godot.mjs'

test('actual PowerShell ancestor guard accepts plain DirectoryInfo parents and rejects a file', { skip: process.platform !== 'win32' }, async t => {
  const workflow = await readFile(new URL('../workflows/scaffold-godot-qualification.yml', import.meta.url), 'utf8')
  const block = workflow.slice(workflow.indexOf('          $ancestor = Get-Item'), workflow.indexOf('          $root = Join-Path'))
  const guard = block.split('\n').find(line => line.includes("throw 'Runner temp ancestors"))
  assert.ok(block.length > 0)
  const directory = await mkdtemp(join(dirname(fileURLToPath(import.meta.url)), 'test-temp-'))
  t.after(() => rm(directory, { recursive: true, force: true }))
  await mkdir(join(directory, 'child')); await writeFile(join(directory, 'file'), '')
  const script = `$ErrorActionPreference = 'Stop'
$env:RUNNER_TEMP = Join-Path $env:GODOT_GUARD_TEST 'child'
$plainParent = (Get-Item -LiteralPath $env:RUNNER_TEMP).Parent
if ($null -ne $plainParent.PSObject.Properties['PSIsContainer']) { throw 'Fixture must expose a plain DirectoryInfo parent' }
${block}
$env:RUNNER_TEMP = Join-Path $env:GODOT_GUARD_TEST 'file'
$rejected = $false
try { ${block} } catch { $rejected = $_.Exception.Message -eq 'Runner temp ancestors must be regular directories' }
if (-not $rejected) { throw 'Regular files must not pass the directory guard' }
$ancestor = [pscustomobject]@{ Attributes = [IO.FileAttributes]::Directory -bor [IO.FileAttributes]::ReparsePoint }
$rejected = $false
try { ${guard} } catch { $rejected = $_.Exception.Message -eq 'Runner temp ancestors must be regular directories' }
if (-not $rejected) { throw 'Reparse attributes must not pass the directory guard' }
Write-Output 'ANCESTOR_GUARD_PASS'
`
  const child = spawnSync(join(process.env.ProgramFiles, 'PowerShell', '7', 'pwsh.exe'),
    ['-NoLogo', '-NoProfile', '-NonInteractive', '-Command', script],
    { env: { ...process.env, GODOT_GUARD_TEST: directory }, encoding: 'utf8', timeout: 15000, windowsHide: true })
  assert.equal(child.status, 0, child.stderr); assert.match(child.stdout, /^ANCESTOR_GUARD_PASS\s*$/)
})

// Real ZIP headers/data exercise the extraction boundary without engine downloads.
function zip(entries) {
  const locals = [], central = []; let offset = 0
  for (const e of entries) {
    const name = Buffer.from(e.name), data = Buffer.from(e.body || ''), method = e.method ?? 8
    const packed = method === 0 ? data : deflateRawSync(data)
    const local = Buffer.alloc(30); local.writeUInt32LE(0x04034b50); local.writeUInt16LE(20, 4)
    local.writeUInt16LE(e.flags ?? 0, 6); local.writeUInt16LE(method, 8)
    local.writeUInt32LE(crc32(data), 14); local.writeUInt32LE(packed.length, 18)
    local.writeUInt32LE(data.length, 22); local.writeUInt16LE(name.length, 26)
    const header = Buffer.alloc(46); header.writeUInt32LE(0x02014b50); header.writeUInt16LE(0x0314, 4)
    local.copy(header, 6, 4, 26); header.writeUInt16LE(name.length, 28)
    header.writeUInt32LE(((e.mode ?? 0o100644) << 16) >>> 0, 38); header.writeUInt32LE(offset, 42)
    locals.push(local, name, packed); central.push(header, name); offset += local.length + name.length + packed.length
  }
  const directory = Buffer.concat(central), end = Buffer.alloc(22)
  end.writeUInt32LE(0x06054b50); end.writeUInt16LE(entries.length, 8); end.writeUInt16LE(entries.length, 10)
  end.writeUInt32LE(directory.length, 12); end.writeUInt32LE(offset, 16)
  return Buffer.concat([...locals, directory, end])
}
const blob = body => createHash('sha1').update('blob ' + Buffer.byteLength(body) + '\0').update(body).digest('hex')

test('ZIP extraction returns exact regular-file bytes and rejects corruption and truncation', () => {
  const data = zip([{ name: 'nested/file.txt', body: 'literal Ω\n' }])
  assert.equal(readZip(data).get('nested/file.txt').toString(), 'literal Ω\n')
  for (const bad of [data.subarray(0, -1), Buffer.from(data)]) {
    if (bad.length === data.length) bad[bad.readUInt32LE(bad.length - 6) + 16] ^= 1
    assert.throws(() => readZip(bad))
  }
})

test('ZIP method8 accepts standard deflate compression hints used by the pinned Godot archive', () => {
  for (const flags of [0, 2, 4, 6, 0x802]) {
    const data = zip([{ name: 'Godot_v4.7.2-stable_win64.exe', body: 'MZ exact byte fixture', flags }])
    assert.equal(readZip(data).get('Godot_v4.7.2-stable_win64.exe').toString(), 'MZ exact byte fixture')
    const mismatched = Buffer.from(data)
    mismatched.writeUInt16LE(flags ^ 2, 6)
    assert.throws(() => readZip(mismatched), undefined, 'local and central hints must agree')
  }
})

test('ZIP deflate hints do not admit encryption, reserved flags, unsupported methods or hints on stored entries', () => {
  assert.equal(readZip(zip([{ name: 'file', body: 'stored bytes', method: 0 }])).get('file').toString(), 'stored bytes')
  for (const flags of [2, 4, 6]) assert.throws(() => readZip(zip([{ name: 'file', body: 'stored bytes', method: 0, flags }])))
  for (const flags of [1, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200, 0x400, 0x1000, 0x2000, 0x4000, 0x8000]) {
    assert.throws(() => readZip(zip([{ name: 'file', body: 'deflated bytes', flags: flags | 2 }])))
  }
  assert.throws(() => readZip(zip([{ name: 'file', body: 'unsupported method', method: 9, flags: 2 }])))
})

test('ZIP names, entry types and duplicate identities cannot escape or alias Windows files', () => {
  for (const name of ['../escape', '/absolute', 'C:/absolute', 'a\\b', 'a//b', './a', 'a/../b', 'a./b', 'a /b', 'con.txt', 'x:stream', 'a\0b']) {
    assert.throws(() => readZip(zip([{ name }])), undefined, name)
  }
  for (const mode of [0o120777, 0o060600, 0o020600]) assert.throws(() => readZip(zip([{ name: 'file', mode }])))
  assert.throws(() => readZip(zip([{ name: 'A' }, { name: 'a' }])))
  assert.throws(() => readZip(zip([{ name: 'a' }, { name: 'a/b' }])))
})

test('ZIP bounds reject expansion, missing bytes, encrypted flags, mismatched local names and overlapping records', () => {
  const large = zip([{ name: 'file', body: 'x'.repeat(1000) }])
  assert.throws(() => readZip(large, { maxBytes: 100 }))
  for (const mutate of [b => b.writeUInt16LE(1, 6), b => { b[30] ^= 1 }, b => b.writeUInt32LE(1, b.readUInt32LE(b.length - 6) + 42)]) {
    const bad = Buffer.from(large); mutate(bad); assert.throws(() => readZip(bad))
  }
})

test('port snapshot requires every selected regular blob from the pinned complete tree', () => {
  const names = ['godot/project.godot', 'godot/addons/noisemaker/runtime/nm_backend.gd', 'parity/device_limits_probe.gd', 'parity/shader_compile_sweep.gd']
  const tree = { truncated: false, tree: names.map(path => ({ path, mode: '100644', type: 'blob', size: 4, sha: blob('body') })) }
  const entries = new Map(names.map(path => ['noisemaker-for-godot-' + PORT_SHA + '/' + path, Buffer.from('body')]))
  assert.equal(verifiedSource(entries, tree).size, 4)
  for (const change of [{ truncated: true }, { tree: tree.tree.slice(1) }, { tree: [...tree.tree, tree.tree[0]] },
    { tree: tree.tree.map((row, i) => i ? row : { ...row, mode: '120000' }) }]) assert.throws(() => verifiedSource(entries, { ...tree, ...change }))
  entries.set('noisemaker-for-godot-' + PORT_SHA + '/' + names[0], Buffer.from('evil'))
  assert.throws(() => verifiedSource(entries, tree))
})

const stopped = { exit_code: 0, cleanup_confirmed: true, timed_out: false, aborted: false, output_exceeded: false,
  containment: { job_bound_before_resume: true }, stdout: '', stderr: '' }
const device = { ...stopped, stdout: 'Vulkan 1.3 - Forward+ - NVIDIA GeForce RTX 3060 Ti\nDEVICE_LIMITS_TEST: PASS texture_limit=16384 probed_color_budget=64\n' }
const sweep = { ...stopped, stdout: 'Vulkan 1.3 - Forward+ - NVIDIA GeForce RTX 3060 Ti\nSHADER_SWEEP compiled=796 missing=0 failed=0\n' }

test('only both unchanged probe contracts with actual Vulkan GPU headers can pass', () => {
  assert.equal(checkProbe('device', device).texture_limit, 16384)
  assert.equal(checkProbe('sweep', sweep).compiled, 796)
  for (const change of [{ exit_code: 1 }, { cleanup_confirmed: false }, { timed_out: true }, { aborted: true },
    { output_exceeded: true }, { unavailable: true }, { stderr: 'ERROR: failure' }, { stderr: 'SCRIPT ERROR failure' },
    { stdout: device.stdout.replace('Vulkan', 'OpenGL') }, { stdout: device.stdout.replace('NVIDIA GeForce RTX 3060 Ti', 'llvmpipe') },
    { stdout: device.stdout + 'DEVICE_LIMITS_TEST: PASS texture_limit=1 probed_color_budget=1\n' }]) assert.throws(() => checkProbe('device', { ...device, ...change }))
  for (const output of ['SHADER_SWEEP compiled=0 missing=0 failed=0', 'SHADER_SWEEP compiled=1 missing=1 failed=0', 'SHADER_SWEEP compiled=1 missing=0 failed=1']) {
    assert.throws(() => checkProbe('sweep', { ...sweep, stdout: sweep.stdout.replace(/SHADER_SWEEP[^\n]+/, output) }))
  }
})

test('engine environment is isolated and excludes service credentials and inherited search paths', () => {
  const env = isolatedEnvironment('C:\\scratch\\run', { SystemRoot: 'C:\\Windows', WINDIR: 'C:\\Windows', PATH: 'private-path', GH_TOKEN: 'secret', APPDATA: 'private-profile' })
  assert.equal(env.GH_TOKEN, undefined); assert.equal(env.PATH, 'C:\\Windows\\System32')
  for (const key of ['TEMP', 'TMP', 'HOME', 'USERPROFILE', 'APPDATA', 'LOCALAPPDATA']) assert.ok(env[key].startsWith('C:\\scratch\\run'))
  assert.ok(!JSON.stringify(env).includes('private'))
})

const sha = 'a'.repeat(40), workflowSha = 'b'.repeat(40)
const host = { source_sha: sha, runner: 'largeboi-sync-camera', host_preserved: true, remaining_invocation_processes: 0,
  remaining_invocation_identities: [], components: { registration: true, workspace: true, camera: true, task: true, listeners: true } }
function containment() {
  return new Map([
    ['source-manifest.json', Buffer.from(JSON.stringify({ repository: 'noisefactorllc/scaffold', source_sha: sha,
      files: Object.entries(RUNTIME_HASHES).map(([path, sha256]) => ({ path, sha256 })) }))],
    ['host-preservation.json', Buffer.from(JSON.stringify(host))],
    ['windows-process.tap', Buffer.from('# tests 28\n# pass 28\n# fail 0\n# cancelled 0\n# skipped 0\n# todo 0\n')],
  ])
}
test('containment admission requires exact runtime bindings, complete28-case pass and host preservation', () => {
  assert.equal(checkContainment(containment(), sha).source_sha, sha)
  for (const mutate of [m => m.delete('source-manifest.json'), m => m.set('windows-process.tap', Buffer.from('# tests 28\n# pass 27\n# fail 1\n# skipped 0\n')),
    m => m.set('host-preservation.json', Buffer.from(JSON.stringify({ ...host, remaining_invocation_processes: 1 }))),
    m => m.set('source-manifest.json', Buffer.from(JSON.stringify({ source_sha: sha, files: [] })))]) {
    const m = containment(); mutate(m); assert.throws(() => checkContainment(m, sha))
  }
  assert.throws(() => checkContainment(containment(), workflowSha))
})

const current = { id: 100, run_number: 1, run_attempt: 2, head_sha: workflowSha }
test('prior run enumeration includes every earlier attempt and refuses incomplete or ambiguous API inventories', () => {
  assert.deepEqual(precedingAttempts({ total_count: 1, workflow_runs: [current] }, current), [{ run: current, attempt: 1 }])
  assert.deepEqual(precedingAttempts({ total_count: 1, workflow_runs: [{ ...current, run_attempt: 1 }] }, { ...current, run_attempt: 1 }), [])
  const old = { ...current, id: 99, run_number: 1, run_attempt: 2 }, second = { ...current, run_number: 2 }
  assert.equal(precedingAttempts({ total_count: 2, workflow_runs: [second, old] }, second).length, 3)
  for (const data of [{ total_count: 2, workflow_runs: [current] }, { total_count: 2, workflow_runs: [current, current] },
    { total_count: 0, workflow_runs: [] }, { total_count: 1, workflow_runs: [{ ...current, run_attempt: 99 }] }]) assert.throws(() => precedingAttempts(data, current))
})

test('deleted or missing prior run numbers cannot be mistaken for a first-ever qualification', () => {
  const missing = { ...current, run_number: 3 }
  assert.throws(() => precedingAttempts({ total_count: 1, workflow_runs: [missing] }, missing))
})

test('prior Godot proof requires exact attempt/source identity and complete cleanup even after a fixture failure', () => {
  const run = { ...current, run_attempt: 1, status: 'completed', conclusion: 'failure' }
  const receipt = { schema_version: 1, workflow: '.github/workflows/scaffold-godot-qualification.yml', repository: 'noisefactorllc/sync',
    workflow_sha: workflowSha, scaffold_sha: sha, port_sha: PORT_SHA, engine_sha256: ENGINE_SHA,
    run_id: 100, run_attempt: 1, cleanup_confirmed: true, active_command: null, commands: [] }
  const proof = new Map([['qualification.json', Buffer.from(JSON.stringify(receipt))], ['host-preservation.json', Buffer.from(JSON.stringify(host))],
    ['scaffold-source.json', containment().get('source-manifest.json')]])
  assert.equal(checkPriorEvidence(proof, run, 1), true)
  for (const r of [{ ...run, conclusion: 'cancelled' }, { ...run, head_sha: sha }, { ...run, status: 'in_progress' }]) assert.throws(() => checkPriorEvidence(proof, r, 1))
  assert.throws(() => checkPriorEvidence(proof, run, 2))
  for (const change of [{ cleanup_confirmed: false }, { engine_sha256: '0'.repeat(64) }, { active_command: 'sweep' },
    { commands: [{ id: 'device', cleanup: { cleanup_confirmed: true } }] }, { commands: null }]) {
    proof.set('qualification.json', Buffer.from(JSON.stringify({ ...receipt, ...change })))
    assert.throws(() => checkPriorEvidence(proof, run, 1))
  }
})

test('actual invocation bookkeeping fences before spawn and retains uncertainty or incomplete output evidence', async t => {
  for (const mode of ['stopped-failure', 'unknown-cleanup', 'unconfirmed-cleanup']) {
    await t.test(mode, async t => {
      const directory = await mkdtemp(join(dirname(fileURLToPath(import.meta.url)), 'test-temp-'))
      t.after(() => rm(directory, { recursive: true, force: true }))
      const receipt = { cleanup_confirmed: true, active_command: null, commands: [] }, path = join(directory, 'receipt.json')
      const save = () => writeFile(path, JSON.stringify(receipt))
      const runProcess = async () => {
        const pending = JSON.parse(await readFile(path, 'utf8'))
        assert.equal(pending.cleanup_confirmed, false); assert.equal(pending.active_command, 'device')
        if (mode === 'unknown-cleanup') throw Object.assign(new Error('secret fixture input'), { code: 'process_cleanup_failed' })
        return { ...device, exit_code: 1, cleanup_confirmed: mode !== 'unconfirmed-cleanup' }
      }
      await assert.rejects(qualification.recordInvocation({ id: 'device', argv: [], timeoutMs: 10, cwd: directory, env: {},
        receipt, save, evidence: directory, runProcess }))
      await save()
      const result = JSON.parse(await readFile(path, 'utf8'))
      assert.equal(result.cleanup_confirmed, mode === 'stopped-failure')
      assert.equal(result.active_command, mode === 'stopped-failure' ? null : 'device')
      assert.ok(!JSON.stringify(result).includes('secret fixture input'))
      if (mode === 'stopped-failure') assert.equal(await readFile(join(directory, 'device.stdout.txt'), 'utf8'), device.stdout)
    })
  }
})

function bootstrapFixture() {
  const run = { id: 99, run_attempt: 1, head_sha: workflowSha, path: '.github/workflows/scaffold-godot-qualification.yml',
    head_repository: { full_name: 'noisefactorllc/sync' }, head_branch: 'main', event: 'workflow_dispatch', status: 'completed', conclusion: 'failure' }
  const names = ['Set up job', 'Verify the existing host and retain its baseline', 'Fetch only this reviewed qualification helper',
    'Check qualification helper behavior', 'Require complete prior cleanup and exact-source containment qualification',
    'Mint a Scaffold contents-read token', 'Fetch only qualified Job Object runtime source', 'Run only the pinned portable Godot fixtures',
    'Verify the camera host and workspace were preserved', 'Retain qualification evidence', 'Clean only the proved invocation directory', 'Complete job']
  const job = { id: 8, run_id: run.id, head_sha: run.head_sha, status: 'completed', conclusion: 'failure', runner_id: 21,
    runner_name: 'largeboi-sync-camera', name: 'Portable Godot4.7.2 diagnostic', steps: names.map((name, index) => ({ name,
      number: index + 1, status: 'completed', conclusion: index === 1 ? 'failure' : [0, 11].includes(index) ? 'success' : 'skipped' })) }
  const lines = ["##[group]Run throw 'Runner temp ancestors must be regular directories'", '##[endgroup]',
    'Exception: C:\\actions-runner-sync\\_work\\_temp\\12345678-1234-1234-1234-123456789abc.ps1:14', 'Line |',
    "  14 |  … sePoint)) { throw 'Runner temp ancestors must be regular directories' …", '     |                ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~',
    '     | Runner temp ancestors must be regular directories', '##[error]Process completed with exit code 1.', 'Cleaning up orphan processes']
  const log = Buffer.from(lines.map(line => '2026-09-25T13:00:15.9634551Z ' + line).join('\r\n') + '\r\n')
  return { run, attempt: 1, job, artifactCount: 0,
    workflowSha256: 'c4ac26bfa36e7742403d7b4276f4e567ec54bbea8d2071b9e5b30268a8e16aab', log }
}

test('bootstrap recovery proves the original source stopped before any owned-directory or child launch', () => {
  const fixture = bootstrapFixture()
  assert.equal(createHash('sha256').update(originalBootstrapWorkflow).digest('hex'), fixture.workflowSha256)
  const proof = qualification.checkBootstrapFailure(fixture)
  assert.equal(proof.kind, 'bootstrap_no_engine'); assert.equal(proof.run_id, 99); assert.equal(proof.job_id, 8)
  assert.equal(proof.log_bytes, fixture.log.length)
  assert.equal(proof.log_sha256, createHash('sha256').update(fixture.log).digest('hex'))
  assert.ok(!JSON.stringify(proof).includes('C:\\actions-runner-sync'))
  for (const change of [
    { artifactCount: 1 }, { artifactCount: undefined }, { attempt: 2 }, { workflowSha256: '0'.repeat(64) },
    { run: { ...fixture.run, conclusion: 'cancelled' } }, { run: { ...fixture.run, status: 'in_progress' } },
    { job: { ...fixture.job, conclusion: 'cancelled' } }, { job: { ...fixture.job, status: 'in_progress' } },
    { job: { ...fixture.job, head_sha: sha } }, { job: { ...fixture.job, run_id: 100 } },
    { job: { ...fixture.job, steps: fixture.job.steps.slice(1) } },
    { job: { ...fixture.job, steps: [...fixture.job.steps, fixture.job.steps[0]] } },
    { job: { ...fixture.job, steps: [...fixture.job.steps].reverse() } },
    ...[2, 5, 6, 7].map(index => ({ job: { ...fixture.job, steps: fixture.job.steps.map((s, i) => i === index ? { ...s, conclusion: 'success' } : s) } })),
    { log: Buffer.from(fixture.log.toString().replace('.ps1:14', '.ps1:33')) },
    { log: Buffer.from(fixture.log.toString().replace(/Runner temp ancestors must be regular directories/g, 'Node failed')) },
    { log: Buffer.from(fixture.log.toString().split('Exception:')[0]) },
    { log: Buffer.concat([fixture.log, Buffer.from('2026-09-25T13:00:16.0000000Z ##[error]Another failure\n')]) },
    { log: Buffer.alloc(262145) }, { log: Buffer.from([255]) },
  ]) assert.throws(() => qualification.checkBootstrapFailure({ ...fixture, ...change }))
})

test('bootstrap log transport streams a bounded response and never forwards API credentials to storage', async t => {
  for (const mode of ['valid', 'api-error', 'api-wrong-redirect', 'wrong-host', 'redirect-userinfo', 'redirect-port', 'storage-redirect',
    'storage-error', 'length-limit', 'length-mismatch', 'stream-limit', 'encoding-error', 'fetch-error']) {
    await t.test(mode, async () => {
      const calls = [], token = 'fixture-private-token', fixture = bootstrapFixture()
      const request = async (url, options) => {
        calls.push({ url: String(url), ...options })
        if (mode === 'fetch-error') throw new Error(token)
        if (calls.length === 1) return new Response(null, { status: mode === 'api-error' ? 403 : mode === 'api-wrong-redirect' ? 301 : 302,
          headers: { location: mode === 'wrong-host' ? 'https://example.org/log' : mode === 'redirect-userinfo' ? 'https://private@productionresultssa11.blob.core.windows.net/log'
            : mode === 'redirect-port' ? 'https://productionresultssa11.blob.core.windows.net:444/log' : 'https://productionresultssa11.blob.core.windows.net/log?sig=fixture' } })
        assert.equal(options.headers?.Authorization, undefined); assert.equal(options.redirect, 'error')
        assert.equal(options.signal, calls[0].signal)
        if (mode === 'storage-redirect') return new Response(null, { status: 302, headers: { location: 'https://example.org/log' } })
        if (mode === 'storage-error') return new Response(null, { status: 500 })
        return new Response(mode === 'stream-limit' ? Buffer.alloc(262145) : mode === 'encoding-error' ? Buffer.from([255]) : fixture.log,
          { status: 200, headers: mode === 'length-limit' ? { 'content-length': '262145' } : mode === 'length-mismatch' ? { 'content-length': '1' } : {} })
      }
      const invoke = () => qualification.fetchBootstrapLog(8, token, request)
      if (mode === 'valid') {
        assert.deepEqual(await invoke(), fixture.log); assert.equal(calls.length, 2)
        assert.equal(calls[0].url, 'https://api.github.com/repos/noisefactorllc/sync/actions/jobs/8/logs')
        assert.equal(calls[0].headers.Authorization, 'Bearer ' + token)
      } else await assert.rejects(invoke, error => !error.message.includes(token))
    })
  }
})

test('the actual workflow admission script binds first-run evidence and refuses incomplete, changed or ambiguous API results', async t => {
  const workflow = await readFile(new URL('../workflows/scaffold-godot-qualification.yml', import.meta.url), 'utf8')
  const block = workflow.split('      - name: Require complete prior cleanup and exact-source containment qualification\n')[1].split('\n      - name: ')[0]
  const code = block.split('          script: |\n')[1].split('\n').map(line => line.slice(12)).join('\n')
  const runScript = new (Object.getPrototypeOf(async function () {}).constructor)('require', 'github', 'context', 'fetch', code)
  const helper = await readFile(new URL('./godot.mjs', import.meta.url))
  const base = { head_repository: { full_name: 'noisefactorllc/sync' }, head_branch: 'main', event: 'workflow_dispatch', head_sha: workflowSha }
  const active = { ...base, id: 100, run_number: 1, run_attempt: 1, workflow_id: 10, path: '.github/workflows/scaffold-godot-qualification.yml' }
  const qualified = { ...base, id: 50, run_attempt: 1, status: 'completed', conclusion: 'success', path: '.github/workflows/scaffold-windows-containment.yml' }
  for (const mode of ['first-run', 'safe-prior', 'prior-cleanup-step-failed', 'prior-wrong-attempt', 'prior-cancelled',
    'partial-inventory', 'duplicate-artifact', 'missing-prior-attempt', 'changed-inventory', 'wrong-source', 'wrong-runner',
    'latest-rerun', 'older-rerun-newer-failure', 'older-rerun-newer-running', 'bootstrap-safe', 'bootstrap-wrong-source',
    'bootstrap-log-error', 'bootstrap-artifact-api-error', 'bootstrap-started-engine', 'bootstrap-source-api-error',
    'bootstrap-job-changed', 'bootstrap-attempt-changed', 'bootstrap-artifact-malformed']) {
    await t.test(mode, async t => {
      const root = await mkdtemp(join(dirname(fileURLToPath(import.meta.url)), 'test-temp-'))
      t.after(() => rm(root, { recursive: true, force: true }))
      await mkdir(join(root, '.github/qualification'), { recursive: true }); await mkdir(join(root, 'evidence'))
      await writeFile(join(root, '.github/qualification/godot.mjs'), helper)
      const saved = Object.fromEntries(['SCAFFOLD_WINDOWS_ROOT', 'GITHUB_RUN_ATTEMPT', 'SCAFFOLD_SOURCE_SHA', 'CONTAINMENT_RUN_ID'].map(key => [key, process.env[key]]))
      Object.assign(process.env, { SCAFFOLD_WINDOWS_ROOT: root, GITHUB_RUN_ATTEMPT: '1', SCAFFOLD_SOURCE_SHA: mode === 'wrong-source' ? workflowSha : sha, CONTAINMENT_RUN_ID: '50' })
      let lists = 0
      const bytes = zip([...containment()].map(([name, body]) => ({ name, body })))
      const item = { id: 7, name: 'windows-containment-' + sha + '-50-1', expired: false, workflow_run: { id: 50 },
        size_in_bytes: bytes.length, digest: 'sha256:' + createHash('sha256').update(bytes).digest('hex') }
      const bootstrap = mode.startsWith('bootstrap-'), bootstrapData = bootstrapFixture()
      const source = mode === 'bootstrap-wrong-source' ? Buffer.concat([originalBootstrapWorkflow, Buffer.from('\n# changed\n')]) : originalBootstrapWorkflow
      const sourceBlob = blob(source), sourceTree = 'c'.repeat(40)
      const priorMode = mode === 'safe-prior' || mode.startsWith('prior-') || bootstrap
      const rerun = mode === 'latest-rerun' || mode.startsWith('older-rerun-')
      const newer = { ...active, id: 101, run_number: 2, status: mode.endsWith('-running') ? 'in_progress' : 'completed',
        conclusion: mode.endsWith('-running') ? null : 'failure' }
      const prior = { ...active, id: rerun ? 100 : 99, status: 'completed', conclusion: mode === 'prior-cancelled' ? 'cancelled' : 'failure' }
      const priorReceipt = { schema_version: 1, workflow: active.path, repository: 'noisefactorllc/sync', workflow_sha: workflowSha,
        scaffold_sha: sha, port_sha: PORT_SHA, engine_sha256: ENGINE_SHA, run_id: prior.id, run_attempt: 1, cleanup_confirmed: true, active_command: null, commands: [] }
      const priorBytes = zip([{ name: 'qualification.json', body: JSON.stringify(priorReceipt) },
        { name: 'host-preservation.json', body: JSON.stringify(host) }, { name: 'scaffold-source.json', body: containment().get('source-manifest.json') }])
      const priorItem = { id: 8, name: 'godot-qualification-' + prior.id + '-1', expired: false, workflow_run: { id: prior.id }, size_in_bytes: priorBytes.length,
        digest: 'sha256:' + createHash('sha256').update(priorBytes).digest('hex') }
      const row = mode === 'missing-prior-attempt' || rerun ? { ...active, run_attempt: 2 } : priorMode ? { ...active, run_number: 2 } : active
      if (mode === 'missing-prior-attempt' || rerun) process.env.GITHUB_RUN_ATTEMPT = '2'
      let attemptReads = 0, bootstrapJobReads = 0
      const api = { async auth() { return { token: 'bootstrap-test-token' } }, rest: {
        repos: { async getCommit() {
          if (mode === 'bootstrap-source-api-error') throw new Error('Prior source unavailable')
          return { data: { sha: workflowSha, commit: { tree: { sha: sourceTree } } } }
        } },
        git: { async getTree() { return { data: { truncated: false, tree: [{ path: active.path, type: 'blob', mode: '100644', sha: sourceBlob }] } } },
          async getBlob() { return { data: { encoding: 'base64', size: source.length, content: source.toString('base64') } } } },
        actions: {
        async getWorkflowRun({ run_id }) { return { data: run_id === 100 ? row : qualified } },
        async listWorkflowRuns() { lists++; return { data: { total_count: mode === 'partial-inventory' || priorMode || mode.startsWith('older-rerun-') ? 2 : 1,
          workflow_runs: mode.startsWith('older-rerun-') ? [newer, row] : priorMode ? [row, prior] : [mode === 'changed-inventory' && lists > 1 ? { ...row, run_attempt: 2 } : row] } } },
        async getWorkflowRunAttempt() { attemptReads++; return { data: priorMode || rerun ? { ...prior,
          run_attempt: mode === 'prior-wrong-attempt' || mode === 'bootstrap-attempt-changed' && attemptReads > 1 ? 2 : 1 }
          : { ...active, run_attempt: 1, status: 'completed', conclusion: 'cancelled' } } },
        async listWorkflowRunArtifacts({ run_id }) {
          if (run_id === prior.id && mode === 'bootstrap-artifact-api-error') throw new Error('Artifact API unavailable')
          if (run_id === prior.id && mode === 'bootstrap-artifact-malformed') return { data: { total_count: 0, artifacts: '' } }
          const rows = run_id === prior.id && bootstrap ? [] : run_id === prior.id && (priorMode || rerun) ? [priorItem] : run_id === 100 ? [] : mode === 'duplicate-artifact' ? [item, item] : [item]
          return { data: { total_count: rows.length, artifacts: rows } }
        },
        async downloadArtifact({ artifact_id }) { return { data: artifact_id === 8 ? priorBytes : bytes } },
      } }, async request(route, { run_id }) {
        if (run_id === prior.id && bootstrap) {
          bootstrapJobReads++
          return { data: { total_count: 1, jobs: [{ ...bootstrapData.job,
            steps: bootstrapData.job.steps.map((step, index) => (mode === 'bootstrap-started-engine' || mode === 'bootstrap-job-changed' && bootstrapJobReads > 1) && index === 7
              ? { ...step, conclusion: 'failure' } : step) }] } }
        }
        return { data: { total_count: 1, jobs: [{ run_id, runner_id: mode === 'wrong-runner' ? 22 : 21,
        runner_name: 'largeboi-sync-camera', conclusion: 'success', name: 'Windows process containment ' + sha,
        steps: ['Verify the camera host and workspace were preserved', 'Retain qualification evidence', 'Clean only the proved invocation directory'].map(name =>
          ({ name, conclusion: mode === 'prior-cleanup-step-failed' && name.startsWith('Clean') ? 'failure' : 'success' })) }] } } } }
      let logCalls = 0
      const fetchLog = async () => {
        if (mode === 'bootstrap-log-error') throw new Error('Job log unavailable')
        return ++logCalls === 1 ? new Response(null, { status: 302, headers: { location: 'https://productionresultssa11.blob.core.windows.net/log' } }) : new Response(bootstrapData.log)
      }
      try {
        const call = () => runScript(createRequire(import.meta.url), api, { runId: 100, sha: workflowSha, repo: { owner: 'noisefactorllc', repo: 'sync' } }, fetchLog)
        if (mode === 'first-run' || mode === 'safe-prior' || mode === 'latest-rerun' || mode === 'bootstrap-safe') {
          await call(); const admitted = JSON.parse(await readFile(join(root, 'admission.json'), 'utf8'))
          if (bootstrap) { assert.equal(admitted.predecessors.length, 1); assert.equal(admitted.predecessors[0].kind, 'bootstrap_no_engine') }
          else assert.deepEqual(admitted.predecessors, mode === 'safe-prior' || mode === 'latest-rerun' ? [{ run_id: prior.id, run_attempt: 1, artifact_id: 8,
            sha256: createHash('sha256').update(priorBytes).digest('hex') }] : [])
          assert.equal(admitted.containment_artifact_id, 7)
          assert.equal(admitted.containment_sha256, createHash('sha256').update(bytes).digest('hex'))
        } else { await assert.rejects(call()); await assert.rejects(readFile(join(root, 'admission.json')), { code: 'ENOENT' }) }
      } finally { for (const [key, value] of Object.entries(saved)) { if (value === undefined) delete process.env[key]; else process.env[key] = value } }
    })
  }
})

// Original reviewed bootstrap workflow bytes from Sync 1b0345587eca9753cb40f24f7fbcb31328e64755.
const originalBootstrapWorkflow = inflateRawSync(Buffer.from('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', 'base64'))
