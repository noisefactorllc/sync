import { createHash } from 'node:crypto'
import { lstat, mkdir, readFile, writeFile } from 'node:fs/promises'
import { dirname, join, resolve, win32 } from 'node:path'
import { pathToFileURL } from 'node:url'
import { inflateRawSync } from 'node:zlib'

export const PORT_SHA = 'c1e9928a922b70bf1820c5097f45e2bb96da3c7e'
export const ENGINE_SHA = '731980f9608d61333e5baf54a2ef17210acc7a538446c0cb9969f002aca1e953'
export const ENGINE_URL = 'https://github.com/godotengine/godot-builds/releases/download/4.7.2-stable/Godot_v4.7.2-stable_win64.exe.zip'
export const RUNTIME_HASHES = {
  'apps/scaffold-cloud/lib/windows-process.mjs': 'c8608bd1ba864327a99938d3f637c005cda6ab70ba7f09070cdf7217af39c725',
  'apps/scaffold-cloud/runtime/windows-process.cs': 'f30dcacb0e6cd96cd716b3defa78f7069a29e7a21f1f3ce7c8a8407c3d965be4',
  'apps/scaffold-cloud/runtime/windows-process.ps1': '6521d4144a51e11747188ab52c89bb6ee5d394aa9902a3586ebb981be4b73bd6',
}
const HASH = /^[a-f0-9]{64}$/, SHA = /^[a-f0-9]{40}$/
const fail = () => { throw new Error('Godot qualification evidence or input is invalid.') }
const check = value => { if (!value) fail() }
const hash = bytes => createHash('sha256').update(bytes).digest('hex')
const json = bytes => JSON.parse(bytes.toString('utf8').replace(/^\uFEFF/, ''))
const integer = value => Number.isSafeInteger(value) && value > 0
const COMPONENTS = ['registration', 'workspace', 'camera', 'task', 'listeners']
// github-script v7 uses Node20, before node:zlib exposed crc32.
const CRC_TABLE = Uint32Array.from({ length: 256 }, (_, value) => {
  for (let bit = 0; bit < 8; bit++) value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1
  return value >>> 0
})
function crc32(bytes) {
  let value = 0xffffffff
  for (const byte of bytes) value = CRC_TABLE[(value ^ byte) & 255] ^ (value >>> 8)
  return (value ^ 0xffffffff) >>> 0
}

