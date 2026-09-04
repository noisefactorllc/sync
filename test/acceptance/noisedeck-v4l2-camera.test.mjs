import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'

import {
  acceptanceAppSettings,
  assertAcceptanceVerdict,
  assertRealChromiumArguments,
  pairingCooldownDelayMs,
  pairingDecisionSucceeded,
  liveReceiverPlan,
  liveSoakProgress,
  redactSecrets,
  validateAcceptanceEnvironment,
} from './noisedeck-v4l2-camera.mjs'

const acceptanceSource = readFileSync(
  new URL('./noisedeck-v4l2-camera.mjs', import.meta.url), 'utf8')

test('acceptance uses the public DSL without mutating private renderer uniforms', () => {
  assert.match(acceptanceSource, /noise\(speed: 40, wrap: true/)
  assert.match(acceptanceSource, /media\(\)\.invert\(\)/)
  assert.doesNotMatch(acceptanceSource, /mediaInput\(\)/)
  assert.doesNotMatch(acceptanceSource, /_globalUniforms/)
})

test('acceptance waits out the daemon cooldown between destructive pairing cases', () => {
  assert.equal(pairingCooldownDelayMs(), 31_000)
  assert.equal(
    acceptanceSource.match(/await waitForPairingCooldown\(\)/g)?.length,
    2,
  )
})

test('acceptance registers the terminal pair client before triggering the browser', () => {
  const helper = acceptanceSource.slice(
    acceptanceSource.indexOf('async function pairWithDecision'),
    acceptanceSource.indexOf('async function pairingResult'),
  )
  assert.match(helper, /const control = run\(/)
  assert.match(helper, /await sleep\(300\)[\s\S]*await beginPairing\(page\)/)
  assert.doesNotMatch(helper, /waitForFunction/)
})

test('acceptance does not start software-rendered noise before pairing completes', () => {
  const approval = acceptanceSource.indexOf(
    "await pairWithDecision(pageA, 'y\\n')",
  )
  const animatedNoise = acceptanceSource.indexOf(
    "'search synth\\nnoise(speed: 40",
  )
  assert.ok(approval >= 0 && animatedNoise > approval)
})

test('acceptance keeps Noisedeck rendering responsive on a CPU-only VM', () => {
  assert.deepEqual(acceptanceAppSettings(), {
    showStartupDialog: false,
    resolution: [320, 180],
  })
})

test('pairing decision evidence accepts the real inline syncctl prompt', () => {
  const browserDenied = { ok: false, code: 'SYNC_PAIRING_DENIED' }
  const browserApproved = { ok: true, status: 'ready' }
  assert.equal(pairingDecisionSucceeded({ code: 1,
    stdout: 'Allow this browser to pair? [y/N] denied\n' }, browserDenied, false), true)
  assert.equal(pairingDecisionSucceeded({ code: 0,
    stdout: 'Allow this browser to pair? [y/N] paired\n' }, browserApproved, true), true)
  assert.equal(pairingDecisionSucceeded({ code: 1,
    stdout: '' }, { ok: false, code: 'SYNC_TIMEOUT' }, false), false)
})

test('live receiver budget tolerates CPU-only Chromium while requiring many frames', () => {
  assert.deepEqual(liveReceiverPlan(), { frames: 24, timeoutMs: 60_000 })
})

test('live soak requires native receive, accept, and camera-drive progress', () => {
  assert.deepEqual(liveSoakProgress([
    { receivedFrames: 10, acceptedFrames: 9, cameraDrivingFrames: 8 },
    { receivedFrames: 13, acceptedFrames: 12, cameraDrivingFrames: 11 },
  ]), {
    receivedFrameGrowth: 3,
    acceptedFrameGrowth: 3,
    cameraDrivingFrameGrowth: 3,
    liveFramesAdvanced: true,
  })
  assert.equal(liveSoakProgress([
    { receivedFrames: 10, acceptedFrames: 9, cameraDrivingFrames: 8 },
    { receivedFrames: 10, acceptedFrames: 9, cameraDrivingFrames: 8 },
  ]).liveFramesAdvanced, false)
})

const validEnvironment = {
  SYNC_DAEMON_PATH: '/usr/bin/syncd',
  SYNC_CTL_PATH: '/usr/bin/syncctl',
  SYNC_CAMERA_DEVICE: '/dev/video12',
  SYNC_V4L2_RECEIVER_PATH: '/usr/bin/sync_v4l2_receiver_probe',
  NOISEDECK_APP_DIR: '/opt/noisedeck/app',
  SYNC_ACCEPTANCE_ARTIFACT_DIR: '/tmp/evidence',
  CHROMIUM_EXECUTABLE_PATH: '/usr/bin/chromium',
}

test('acceptance environment requires exact absolute inputs and device path', () => {
  assert.deepEqual(validateAcceptanceEnvironment(validEnvironment), [])
  assert.match(validateAcceptanceEnvironment({ ...validEnvironment,
    SYNC_CAMERA_DEVICE: '/dev/video1/../video2',
  }).join('\n'), /SYNC_CAMERA_DEVICE/)
  assert.match(validateAcceptanceEnvironment({ ...validEnvironment,
    SYNC_CTL_PATH: 'syncctl',
  }).join('\n'), /SYNC_CTL_PATH/)
})

test('acceptance refuses every Chromium fake-media switch', () => {
  assert.doesNotThrow(() => assertRealChromiumArguments(['--no-sandbox']))
  for (const flag of [
    '--use-fake-device-for-media-stream',
    '--use-file-for-fake-video-capture=/tmp/a.y4m',
    '--use-fake-ui-for-media-stream',
  ]) assert.throws(() => assertRealChromiumArguments([flag]), /fake/i)
})

test('acceptance verdict requires changing independent and effected pixels', () => {
  const passing = {
    camera: { driver: 'v4l2 loopback', card: 'Sync Camera', format: 'NV12' },
    chromium: { fakeDeviceFlags: [] },
    pairing: { denied: true, timedOut: true, approved: true, revoked: true },
    noisedeck: {
      sourceChanging: true,
      outputChanging: true,
      outputDiffersFromSource: true,
      outputIsNotIdleCard: true,
    },
    independentReceiver: { passed: true, frameCount: 90, uniqueChecksumCount: 3 },
    daemon: { cameraWritesAdvanced: true, lastWriteAdvanced: true },
    soak: { bounded: true, slotCount: 3, staleWhileLive: false,
      liveFramesAdvanced: true },
  }
  assert.doesNotThrow(() => assertAcceptanceVerdict(passing))
  for (const mutate of [
    value => { value.independentReceiver.frameCount = 0 },
    value => { value.independentReceiver.uniqueChecksumCount = 1 },
    value => { value.noisedeck.outputDiffersFromSource = false },
    value => { value.noisedeck.outputChanging = false },
    value => { value.soak.liveFramesAdvanced = false },
  ]) {
    const value = structuredClone(passing)
    mutate(value)
    assert.throws(() => assertAcceptanceVerdict(value))
  }
})

test('artifact redaction removes token-shaped values and keys', () => {
  const secret = 'a'.repeat(64)
  const screenshotHash = `sha256-${'A'.repeat(43)}`
  const redacted = redactSecrets({ token: secret, nested: `prefix-${secret}`,
    sourceHashes: [screenshotHash] })
  assert.equal(JSON.stringify(redacted).includes(secret), false)
  assert.deepEqual(redacted.token, '[redacted]')
  assert.deepEqual(redacted.sourceHashes, [screenshotHash])
})
