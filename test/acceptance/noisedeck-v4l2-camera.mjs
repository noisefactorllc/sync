import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import crypto from 'node:crypto'
import { createServer } from 'node:http'
import { createRequire } from 'node:module'
import { mkdtemp, mkdir, readFile, rm, stat, writeFile } from 'node:fs/promises'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'
import { pathToFileURL } from 'node:url'

const REQUIRED_ENVIRONMENT = [
  'SYNC_DAEMON_PATH',
  'SYNC_CTL_PATH',
  'SYNC_CAMERA_DEVICE',
  'SYNC_V4L2_RECEIVER_PATH',
  'NOISEDECK_APP_DIR',
  'SYNC_ACCEPTANCE_ARTIFACT_DIR',
  'CHROMIUM_EXECUTABLE_PATH',
]
const TOKEN_PATTERN = /\b[0-9a-f]{64}\b/gi
const FORBIDDEN_CHROMIUM_FLAGS = [
  '--use-fake-device-for-media-stream',
  '--use-file-for-fake-video-capture',
  '--use-fake-ui-for-media-stream',
]

export function validateAcceptanceEnvironment(environment) {
  const errors = []
  for (const name of REQUIRED_ENVIRONMENT) {
    const value = environment[name]
    if (typeof value !== 'string' || !path.isAbsolute(value)) {
      errors.push(`${name} must be an absolute path`)
    }
  }
  if (!/^\/dev\/video[0-9]+$/.test(environment.SYNC_CAMERA_DEVICE ?? '')) {
    errors.push('SYNC_CAMERA_DEVICE must be exactly /dev/videoN')
  }
  return errors
}

export function assertRealChromiumArguments(arguments_) {
  for (const argument of arguments_) {
    if (FORBIDDEN_CHROMIUM_FLAGS.some(flag => argument === flag ||
      argument.startsWith(`${flag}=`))) {
      throw new Error(`fake Chromium media flag is forbidden: ${argument}`)
    }
  }
}

export function redactSecrets(value) {
  if (Array.isArray(value)) return value.map(redactSecrets)
  if (value && typeof value === 'object') {
    return Object.fromEntries(Object.entries(value).map(([key, child]) => [
      key,
      /token|digest/i.test(key) ? '[redacted]' : redactSecrets(child),
    ]))
  }
  return typeof value === 'string' ? value.replaceAll(TOKEN_PATTERN, '[redacted]') : value
}

export function assertAcceptanceVerdict(value) {
  assert.equal(value.camera?.driver, 'v4l2 loopback')
  assert.equal(value.camera?.card, 'Sync Camera')
  assert.equal(value.camera?.format, 'NV12')
  assert.deepEqual(value.chromium?.fakeDeviceFlags, [])
  for (const field of ['denied', 'timedOut', 'approved', 'revoked']) {
    assert.equal(value.pairing?.[field], true, `pairing.${field}`)
  }
  for (const field of [
    'sourceChanging', 'outputChanging', 'outputDiffersFromSource',
    'outputIsNotIdleCard',
  ]) assert.equal(value.noisedeck?.[field], true, `noisedeck.${field}`)
  assert.equal(value.independentReceiver?.passed, true)
  assert.ok(value.independentReceiver?.frameCount > 0, 'receiver read zero frames')
  assert.ok(value.independentReceiver?.uniqueChecksumCount > 1,
    'receiver frames were all identical')
  assert.equal(value.daemon?.cameraWritesAdvanced, true)
  assert.equal(value.daemon?.lastWriteAdvanced, true)
  assert.equal(value.soak?.bounded, true)
  assert.equal(value.soak?.slotCount, 3)
  assert.equal(value.soak?.staleWhileLive, false)
  assert.equal(value.soak?.liveFramesAdvanced, true)
}

