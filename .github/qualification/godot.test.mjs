import assert from 'node:assert/strict'
import { createHash } from 'node:crypto'
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises'
import { createRequire } from 'node:module'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { crc32, deflateRawSync } from 'node:zlib'
import { test } from 'node:test'
import { readZip, verifiedSource, checkProbe, isolatedEnvironment, checkContainment, checkPriorEvidence, precedingAttempts,
  PORT_SHA, ENGINE_SHA, RUNTIME_HASHES } from './godot.mjs'
import * as qualification from './godot.mjs'

// Real ZIP headers/data exercise the extraction boundary without engine downloads.
function zip(entries) {
  const locals = [], central = []; let offset = 0
  for (const e of entries) {
    const name = Buffer.from(e.name), data = Buffer.from(e.body || ''), packed = deflateRawSync(data)
    const local = Buffer.alloc(30); local.writeUInt32LE(0x04034b50); local.writeUInt16LE(20, 4)
    local.writeUInt16LE(8, 8); local.writeUInt32LE(crc32(data), 14); local.writeUInt32LE(packed.length, 18)
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

test('the actual workflow admission script binds first-run evidence and refuses incomplete, changed or ambiguous API results', async t => {
  const workflow = await readFile(new URL('../workflows/scaffold-godot-qualification.yml', import.meta.url), 'utf8')
  const block = workflow.split('      - name: Require complete prior cleanup and exact-source containment qualification\n')[1].split('\n      - name: ')[0]
  const code = block.split('          script: |\n')[1].split('\n').map(line => line.slice(12)).join('\n')
  const runScript = new (Object.getPrototypeOf(async function () {}).constructor)('require', 'github', 'context', code)
  const helper = await readFile(new URL('./godot.mjs', import.meta.url))
  const base = { head_repository: { full_name: 'noisefactorllc/sync' }, head_branch: 'main', event: 'workflow_dispatch', head_sha: workflowSha }
  const active = { ...base, id: 100, run_number: 1, run_attempt: 1, workflow_id: 10, path: '.github/workflows/scaffold-godot-qualification.yml' }
  const qualified = { ...base, id: 50, run_attempt: 1, status: 'completed', conclusion: 'success', path: '.github/workflows/scaffold-windows-containment.yml' }
  for (const mode of ['first-run', 'safe-prior', 'prior-cleanup-step-failed', 'prior-wrong-attempt', 'prior-cancelled',
    'partial-inventory', 'duplicate-artifact', 'missing-prior-attempt', 'changed-inventory', 'wrong-source', 'wrong-runner',
    'latest-rerun', 'older-rerun-newer-failure', 'older-rerun-newer-running']) {
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
      const priorMode = mode === 'safe-prior' || mode.startsWith('prior-')
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
      const api = { rest: { actions: {
        async getWorkflowRun({ run_id }) { return { data: run_id === 100 ? row : qualified } },
        async listWorkflowRuns() { lists++; return { data: { total_count: mode === 'partial-inventory' || priorMode || mode.startsWith('older-rerun-') ? 2 : 1,
          workflow_runs: mode.startsWith('older-rerun-') ? [newer, row] : priorMode ? [row, prior] : [mode === 'changed-inventory' && lists > 1 ? { ...row, run_attempt: 2 } : row] } } },
        async getWorkflowRunAttempt() { return { data: priorMode || rerun ? { ...prior, run_attempt: mode === 'prior-wrong-attempt' ? 2 : 1 }
          : { ...active, run_attempt: 1, status: 'completed', conclusion: 'cancelled' } } },
        async listWorkflowRunArtifacts({ run_id }) { const rows = run_id === prior.id && (priorMode || rerun) ? [priorItem] : run_id === 100 ? [] : mode === 'duplicate-artifact' ? [item, item] : [item]; return { data: { total_count: rows.length, artifacts: rows } } },
        async downloadArtifact({ artifact_id }) { return { data: artifact_id === 8 ? priorBytes : bytes } },
      } }, async request(route, { run_id }) { return { data: { total_count: 1, jobs: [{ run_id, runner_id: mode === 'wrong-runner' ? 22 : 21,
        runner_name: 'largeboi-sync-camera', conclusion: 'success', name: 'Windows process containment ' + sha,
        steps: ['Verify the camera host and workspace were preserved', 'Retain qualification evidence', 'Clean only the proved invocation directory'].map(name =>
          ({ name, conclusion: mode === 'prior-cleanup-step-failed' && name.startsWith('Clean') ? 'failure' : 'success' })) }] } } } }
      try {
        const call = () => runScript(createRequire(import.meta.url), api, { runId: 100, sha: workflowSha, repo: { owner: 'noisefactorllc', repo: 'sync' } })
        if (mode === 'first-run' || mode === 'safe-prior' || mode === 'latest-rerun') {
          await call(); const admitted = JSON.parse(await readFile(join(root, 'admission.json'), 'utf8'))
          assert.deepEqual(admitted.predecessors, mode === 'safe-prior' || mode === 'latest-rerun' ? [{ run_id: prior.id, run_attempt: 1, artifact_id: 8,
            sha256: createHash('sha256').update(priorBytes).digest('hex') }] : [])
          assert.equal(admitted.containment_artifact_id, 7)
          assert.equal(admitted.containment_sha256, createHash('sha256').update(bytes).digest('hex'))
        } else { await assert.rejects(call()); await assert.rejects(readFile(join(root, 'admission.json')), { code: 'ENOENT' }) }
      } finally { for (const [key, value] of Object.entries(saved)) { if (value === undefined) delete process.env[key]; else process.env[key] = value } }
    })
  }
})