function archiveName(name) {
  check(typeof name === 'string' && name.length > 0 && name.length <= 240 && !/[\\\x00-\x1f<>:"|?*]/.test(name))
  const parts = name.replace(/\/$/, '').split('/')
  check(parts.every(p => p && p !== '.' && p !== '..' && !/[. ]$/.test(p) && !/^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(p)))
  return parts.join('/').toLowerCase()
}

// Parse before writing anything. ZIP64, encryption, multipart archives and aliasing fail closed.
export function readZip(buffer, { maxBytes = 512 * 1024 * 1024 } = {}) {
  check(Buffer.isBuffer(buffer) && buffer.length >= 22 && buffer.length <= 128 * 1024 * 1024)
  let end = -1
  for (let p = buffer.length - 22; p >= Math.max(0, buffer.length - 65557); p--) {
    if (buffer.readUInt32LE(p) === 0x06054b50 && p + 22 + buffer.readUInt16LE(p + 20) === buffer.length) { end = p; break }
  }
  check(end >= 0 && buffer.readUInt16LE(end + 4) === 0 && buffer.readUInt16LE(end + 6) === 0)
  const count = buffer.readUInt16LE(end + 10), start = buffer.readUInt32LE(end + 16)
  check(count > 0 && count <= 4096 && buffer.readUInt16LE(end + 8) === count && start + buffer.readUInt32LE(end + 12) === end)
  const entries = [], identities = new Map(); let cursor = start, expanded = 0
  const decoder = new TextDecoder('utf-8', { fatal: true })
  for (let i = 0; i < count; i++) {
    check(cursor + 46 <= end && buffer.readUInt32LE(cursor) === 0x02014b50)
    const flags = buffer.readUInt16LE(cursor + 8), method = buffer.readUInt16LE(cursor + 10)
    const crc = buffer.readUInt32LE(cursor + 16), size = buffer.readUInt32LE(cursor + 24), packed = buffer.readUInt32LE(cursor + 20)
    const length = buffer.readUInt16LE(cursor + 28), extra = buffer.readUInt16LE(cursor + 30), comment = buffer.readUInt16LE(cursor + 32)
    const attributes = buffer.readUInt32LE(cursor + 38), offset = buffer.readUInt32LE(cursor + 42)
    check(cursor + 46 + length + extra + comment <= end && buffer.readUInt16LE(cursor + 34) === 0)
    const nameBytes = buffer.subarray(cursor + 46, cursor + 46 + length), name = decoder.decode(nameBytes), key = archiveName(name)
    const directory = name.endsWith('/'), type = (attributes >>> 16) & 0xf000
    // APPNOTE4.4.4: bits1/2 describe compression level for deflate; they are undefined for stored entries.
    const allowedFlags = method === 8 ? 0x080e : 0x0808
    check(!identities.has(key) && (flags & ~allowedFlags) === 0 && [0, 8].includes(method) && !(attributes & 0x400))
    check(directory ? [0, 0x4000].includes(type) && size === 0 : [0, 0x8000].includes(type) && !(attributes & 0x10))
    expanded += size; check(expanded <= maxBytes && size <= 256 * 1024 * 1024)
    check(offset + 30 <= start && buffer.readUInt32LE(offset) === 0x04034b50)
    check(buffer.readUInt16LE(offset + 6) === flags && buffer.readUInt16LE(offset + 8) === method && buffer.readUInt16LE(offset + 26) === length)
    check(buffer.subarray(offset + 30, offset + 30 + length).equals(nameBytes))
    const dataStart = offset + 30 + length + buffer.readUInt16LE(offset + 28), dataEnd = dataStart + packed
    check(dataEnd <= start)
    let recordEnd = dataEnd
    if (flags & 8) {
      check(recordEnd + 12 <= start)
      if (buffer.readUInt32LE(recordEnd) === 0x08074b50) recordEnd += 4
      check(recordEnd + 12 <= start && buffer.readUInt32LE(recordEnd) === crc && buffer.readUInt32LE(recordEnd + 4) === packed && buffer.readUInt32LE(recordEnd + 8) === size)
      recordEnd += 12
    } else check(buffer.readUInt32LE(offset + 14) === crc && buffer.readUInt32LE(offset + 18) === packed && buffer.readUInt32LE(offset + 22) === size)
    identities.set(key, directory); entries.push({ name, key, directory, method, crc, size, offset, dataStart, dataEnd, recordEnd })
    cursor += 46 + length + extra + comment
  }
  check(cursor === end)
  let boundary = 0
  for (const e of [...entries].sort((a, b) => a.offset - b.offset)) { check(e.offset === boundary); boundary = e.recordEnd }
  check(boundary === start)
  const files = new Map()
  for (const e of entries) {
    const parts = e.key.split('/'); parts.pop()
    while (parts.length) { check(identities.get(parts.join('/')) !== false); parts.pop() }
    const packed = buffer.subarray(e.dataStart, e.dataEnd)
    const data = e.method === 0 ? packed : inflateRawSync(packed, { maxOutputLength: Math.max(1, e.size) })
    check(data.length === e.size && crc32(data) === e.crc)
    if (!e.directory) files.set(e.name, data)
  }
  return files
}

export function verifiedSource(archive, tree) {
  check(tree?.truncated === false && Array.isArray(tree.tree) && tree.tree.length <= 4096)
  const files = new Map(), seen = new Set(), prefix = 'noisemaker-for-godot-' + PORT_SHA + '/'
  for (const row of tree.tree) {
    if (!(row.path.startsWith('godot/') && !row.path.endsWith('.md') || ['parity/device_limits_probe.gd', 'parity/shader_compile_sweep.gd'].includes(row.path)) || row.type === 'tree') continue
    const identity = archiveName(row.path)
    check(!seen.has(identity) && row.type === 'blob' && ['100644', '100755'].includes(row.mode) && SHA.test(row.sha))
    seen.add(identity)
    const bytes = archive.get(prefix + row.path)
    check(bytes && bytes.length === row.size && bytes.length <= 2 * 1024 * 1024)
    check(createHash('sha1').update('blob ' + bytes.length + '\0').update(bytes).digest('hex') === row.sha)
    files.set(row.path, bytes)
  }
  for (const path of ['godot/project.godot', 'godot/addons/noisemaker/runtime/nm_backend.gd', 'parity/device_limits_probe.gd', 'parity/shader_compile_sweep.gd']) check(files.has(path))
  return files
}

export function isolatedEnvironment(root, inherited) {
  check(win32.isAbsolute(root) && typeof inherited.SystemRoot === 'string' && win32.isAbsolute(inherited.SystemRoot))
  return { SystemRoot: inherited.SystemRoot, WINDIR: inherited.SystemRoot, PATH: win32.join(inherited.SystemRoot, 'System32'),
    HOME: win32.join(root, 'home'), USERPROFILE: win32.join(root, 'home'), APPDATA: win32.join(root, 'home', 'AppData', 'Roaming'),
    LOCALAPPDATA: win32.join(root, 'home', 'AppData', 'Local'), TEMP: win32.join(root, 'scratch'), TMP: win32.join(root, 'scratch') }
}

function cleanResult(result) {
  check(result?.cleanup_confirmed === true && result.containment?.job_bound_before_resume === true && result.exit_code === 0 &&
    !result.timed_out && !result.aborted && !result.output_exceeded && !result.unavailable)
}
export function checkProbe(kind, result) {
  cleanResult(result)
  const text = result.stdout + '\n' + result.stderr
  check(!/ERROR:|SCRIPT ERROR/i.test(text) && /Vulkan [0-9.]+[^\r\n]*Forward\+[^\r\n]*NVIDIA[^\r\n]*RTX 3060 Ti/.test(text))
  if (kind === 'device') {
    const matches = [...text.matchAll(/^DEVICE_LIMITS_TEST: PASS texture_limit=(\d+) probed_color_budget=(\d+)\s*$/gm)]
    check(matches.length === 1 && Number(matches[0][1]) > 0 && [16, 32, 40, 48, 64].includes(Number(matches[0][2])))
    return { texture_limit: Number(matches[0][1]), color_budget: Number(matches[0][2]) }
  }
  check(kind === 'sweep')
  const matches = [...text.matchAll(/^SHADER_SWEEP compiled=(\d+) missing=(\d+) failed=(\d+)\s*$/gm)]
  check(matches.length === 1 && Number(matches[0][1]) > 0 && matches[0][2] === '0' && matches[0][3] === '0')
  return { compiled: Number(matches[0][1]), missing: 0, failed: 0 }
}

function preserved(host, sha) {
  check(host?.source_sha === sha && host.runner === 'largeboi-sync-camera' && host.host_preserved === true &&
    host.remaining_invocation_processes === 0 && Array.isArray(host.remaining_invocation_identities) && host.remaining_invocation_identities.length === 0 &&
    COMPONENTS.every(key => host.components?.[key] === true))
}
function runtimeManifest(manifest, sha) {
  check(SHA.test(sha))
  check(manifest.repository === 'noisefactorllc/scaffold' && manifest.source_sha === sha && Array.isArray(manifest.files))
  const seen = new Set()
  for (const file of manifest.files) { check(!seen.has(file.path) && HASH.test(file.sha256)); seen.add(file.path) }
  for (const [path, expected] of Object.entries(RUNTIME_HASHES)) check(manifest.files.find(file => file.path === path)?.sha256 === expected)
  return manifest
}
export function checkContainment(files, sha) {
  const manifest = runtimeManifest(json(files.get('source-manifest.json')), sha)
  preserved(json(files.get('host-preservation.json')), sha)
  const tap = files.get('windows-process.tap').toString('utf8')
  for (const [field, value] of Object.entries({ tests: 28, pass: 28, fail: 0, cancelled: 0, skipped: 0, todo: 0 })) {
    const matches = [...tap.matchAll(new RegExp('^# ' + field + ' (\\d+)\\s*$', 'gm'))]
    check(matches.length === 1 && Number(matches[0][1]) === value)
  }
  check(!/^not ok /m.test(tap))
  return manifest
}

export function precedingAttempts(inventory, current) {
  check(integer(current?.run_number) && current.run_number < 100)
  check(inventory && Number.isInteger(inventory.total_count) && inventory.total_count > 0 && inventory.total_count < 100 &&
    Array.isArray(inventory.workflow_runs) && inventory.workflow_runs.length === inventory.total_count)
  const identities = new Set(), numbers = new Set(), previous = []
  for (const run of inventory.workflow_runs) {
    check(integer(run.id) && integer(run.run_number) && integer(run.run_attempt) && run.run_attempt <= 5 && SHA.test(run.head_sha) &&
      !identities.has(run.id) && !numbers.has(run.run_number))
    identities.add(run.id); numbers.add(run.run_number)
    if (run.id === current.id) check(run.run_number === current.run_number && run.run_attempt === current.run_attempt && run.head_sha === current.head_sha)
    // Only the newest run may proceed; rerunning an older run cannot bypass later unresolved work.
    check(run.run_number <= current.run_number)
    const count = run.id === current.id ? current.run_attempt - 1 : run.run_attempt
    for (let attempt = 1; attempt <= count; attempt++) previous.push({ run, attempt })
  }
  check(identities.has(current.id) && previous.length <= 64)
  for (let number = 1; number < current.run_number; number++) check(numbers.has(number))
  return previous
}
export function checkPriorEvidence(files, run, attempt) {
  check(run.status === 'completed' && ['success', 'failure'].includes(run.conclusion) && run.run_attempt === attempt)
  const receipt = json(files.get('qualification.json'))
  check(receipt.schema_version === 1 && receipt.workflow === '.github/workflows/scaffold-godot-qualification.yml' &&
    receipt.repository === 'noisefactorllc/sync' && receipt.workflow_sha === run.head_sha && receipt.run_id === run.id &&
    receipt.run_attempt === attempt && receipt.port_sha === PORT_SHA && receipt.engine_sha256 === ENGINE_SHA &&
    receipt.cleanup_confirmed === true && receipt.active_command === null && Array.isArray(receipt.commands) && receipt.commands.length <= 4)
  runtimeManifest(json(files.get('scaffold-source.json')), receipt.scaffold_sha)
  for (const [index, command] of receipt.commands.entries()) {
    check(command.id === ['version', 'help', 'device', 'sweep'][index] && command.cleanup?.cleanup_confirmed === true &&
      command.cleanup.job_bound_before_resume === true && command.cleanup.job_empty === true && command.cleanup.active_processes === 0 &&
      command.cleanup.capture_completed === true && command.cleanup.stdin_completed === true)
    check(Array.isArray(command.output) && command.output.length === 2)
    for (const [stream, output] of command.output.entries()) {
      const expected = command.id + '.' + ['stdout', 'stderr'][stream] + '.txt', bytes = files.get(expected)
      check(output.path === expected && bytes && bytes.length === output.bytes && hash(bytes) === output.sha256)
    }
  }
  preserved(json(files.get('host-preservation.json')), receipt.scaffold_sha)
  return true
}

// This reviewed source throws at baseline line14 before directory creation or its first child launch.
const BOOTSTRAP_SOURCE = 'c4ac26bfa36e7742403d7b4276f4e567ec54bbea8d2071b9e5b30268a8e16aab'
export function checkBootstrapFailure({ run, attempt, job, artifactCount, workflowSha256, log }) {
  check(workflowSha256 === BOOTSTRAP_SOURCE && artifactCount === 0 && integer(attempt) && run.run_attempt === attempt &&
    integer(run.id) && SHA.test(run.head_sha) && run.path === '.github/workflows/scaffold-godot-qualification.yml' &&
    run.head_repository?.full_name === 'noisefactorllc/sync' && run.head_branch === 'main' && run.event === 'workflow_dispatch' &&
    run.status === 'completed' && run.conclusion === 'failure')
  check(integer(job.id) && job.run_id === run.id && job.head_sha === run.head_sha && job.status === 'completed' &&
    job.conclusion === 'failure' && job.runner_id === 21 && job.runner_name === 'largeboi-sync-camera' && job.name === 'Portable Godot4.7.2 diagnostic')
  const names = ['Set up job', 'Verify the existing host and retain its baseline', 'Fetch only this reviewed qualification helper',
    'Check qualification helper behavior', 'Require complete prior cleanup and exact-source containment qualification',
    'Mint a Scaffold contents-read token', 'Fetch only qualified Job Object runtime source', 'Run only the pinned portable Godot fixtures',
    'Verify the camera host and workspace were preserved', 'Retain qualification evidence', 'Clean only the proved invocation directory', 'Complete job']
  check(Array.isArray(job.steps) && job.steps.length === names.length)
  for (const [index, name] of names.entries()) {
    const step = job.steps[index]
    check(step.name === name && step.number === index + 1 && step.status === 'completed' &&
      step.conclusion === (index === 1 ? 'failure' : [0, 11].includes(index) ? 'success' : 'skipped'))
  }
  check(Buffer.isBuffer(log) && log.length > 0 && log.length <= 262144)
  const text = new TextDecoder('utf-8', { fatal: true }).decode(log).replace(/\x1b\[[0-9;]*m/g, '')
  check(!/[\x00-\x08\x0b\x0c\x0e-\x1f]/.test(text))
  const lines = text.trimEnd().split(/\r?\n/).map(line => {
    check(/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{1,7}Z /.test(line))
    return line.slice(line.indexOf(' ') + 1)
  })
  const echoEnd = lines.lastIndexOf('##[endgroup]'), tail = lines.slice(echoEnd + 1)
  check(echoEnd >= 0 && tail.length === 7 && lines.filter(line => line.startsWith('Exception:')).length === 1 &&
    lines.filter(line => line.startsWith('##[error]')).length === 1)
  check(/^Exception: C:\\actions-runner-sync\\_work\\_temp\\[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}\.ps1:14$/.test(tail[0]) &&
    tail[1] === 'Line |' && /^\s*14 \| .*throw 'Runner temp ancestors must be regular directories' .*$/u.test(tail[2]) &&
    /^\s*\|\s+~+\s*$/.test(tail[3]) && /^\s*\| Runner temp ancestors must be regular directories$/.test(tail[4]) &&
    tail[5] === '##[error]Process completed with exit code 1.' && tail[6] === 'Cleaning up orphan processes')
  return { kind: 'bootstrap_no_engine', run_id: run.id, run_attempt: attempt, job_id: job.id, workflow_sha: run.head_sha,
    workflow_sha256: workflowSha256, reason: 'ancestor_guard_before_child', log_bytes: log.length, log_sha256: hash(log),
    steps: job.steps.map(({ number, name, conclusion }) => ({ number, name, conclusion })) }
}

export async function fetchBootstrapLog(jobId, token, request = fetch) {
  try {
    check(integer(jobId) && typeof token === 'string' && token.length > 0)
    const signal = AbortSignal.timeout(20000)
    const api = await request('https://api.github.com/repos/noisefactorllc/sync/actions/jobs/' + jobId + '/logs',
      { signal, redirect: 'manual', headers: { Authorization: 'Bearer ' + token, 'X-GitHub-Api-Version': '2022-11-28' } })
    const location = api.headers.get('location'); await api.body?.cancel()
    check(api.status === 302 && location)
    const url = new URL(location)
    check(url.protocol === 'https:' && !url.username && !url.password && !url.port && !url.hash &&
      /^productionresultssa[0-9]+\.blob\.core\.windows\.net$/.test(url.hostname))
    // GitHub documents this one redirect. Storage receives no GitHub credentials and cannot redirect again.
    const response = await request(url.href, { signal, redirect: 'error' })
    const declared = response.headers.get('content-length')
    check(response.status === 200 && response.body && (declared === null || /^\d+$/.test(declared) && Number(declared) <= 262144))
    const reader = response.body.getReader(), chunks = []; let length = 0
    try {
      while (true) {
        const next = await reader.read(); if (next.done) break
        length += next.value.length; check(length <= 262144); chunks.push(next.value)
      }
    } finally { await reader.cancel() }
    check(length > 0 && (declared === null || Number(declared) === length))
    const bytes = Buffer.concat(chunks)
    new TextDecoder('utf-8', { fatal: true }).decode(bytes)
    return bytes
  } catch { fail() } // Never print the token, signed storage URL or remote response text.
}

export async function fetchBytes(url, maximum) {
  const signal = AbortSignal.timeout(120000)
  for (let redirects = 0; redirects <= 4; redirects++) {
    const parsed = new URL(url)
    check(parsed.protocol === 'https:' && ['github.com', 'api.github.com', 'codeload.github.com', 'release-assets.githubusercontent.com'].includes(parsed.hostname) && !parsed.username && !parsed.password)
    const response = await fetch(parsed, { signal, redirect: 'manual', headers: { 'User-Agent': 'scaffold-godot-qualification' } })
    if ([301, 302, 303, 307, 308].includes(response.status)) { url = new URL(response.headers.get('location'), parsed).href; continue }
    check(response.ok && Number(response.headers.get('content-length') || 0) <= maximum)
    const parts = []; let length = 0
    for await (const part of response.body) { length += part.length; check(length <= maximum); parts.push(part) }
    return Buffer.concat(parts)
  }
  fail()
}

async function writeFiles(root, files) {
  for (const [path, bytes] of files) {
    archiveName(path)
    const target = join(root, path)
    await mkdir(dirname(target), { recursive: true })
    await writeFile(target, bytes, { flag: 'wx' })
  }
}

export async function recordInvocation({ id, argv, timeoutMs, cwd, env, signal, receipt, save, evidence, runProcess }) {
  receipt.cleanup_confirmed = false; receipt.active_command = id; await save()
  let result
  try { result = await runProcess({ argv, cwd, env, timeoutMs, maxOutputBytes: 1024 * 1024, signal }) }
  catch (error) { receipt.failure = error.code === 'process_cleanup_failed' ? 'process_cleanup_failed' : 'runtime_unavailable'; throw error }
  receipt.cleanup_confirmed = result.cleanup_confirmed === true && result.containment?.job_bound_before_resume === true
  const output = []
  for (const stream of ['stdout', 'stderr']) {
    const bytes = Buffer.from(result[stream], 'utf8'), name = id + '.' + stream + '.txt'
    await writeFile(join(evidence, name), bytes, { flag: 'wx' }); output.push({ path: name, bytes: bytes.length, sha256: hash(bytes) })
  }
  receipt.commands.push({ id, exit_code: result.exit_code, timed_out: result.timed_out, aborted: result.aborted,
    output_exceeded: result.output_exceeded, cleanup: result.containment?.cleanup, api: result.containment?.api, output })
  if (receipt.cleanup_confirmed) receipt.active_command = null
  await save(); cleanResult(result); return result
}

async function execute(root) {
  check(process.platform === 'win32' && resolve(root) === root && dirname(root).toLowerCase() === resolve(process.env.RUNNER_TEMP).toLowerCase())
  const admission = json(await readFile(join(root, 'admission.json')))
  check(admission.workflow_sha === process.env.GITHUB_SHA && admission.run_id === Number(process.env.GITHUB_RUN_ID) && admission.run_attempt === Number(process.env.GITHUB_RUN_ATTEMPT))
  checkContainment(readZip(await readFile(join(root, 'containment.zip')), { maxBytes: 8 * 1024 * 1024 }), admission.scaffold_sha)
  for (const [file, expected] of Object.entries(RUNTIME_HASHES)) {
    const path = join(root, file), stat = await lstat(path)
    check(stat.isFile() && !stat.isSymbolicLink() && hash(await readFile(path)) === expected)
  }
  const { runWindowsProcess } = await import(pathToFileURL(join(root, 'apps/scaffold-cloud/lib/windows-process.mjs')).href)
  const evidence = join(root, 'evidence'), payload = join(root, 'payload'), environment = isolatedEnvironment(payload, process.env)
  const receipt = { schema_version: 1, workflow: '.github/workflows/scaffold-godot-qualification.yml', repository: 'noisefactorllc/sync',
    workflow_sha: admission.workflow_sha, scaffold_sha: admission.scaffold_sha, run_id: admission.run_id, run_attempt: admission.run_attempt,
    port_sha: PORT_SHA, engine_sha256: ENGINE_SHA, engine_asset_id: 519678169,
    cleanup_confirmed: true, active_command: null, qualified: false, stage: 'source_metadata', commands: [] }
  const save = () => writeFile(join(evidence, 'qualification.json'), JSON.stringify(receipt, null, 2))
  await save()
  const cancellation = new AbortController()
  const abort = () => cancellation.abort()
  process.on('SIGINT', abort); process.on('SIGTERM', abort)
  try {
    for (const directory of new Set([payload, ...['HOME', 'APPDATA', 'LOCALAPPDATA', 'TEMP'].map(key => environment[key])])) await mkdir(directory, { recursive: true })
    const commit = json(await fetchBytes('https://api.github.com/repos/noisefactorllc/noisemaker-for-godot/commits/' + PORT_SHA, 2 * 1024 * 1024))
    check(commit.sha === PORT_SHA && SHA.test(commit.commit?.tree?.sha))
    const tree = json(await fetchBytes('https://api.github.com/repos/noisefactorllc/noisemaker-for-godot/git/trees/' + commit.commit.tree.sha + '?recursive=1', 4 * 1024 * 1024))
    check(tree.sha === commit.commit.tree.sha)
    receipt.stage = 'source_archive'; await save()
    const sourceZip = await fetchBytes('https://codeload.github.com/noisefactorllc/noisemaker-for-godot/zip/' + PORT_SHA, 16 * 1024 * 1024)
    receipt.source_archive = { bytes: sourceZip.length, sha256: hash(sourceZip) }; await save()
    const files = verifiedSource(readZip(sourceZip, { maxBytes: 16 * 1024 * 1024 }), tree)
    await writeFiles(join(payload, 'source'), files)
    await writeFile(join(evidence, 'port-source.json'), JSON.stringify({ repository: 'noisefactorllc/noisemaker-for-godot', sha: PORT_SHA,
      tree_sha: tree.sha, archive_sha256: hash(sourceZip), files: [...files].map(([path, bytes]) => ({ path, bytes: bytes.length, sha256: hash(bytes) })) }, null, 2))
    receipt.stage = 'engine_archive'; await save()
    const engineZip = await fetchBytes(ENGINE_URL, 86013866)
    receipt.engine_archive = { bytes: engineZip.length, sha256: hash(engineZip) }; await save()
    check(engineZip.length === 86013866 && hash(engineZip) === ENGINE_SHA)
    const engineFiles = readZip(engineZip), executableName = 'Godot_v4.7.2-stable_win64_console.exe'
    check(engineFiles.size === 2 && engineFiles.has(executableName) && engineFiles.has('Godot_v4.7.2-stable_win64.exe'))
    for (const bytes of engineFiles.values()) check(bytes.length > 2 && bytes[0] === 77 && bytes[1] === 90)
    await writeFiles(join(payload, 'engine'), engineFiles)
    await writeFile(join(payload, 'engine', '._sc_'), '', { flag: 'wx' })
    receipt.engine_files = [...engineFiles].map(([path, bytes]) => ({ path, bytes: bytes.length, sha256: hash(bytes) }))
    const executable = join(payload, 'engine', executableName), source = join(payload, 'source')
    const run = (id, argv, timeoutMs) => {
      receipt.stage = id
      return recordInvocation({ id, argv, timeoutMs, cwd: payload, env: environment,
        signal: cancellation.signal, receipt, save, evidence, runProcess: runWindowsProcess })
    }
    const version = await run('version', [executable, '--version'], 15000)
    check(/^4\.7\.2\.stable\.official\.[a-f0-9]+\s*$/.test(version.stdout) && !version.stderr.trim())
    receipt.engine_version = version.stdout.trim()
    const help = await run('help', [executable, '--help'], 15000)
    check(['--rendering-driver', '--rendering-method', '--display-driver', '--audio-driver', 'vulkan', 'windows', 'Dummy'].every(value => help.stdout.includes(value)))
    for (const [id, script, timeout] of [['device', 'device_limits_probe.gd', 60000], ['sweep', 'shader_compile_sweep.gd', 300000]]) {
      const result = await run(id, [executable, '--path', join(source, 'godot'), '--script', join(source, 'parity', script), '--position', '5000,5000',
        '--rendering-method', 'forward_plus', '--rendering-driver', 'vulkan', '--display-driver', 'windows', '--audio-driver', 'Dummy',
        '--windowed', '--resolution', '64x64', '--single-window'], timeout)
      receipt.commands.at(-1).probe = checkProbe(id, result); await save()
    }
    receipt.qualified = true; receipt.stage = 'complete'
  } finally { process.off('SIGINT', abort); process.off('SIGTERM', abort); await save() }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  execute(process.argv[2]).catch(() => { process.stderr.write('Godot qualification failed; inspect the bounded retained evidence.\n'); process.exitCode = 1 })
}