export function liveSoakProgress(samples) {
  if (!Array.isArray(samples) || samples.length < 2) {
    return {
      receivedFrameGrowth: 0,
      acceptedFrameGrowth: 0,
      cameraDrivingFrameGrowth: 0,
      liveFramesAdvanced: false,
    }
  }
  const first = samples[0]
  const last = samples.at(-1)
  const growth = name => Number.isSafeInteger(first?.[name]) &&
    Number.isSafeInteger(last?.[name]) ? last[name] - first[name] : 0
  const receivedFrameGrowth = growth('receivedFrames')
  const acceptedFrameGrowth = growth('acceptedFrames')
  const cameraDrivingFrameGrowth = growth('cameraDrivingFrames')
  return {
    receivedFrameGrowth,
    acceptedFrameGrowth,
    cameraDrivingFrameGrowth,
    liveFramesAdvanced: receivedFrameGrowth > 0 && acceptedFrameGrowth > 0 &&
      cameraDrivingFrameGrowth > 0,
  }
}

function sleep(milliseconds) {
  return new Promise(resolve => setTimeout(resolve, milliseconds))
}

export function pairingCooldownDelayMs() {
  return 31_000
}

export function pairingDecisionSucceeded(control, browser, approved) {
  if (approved) {
    return control.code === 0 && /paired\n?$/.test(control.stdout) &&
      browser.ok === true
  }
  return control.code === 1 && /denied\n?$/.test(control.stdout) &&
    browser.ok === false && browser.code === 'SYNC_PAIRING_DENIED'
}

async function waitForPairingCooldown() {
  await sleep(pairingCooldownDelayMs())
}

function run(command, arguments_ = [], options = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, arguments_, {
      cwd: options.cwd,
      env: options.env ?? process.env,
      stdio: ['pipe', 'pipe', 'pipe'],
    })
    let stdout = ''
    let stderr = ''
    child.stdout.setEncoding('utf8')
    child.stderr.setEncoding('utf8')
    child.stdout.on('data', chunk => { stdout += chunk })
    child.stderr.on('data', chunk => { stderr += chunk })
    if (options.input !== undefined) child.stdin.end(options.input)
    else child.stdin.end()
    const timer = setTimeout(() => child.kill('SIGKILL'), options.timeoutMs ?? 60_000)
    child.once('error', error => { clearTimeout(timer); reject(error) })
    child.once('close', (code, signal) => {
      clearTimeout(timer)
      resolve({ code, signal, stdout, stderr })
    })
  })
}

function contentType(filename) {
  return ({
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.mjs': 'text/javascript; charset=utf-8',
    '.json': 'application/json',
    '.css': 'text/css',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
    '.svg': 'image/svg+xml',
    '.wasm': 'application/wasm',
  })[path.extname(filename)] ?? 'application/octet-stream'
}

async function serve(directory, port) {
  const root = path.resolve(directory)
  const server = createServer(async (request, response) => {
    try {
      const requestPath = decodeURIComponent(new URL(request.url, `http://127.0.0.1:${port}`).pathname)
      let filename = path.resolve(root, `.${requestPath}`)
      if (filename !== root && !filename.startsWith(`${root}${path.sep}`)) throw new Error('path escape')
      const metadata = await stat(filename)
      if (metadata.isDirectory()) filename = path.join(filename, 'index.html')
      const bytes = await readFile(filename)
      response.writeHead(200, {
        'Content-Type': contentType(filename),
        'Cache-Control': 'no-store',
      })
      response.end(bytes)
    } catch {
      response.writeHead(404)
      response.end('not found')
    }
  })
  await new Promise((resolve, reject) => {
    server.once('error', reject)
    server.listen(port, '127.0.0.1', resolve)
  })
  return server
}

async function waitForNoisedeck(page) {
  await page.waitForFunction(() => Boolean(
    window.__noisemakerUI &&
    window.__noisemakerCanvasRenderer?.pipeline &&
    window._testExports?.pipelineManager &&
    document.querySelector('canvas'),
  ), null, { timeout: 45_000 })
}

export function acceptanceAppSettings() {
  return { showStartupDialog: false, resolution: [320, 180] }
}

export function liveReceiverPlan() {
  return { frames: 24, timeoutMs: 60_000 }
}

async function preparePage(context, url) {
  const page = await context.newPage()
  await page.addInitScript(settings => {
    localStorage.setItem('noisedeck-app-settings', JSON.stringify(settings))
  }, acceptanceAppSettings())
  await page.route('**/deployment-meta.json', route => route.fulfill({
    status: 200,
    contentType: 'application/json',
    body: JSON.stringify({ git_hash: 'sync-linux-acceptance', date: 0 }),
  }))
  await page.goto(url, { waitUntil: 'domcontentloaded', timeout: 45_000 })
  await waitForNoisedeck(page)
  return page
}

async function setDsl(page, dsl) {
  const result = await page.evaluate(async source => {
    window.__noisemakerUI.setDsl(source)
    return window._testExports.pipelineManager.rebuildPipelineFromDsl({ silent: true })
  }, dsl)
  assert.equal(result.success, true,
    result.error?.message ?? result.message ?? JSON.stringify(result))
}

async function beginPairing(page) {
  await page.evaluate(() => {
    window.__syncPairResult = null
    import('/js/app.js').then(({ syncOutputController }) => syncOutputController.connect())
      .then(state => { window.__syncPairResult = { ok: true, status: state.status } })
      .catch(error => {
        window.__syncPairResult = { ok: false, code: error?.code, message: error?.message }
      })
  })
}

async function pairWithDecision(page, input) {
  const control = run(process.env.SYNC_CTL_PATH, ['pair'], {
    input, timeoutMs: 40_000,
  })
  await sleep(300)
  await beginPairing(page)
  return Promise.all([control, pairingResult(page)])
}

async function pairingResult(page, timeoutMs = 40_000) {
  await page.waitForFunction(() => window.__syncPairResult !== null, null, { timeout: timeoutMs })
  return page.evaluate(() => window.__syncPairResult)
}

async function syncctl(command, ...arguments_) {
  return run(process.env.SYNC_CTL_PATH, [command, ...arguments_], { timeoutMs: 45_000 })
}

async function status() {
  const result = await syncctl('status')
  assert.equal(result.code, 0, result.stderr)
  return JSON.parse(result.stdout.trim())
}

async function receiver(directory, frames, timeoutMs = 20_000) {
  await mkdir(directory, { recursive: true })
  const result = await run(process.env.SYNC_V4L2_RECEIVER_PATH, [
    '--device', process.env.SYNC_CAMERA_DEVICE,
    '--frames', String(frames),
    '--output', directory,
  ], { timeoutMs })
  assert.equal(result.code, 0, `${result.stderr}\n${result.stdout}`)
  return JSON.parse(result.stdout.trim())
}

async function screenshotHash(page, filename) {
  const bytes = await page.locator('canvas').first().screenshot({ path: filename })
  return `sha256-${crypto.createHash('sha256').update(bytes).digest('base64url')}`
}

async function processRssKiB() {
  const located = await run('/usr/bin/pgrep', ['-n', 'syncd'])
  if (located.code !== 0) return null
  const pid = located.stdout.trim()
  if (!/^[0-9]+$/.test(pid)) return null
  const text = await readFile(`/proc/${pid}/status`, 'utf8')
  const match = text.match(/^VmRSS:\s+([0-9]+)\s+kB$/m)
  return match ? Number(match[1]) : null
}

async function launchContext(chromium, profile, origin, chromiumArguments) {
  const context = await chromium.launchPersistentContext(profile, {
    executablePath: process.env.CHROMIUM_EXECUTABLE_PATH,
    headless: false,
    args: chromiumArguments,
    viewport: { width: 1280, height: 900 },
  })
  await context.grantPermissions(['camera'], { origin })
  try {
    await context.grantPermissions(['camera', 'local-network-access'], { origin })
  } catch {
    // Older Chromium does not name the loopback permission separately.
  }
  return context
}

export async function runAcceptance() {
  const environmentErrors = validateAcceptanceEnvironment(process.env)
  if (environmentErrors.length) throw new Error(environmentErrors.join('\n'))
  const chromiumArguments = ['--no-sandbox', '--disable-dev-shm-usage']
  assertRealChromiumArguments(chromiumArguments)

  const artifacts = path.resolve(process.env.SYNC_ACCEPTANCE_ARTIFACT_DIR)
  await mkdir(artifacts, { recursive: true, mode: 0o700 })
  const url = 'http://127.0.0.1:8002/'
  const origin = new URL(url).origin
  const server = await serve(process.env.NOISEDECK_APP_DIR, 8002)
  const profiles = await mkdtemp(path.join(os.tmpdir(), 'sync-linux-browser-'))
  const noisemakerRoot = path.dirname(process.env.NOISEDECK_APP_DIR)
  const playwrightRoot = process.env.PLAYWRIGHT_MODULE_DIR || noisemakerRoot
  const requireFromNoisedeck = createRequire(path.join(playwrightRoot, 'package.json'))
  const { chromium } = requireFromNoisedeck('@playwright/test')
  let contextA
  let contextB
  let pageA
  let pageB
  const evidence = {
    version: 1,
    platform: {},
    camera: {},
    chromium: { executable: process.env.CHROMIUM_EXECUTABLE_PATH, args: chromiumArguments,
      fakeDeviceFlags: [] },
    pairing: { denied: false, timedOut: false, approved: false, revoked: false },
    noisedeck: {},
    independentReceiver: {},
    daemon: {},
    lifecycle: {},
    soak: {},
    ndi: {},
    artifacts: {},
    vm: { deleted: false },
  }

  try {
    const osRelease = await readFile('/etc/os-release', 'utf8')
    const uname = await run('/usr/bin/uname', ['-rmo'])
    const chromiumVersion = await run(process.env.CHROMIUM_EXECUTABLE_PATH, ['--version'])
    evidence.platform = {
      os: osRelease.includes('VERSION_ID="24.04"') ? 'Ubuntu 24.04' : 'unsupported',
      arch: process.arch === 'x64' ? 'x86_64' : process.arch,
      kernel: uname.stdout.trim(),
    }
    evidence.chromium.version = chromiumVersion.stdout.trim()

    const idle = await receiver(path.join(artifacts, 'receiver-idle'), 12)
    assert.equal(idle.idleCardFrames, idle.frameCount, 'idle camera did not show Sync card')

    contextA = await launchContext(chromium, path.join(profiles, 'a'), origin, chromiumArguments)
    contextB = await launchContext(chromium, path.join(profiles, 'b'), origin, chromiumArguments)
    pageA = await preparePage(contextA, url)
    pageB = await preparePage(contextB, url)

    const [deniedCtl, deniedBrowser] = await pairWithDecision(pageA, 'n\n')
    evidence.pairing.denied = pairingDecisionSucceeded(
      deniedCtl, deniedBrowser, false)
    evidence.pairing.deniedAttempt = {
      controlCode: deniedCtl.code, controlOutput: deniedCtl.stdout,
      controlError: deniedCtl.stderr, browser: deniedBrowser,
    }
    assert.equal(evidence.pairing.denied, true)
    await waitForPairingCooldown()

    await beginPairing(pageA)
    const timeoutBrowser = await pairingResult(pageA, 40_000)
    evidence.pairing.timedOut = timeoutBrowser.ok === false &&
      timeoutBrowser.code === 'SYNC_TIMEOUT'
    evidence.pairing.timeoutAttempt = { browser: timeoutBrowser }
    assert.equal(evidence.pairing.timedOut, true)
    await waitForPairingCooldown()

    const [approvedCtl, approvedBrowser] = await pairWithDecision(pageA, 'y\n')
    evidence.pairing.approved = pairingDecisionSucceeded(
      approvedCtl, approvedBrowser, true)
    evidence.pairing.approvedAttempt = {
      controlCode: approvedCtl.code, controlOutput: approvedCtl.stdout,
      controlError: approvedCtl.stderr, browser: approvedBrowser,
    }
    assert.equal(evidence.pairing.approved, true, approvedCtl.stderr)

    await setDsl(pageA,
      'search synth\nnoise(speed: 40, wrap: true, type: 1, colorMode: 6).write(o0)\nrender(o0)')

    const before = await status()
    await pageA.evaluate(async () => {
      const { syncOutputController } = await import('/js/app.js')
      await syncOutputController.start('Noisedeck A')
    })
    await pageA.waitForFunction(() => import('/js/app.js').then(
      ({ syncOutputController }) => syncOutputController.state.status === 'sending'),
    null, { timeout: 20_000 })

    await setDsl(pageB,
      'search synth, filter\nmedia().invert().write(o0)\nrender(o0)')
    const selectedCamera = await pageB.evaluate(async () => {
      const stream = await navigator.mediaDevices.getUserMedia({ video: true })
      stream.getTracks().forEach(track => track.stop())
      const devices = await navigator.mediaDevices.enumerateDevices()
      const device = devices.find(item => item.kind === 'videoinput' && item.label === 'Sync Camera')
      if (!device) return null
      const manager = window.__noisemakerUI._mediaManager
      const stepIndex = [...manager.mediaInputs.keys()][0]
      if (stepIndex === undefined) return null
      await manager._startCamera(stepIndex, device.deviceId)
      return { deviceId: device.deviceId, stepIndex }
    })
    assert.ok(selectedCamera, 'Noisedeck B could not select Sync Camera by exact label')
    await pageB.waitForFunction(() => {
      const media = [...window.__noisemakerUI._mediaManager.mediaInputs.values()][0]
      return media?.videoEl?.videoWidth === 1920 && media?.videoEl?.videoHeight === 1080
    }, null, { timeout: 20_000 })

    const receiverPlan = liveReceiverPlan()
    const liveReceiverPromise = receiver(
      path.join(artifacts, 'receiver-live'), receiverPlan.frames,
      receiverPlan.timeoutMs).then(
      value => ({ value }),
      error => ({ error }),
    )
    const sourceHashes = []
    const outputHashes = []
    for (let index = 0; index < 3; ++index) {
      await sleep(1500)
      const sourceName = path.join(artifacts, `noisedeck-a-${index + 1}.png`)
      const outputName = path.join(artifacts, `noisedeck-b-${index + 1}.png`)
      sourceHashes.push(await screenshotHash(pageA, sourceName))
      outputHashes.push(await screenshotHash(pageB, outputName))
    }
    const liveReceiverResult = await liveReceiverPromise
    if (liveReceiverResult.error) throw liveReceiverResult.error
    const liveReceiver = liveReceiverResult.value
    const after = await status()
    const uniqueSource = new Set(sourceHashes).size
    const uniqueOutput = new Set(outputHashes).size
    evidence.noisedeck = {
      sourceChanging: uniqueSource > 1,
      outputChanging: uniqueOutput > 1,
      outputDiffersFromSource: outputHashes.every(hash => !sourceHashes.includes(hash)),
      outputIsNotIdleCard: liveReceiver.idleCardFrames < liveReceiver.frameCount,
      sourceHashes,
      outputHashes,
      effectDsl: 'media().invert()',
    }
    evidence.camera = {
      driver: 'v4l2 loopback', card: 'Sync Camera', format: liveReceiver.format,
      device: process.env.SYNC_CAMERA_DEVICE, width: liveReceiver.width,
      height: liveReceiver.height,
    }
    evidence.independentReceiver = {
      passed: liveReceiver.frameCount === receiverPlan.frames &&
        liveReceiver.uniqueChecksumCount > 1 &&
        liveReceiver.idleCardFrames < liveReceiver.frameCount,
      frameCount: liveReceiver.frameCount,
      uniqueChecksumCount: liveReceiver.uniqueChecksumCount,
      idleCardFrames: liveReceiver.idleCardFrames,
    }
    evidence.daemon = {
      before: before.metrics,
      after: after.metrics,
      cameraWritesAdvanced: after.metrics.cameraWrites > before.metrics.cameraWrites,
      lastWriteAdvanced: after.metrics.cameraLastWriteMs > before.metrics.cameraLastWriteMs,
    }
    evidence.ndi = after.providers.find(provider => provider.id === 'ndi') ?? { selected: false }

    await pageB.evaluate(async ({ stepIndex, deviceId }) => {
      const manager = window.__noisemakerUI._mediaManager
      manager._stopCamera(stepIndex)
      await manager._startCamera(stepIndex, deviceId)
    }, selectedCamera)
    await pageB.waitForFunction(() => {
      const media = [...window.__noisemakerUI._mediaManager.mediaInputs.values()][0]
      return media?.videoEl?.videoWidth === 1920
    }, null, { timeout: 20_000 })
    evidence.lifecycle.consumerReopen = true

    const soakSeconds = Number(process.env.SYNC_ACCEPTANCE_SOAK_SECONDS ?? 900)
    assert.ok(Number.isSafeInteger(soakSeconds) && soakSeconds >= 10 && soakSeconds <= 3600)
    const soakSamples = []
    const soakDeadline = Date.now() + soakSeconds * 1000
    while (Date.now() < soakDeadline) {
      const sampleStatus = await status()
      soakSamples.push({
        atMs: Date.now(),
        rssKiB: await processRssKiB(),
        cameraWrites: sampleStatus.metrics.cameraWrites,
        cameraLastWriteAgeMs: sampleStatus.metrics.cameraLastWriteAgeMs,
        cameraReopenAttempts: sampleStatus.metrics.cameraReopenAttempts,
        receivedFrames: sampleStatus.metrics.receivedFrames,
        acceptedFrames: sampleStatus.metrics.acceptedFrames,
        cameraDrivingFrames: sampleStatus.metrics.cameraDrivingFrames,
      })
      await sleep(Math.min(5000, Math.max(0, soakDeadline - Date.now())))
    }
    const rssValues = soakSamples.map(sample => sample.rssKiB).filter(Number.isFinite)
    const rssGrowthKiB = rssValues.length ? Math.max(...rssValues) - Math.min(...rssValues) : Infinity
    evidence.soak = {
      durationSeconds: soakSeconds,
      sampleCount: soakSamples.length,
      rssGrowthKiB,
      bounded: soakSamples.length >= 2 && rssGrowthKiB <= 64 * 1024,
      slotCount: after.metrics.cameraSlotCount,
      staleWhileLive: soakSamples.some(sample =>
        sample.cameraLastWriteAgeMs === null || sample.cameraLastWriteAgeMs > 1000),
      ...liveSoakProgress(soakSamples),
      samples: soakSamples,
    }

    await pageA.evaluate(async () => {
      const { syncOutputController } = await import('/js/app.js')
      await syncOutputController.stop()
    })
    const revoked = await syncctl('revoke', origin)
    evidence.pairing.revoked = revoked.code === 0 && /"status":"revoked"/.test(revoked.stdout)
    assert.equal(evidence.pairing.revoked, true, revoked.stderr)

    evidence.artifacts = {
      sourceScreenshots: [1, 2, 3].map(index => `noisedeck-a-${index}.png`),
      outputScreenshots: [1, 2, 3].map(index => `noisedeck-b-${index}.png`),
      receiverFrames: [
        'receiver-live/first.pgm', 'receiver-live/middle.pgm', 'receiver-live/last.pgm',
      ],
    }
    assertAcceptanceVerdict(evidence)
    await writeFile(path.join(artifacts, 'acceptance.json'),
      `${JSON.stringify(redactSecrets(evidence), null, 2)}\n`, { mode: 0o600 })
    return evidence
  } catch (error) {
    evidence.failure = { name: error.name, message: error.message, stack: error.stack }
    await writeFile(path.join(artifacts, 'acceptance.failed.json'),
      `${JSON.stringify(redactSecrets(evidence), null, 2)}\n`, { mode: 0o600 }).catch(() => {})
    throw error
  } finally {
    await contextA?.close().catch(() => {})
    await contextB?.close().catch(() => {})
    await new Promise(resolve => server.close(resolve))
    await rm(profiles, { recursive: true, force: true })
  }
}

const isMain = process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href
if (isMain) {
  runAcceptance().then(value => {
    process.stdout.write(`${JSON.stringify({ passed: true, artifacts: value.artifacts })}\n`)
  }).catch(error => {
    process.stderr.write(`${error.stack ?? error}\n`)
    process.exitCode = 1
  })
}
