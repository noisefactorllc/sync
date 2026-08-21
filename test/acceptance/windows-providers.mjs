// Windows provider certification: does Sync actually publish video that
// something else can receive?
//
// This drives the real control protocol against a real syncd, with a real
// SpoutLibrary and a real NDI runtime, and publishes real frames. It proves
// only the publishing half. The receiving half is proved by two independent
// probes that discover what was published and inspect the pixels that
// arrived -- see native/test/windows/spout_receiver_probe.cpp and
// native/test/windows/ndi_receiver_probe.cpp.
//
// Not a CI test, and deliberately not registered with ctest: it needs a GPU,
// a Spout runtime, and an NDI runtime, none of which a hosted runner has.
//
// Getting the pieces:
//   syncd + SpoutLibrary.dll   the shipped installer is the best subject,
//                              because it certifies what users actually run.
//                              Extract the published Sync setup, or use a
//                              local build together with a SpoutLibrary built
//                              from the revision named in
//                              packaging/windows/Third-Party-Notices.txt.
//   NDI runtime                install the NDI Runtime, or point --ndi at any
//                              directory holding Processing.NDI.Lib.x64.dll.
//                              Sync does not redistribute it -- its licence
//                              forbids that, which is why the guide asks
//                              users to install it themselves.
//
// Running it: publish with a hold long enough to attach receivers, then run
// each probe against the same sender name.
//
//   node test/acceptance/windows-providers.mjs \
//     --syncd <syncd.exe> --spout <SpoutLibrary.dll> --ndi <runtime dir> \
//     --sender SyncCert --width 640 --height 360 --hold 40000 &
//   sync_spout_receiver_probe <SpoutLibrary.dll> SyncCert 640 360
//   sync_ndi_receiver_probe   <Processing.NDI.Lib.x64.dll> SyncCert 640 360
//
// Build the probes by configuring with SYNC_SPOUT_INCLUDE_DIR and
// SYNC_NDI_INCLUDE_DIR pointed at the respective headers.
//
// Both probes exit non-zero on a wrong size, wrong pixels, or an image that
// arrived vertically flipped -- which is the failure most worth catching,
// because a flipped frame still looks like a working integration from this
// side until somebody actually looks at the output.

import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import crypto from 'node:crypto'
import net from 'node:net'
import process from 'node:process'

function argument(name, fallback = undefined) {
  const index = process.argv.indexOf(`--${name}`)
  if (index < 0 || index + 1 >= process.argv.length) return fallback
  return process.argv[index + 1]
}

const SYNCD = argument('syncd')
const SPOUT = argument('spout')
const NDI = argument('ndi')
const WIDTH = Number(argument('width', '64'))
const HEIGHT = Number(argument('height', '64'))
const FRAMES = Number(argument('frames', '120'))
const HOLD_MS = Number(argument('hold', '0'))
const SENDER = argument('sender', 'SyncCert')
const PORT = Number(argument('port', '53996'))
const ORIGIN = 'https://cert.example'
const TOKEN = 'c'.repeat(64)
const HOST = '127.0.0.1'

if (!SYNCD) throw new Error('--syncd <path to syncd.exe> is required')

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

function connect() {
  return new Promise((resolve, reject) => {
    const socket = net.connect({ host: HOST, port: PORT })
    socket.once('connect', () => resolve(socket))
    socket.once('error', reject)
  })
}

// Written byte by byte rather than with a client library: this server is
// deliberately strict about what it will parse, and an Accept or User-Agent
// header it did not ask for is a different request from the one under test.
//
// /status carries no Origin on purpose. It refuses cross-origin requests --
// verified separately -- so sending one would certify the rejection path.
async function httpGet(route, origin = undefined) {
  const socket = await connect()
  const originHeader = origin === undefined ? '' : `Origin: ${origin}\r\n`
  socket.end(
    `GET ${route} HTTP/1.1\r\nHost: ${HOST}:${PORT}\r\n${originHeader}Connection: close\r\n\r\n`)
  const text = await new Promise((resolve, reject) => {
    const chunks = []
    const timer = setTimeout(() => {
      socket.destroy()
      reject(new Error(`timed out reading ${route}`))
    }, 8000)
    const finish = () => {
      clearTimeout(timer)
      resolve(Buffer.concat(chunks).toString('utf8'))
    }
    socket.on('data', (chunk) => chunks.push(chunk))
    socket.on('end', finish)
    socket.on('close', finish)
    socket.on('error', (error) => { clearTimeout(timer); reject(error) })
  })
  const separator = text.indexOf('\r\n\r\n')
  return {
    status: Number(text.slice(9, 12)),
    body: separator < 0 ? '' : text.slice(separator + 4),
  }
}

// --- minimal RFC6455 client, client-masked, no extensions ---------------

function maskedFrame(opcode, payload = Buffer.alloc(0)) {
  const mask = crypto.randomBytes(4)
  const masked = Buffer.from(payload)
  for (let i = 0; i < masked.length; i += 1) masked[i] ^= mask[i % 4]
  const header = [0x80 | opcode]
  if (masked.length < 126) {
    header.push(0x80 | masked.length)
  } else if (masked.length < 65536) {
    header.push(0x80 | 126, (masked.length >> 8) & 0xff, masked.length & 0xff)
  } else {
    header.push(0x80 | 127, 0, 0, 0, 0,
      (masked.length >>> 24) & 0xff, (masked.length >>> 16) & 0xff,
      (masked.length >>> 8) & 0xff, masked.length & 0xff)
  }
  return Buffer.concat([Buffer.from(header), mask, masked])
}

class WebSocketClient {
  constructor(socket, leftover) {
    this.socket = socket
    this.buffer = leftover ?? Buffer.alloc(0)
    this.frames = []
    this.waiters = []
    socket.on('data', (chunk) => {
      this.buffer = Buffer.concat([this.buffer, chunk])
      this.drain()
    })
    this.drain()
  }

  drain() {
    for (;;) {
      if (this.buffer.length < 2) return
      const opcode = this.buffer[0] & 0x0f
      let length = this.buffer[1] & 0x7f
      let offset = 2
      if (length === 126) {
        if (this.buffer.length < 4) return
        length = this.buffer.readUInt16BE(2)
        offset = 4
      } else if (length === 127) {
        if (this.buffer.length < 10) return
        length = Number(this.buffer.readBigUInt64BE(2))
        offset = 10
      }
      if (this.buffer.length < offset + length) return
      const frame = {
        opcode,
        payload: Buffer.from(this.buffer.subarray(offset, offset + length)),
      }
      this.buffer = this.buffer.subarray(offset + length)
      const waiter = this.waiters.shift()
      if (waiter) waiter(frame)
      else this.frames.push(frame)
    }
  }

  send(opcode, payload) { this.socket.write(maskedFrame(opcode, payload)) }
  sendJson(value) { this.send(0x1, Buffer.from(JSON.stringify(value), 'utf8')) }
  sendBinary(payload) { this.send(0x2, payload) }

  nextFrame(what, timeoutMs = 10_000) {
    if (this.frames.length) return Promise.resolve(this.frames.shift())
    return new Promise((resolve, reject) => {
      const timer = setTimeout(
        () => reject(new Error(`timed out waiting for ${what}`)), timeoutMs)
      this.waiters.push((frame) => { clearTimeout(timer); resolve(frame) })
    })
  }

  async nextJson(what) {
    const frame = await this.nextFrame(what)
    assert.equal(frame.opcode, 0x1, `${what}: expected a text frame`)
    return JSON.parse(frame.payload.toString('utf8'))
  }

  close() { try { this.socket.destroy() } catch { /* already gone */ } }
}

function upgrade(route, subprotocol) {
  return new Promise((resolve, reject) => {
    const socket = net.connect({ host: HOST, port: PORT })
    const timer = setTimeout(() => {
      socket.destroy()
      reject(new Error(`timed out upgrading ${route}`))
    }, 10_000)
    let buffer = Buffer.alloc(0)
    const onData = (chunk) => {
      buffer = Buffer.concat([buffer, chunk])
      const end = buffer.indexOf('\r\n\r\n')
      if (end < 0) return
      clearTimeout(timer)
      socket.off('data', onData)
      const head = buffer.subarray(0, end).toString('utf8')
      const status = Number(head.slice(9, 12))
      if (status !== 101) {
        socket.destroy()
        reject(new Error(`upgrade ${route} -> ${status}\n${head}`))
        return
      }
      resolve(new WebSocketClient(socket, buffer.subarray(end + 4)))
    }
    socket.on('error', (error) => { clearTimeout(timer); reject(error) })
    socket.on('connect', () => {
      const key = crypto.randomBytes(16).toString('base64')
      const lines = [
        `GET ${route} HTTP/1.1`,
        `Host: ${HOST}:${PORT}`,
        'Upgrade: websocket',
        'Connection: Upgrade',
        `Sec-WebSocket-Key: ${key}`,
        'Sec-WebSocket-Version: 13',
        `Origin: ${ORIGIN}`,
      ]
      if (subprotocol) lines.push(`Sec-WebSocket-Protocol: ${subprotocol}`)
      socket.write(`${lines.join('\r\n')}\r\n\r\n`)
    })
    socket.on('data', onData)
  })
}

// --- the wire frame -------------------------------------------------------

// Layout from native/src/protocol.cpp decode_frame(): a 64-byte little-endian
// header then tightly packed RGBA. Every reserved byte must be zero and the
// top-down flag is mandatory, so this is written out field by field rather
// than approximated.
const MAGIC = 0x434e5953
const HEADER_BYTES = 64

function wireFrame(width, height, sequence, pixels) {
  const header = Buffer.alloc(HEADER_BYTES)
  header.writeUInt32LE(MAGIC, 0)
  header.writeUInt16LE(1, 4)              // version
  header.writeUInt16LE(HEADER_BYTES, 6)   // header size
  header.writeUInt32LE(1, 8)              // flags: top-down, and only that
  header.writeUInt16LE(1, 12)             // pixel format: RGBA8
  header.writeUInt16LE(1, 14)             // colour space
  header.writeUInt16LE(1, 16)             // alpha mode
  header.writeUInt16LE(0, 18)             // reserved
  header.writeUInt32LE(width, 20)
  header.writeUInt32LE(height, 24)
  header.writeUInt32LE(width * 4, 28)     // row stride
  header.writeUInt32LE(width * 4 * height, 32)
  header.writeBigUInt64LE(BigInt(sequence), 36)
  header.writeBigUInt64LE(BigInt(Date.now() * 1000), 44)
  return Buffer.concat([header, pixels])
}

// The pattern both receivers look for.
//
// A single marker pixel was the first attempt and it does not survive NDI:
// the transport subsamples chroma and compresses, so one bright dot arrives
// smeared into its neighbours. Halves survive that and still prove
// orientation -- red on top, blue on bottom, so a vertically flipped image
// is unmistakable rather than subtle.
//
// The low-order bits of green carry a per-frame tick, which Spout (an
// uncompressed shared texture) reproduces exactly and NDI does not; only the
// Spout probe checks it.
function certificationPixels(width, height, tick) {
  const pixels = Buffer.alloc(width * height * 4)
  for (let y = 0; y < height; y += 1) {
    const top = y < height / 2
    for (let x = 0; x < width; x += 1) {
      const i = (y * width + x) * 4
      pixels[i + 0] = top ? 0xe0 : 0x10
      pixels[i + 1] = tick & 0xff
      pixels[i + 2] = top ? 0x10 : 0xe0
      pixels[i + 3] = 0xff
    }
  }
  return pixels
}

async function main() {
  const args = ['--port', String(PORT), '--test-origin', ORIGIN, '--test-token', TOKEN]
  if (SPOUT) args.push('--publisher', 'spout', '--spout-library', SPOUT)
  if (NDI) args.push('--publisher', 'ndi', '--ndi-runtime', NDI)

  const daemon = spawn(SYNCD, args, { stdio: ['ignore', 'pipe', 'pipe'] })
  let stdout = ''
  let stderr = ''
  daemon.stdout.on('data', (d) => { stdout += d })
  daemon.stderr.on('data', (d) => { stderr += d })
  const stop = () => { try { daemon.kill() } catch { /* gone */ } }
  process.on('exit', stop)

  for (let i = 0; i < 100 && !stdout.includes('"type":"ready"'); i += 1) {
    if (daemon.exitCode !== null) throw new Error(`syncd exited ${daemon.exitCode}\n${stderr}`)
    await sleep(100)
  }
  if (!stdout.includes('"type":"ready"')) throw new Error(`syncd never became ready\n${stderr}`)
  console.log(`syncd ready on ${PORT}`)

  const status = await httpGet('/status')
  if (status.status !== 200) throw new Error(`/status -> ${status.status} ${status.body}`)
  const health = JSON.parse(status.body)
  console.log(`version ${health.version}`)
  console.log('providers:')
  for (const provider of health.capabilities.providers) {
    console.log(`  ${provider.id.padEnd(6)} selected=${provider.selected} available=${provider.available}`)
  }
  const unavailable = health.capabilities.providers.filter((p) => !p.available)
  if (unavailable.length) {
    throw new Error(
      `these providers did not initialise: ${unavailable.map((p) => p.id).join(', ')}`)
  }

  const control = await upgrade('/control')
  control.sendJson({ type: 'hello', token: TOKEN, protocolVersions: [1] })
  const hello = await control.nextJson('hello response')
  assert.equal(hello.type, 'welcome', `unexpected hello reply: ${JSON.stringify(hello)}`)

  control.sendJson({ type: 'createSender', name: SENDER })
  const created = await control.nextJson('sender creation')
  assert.equal(created.type, 'senderCreated', `unexpected: ${JSON.stringify(created)}`)
  console.log(`\nsender "${SENDER}" created`)

  const data = await upgrade(created.path, `sync.sender.${created.ticket}`)
  console.log(`publishing ${WIDTH}x${HEIGHT} frames...`)

  let sent = 0
  const publish = async (count) => {
    for (let i = 0; i < count; i += 1) {
      data.sendBinary(wireFrame(WIDTH, HEIGHT, sent, certificationPixels(WIDTH, HEIGHT, sent)))
      sent += 1
      await sleep(16)
    }
  }
  await publish(FRAMES)
  console.log(`sent ${sent} frames`)

  // The daemon reports a decode or publish failure over the control channel.
  // Silence here is the point: any error frame means a provider rejected what
  // we sent, and the run should not be called a pass.
  if (control.frames.length) {
    const problems = control.frames.map((f) => f.payload.toString('utf8'))
    throw new Error(`daemon reported: ${problems.join(' | ')}`)
  }

  const during = JSON.parse((await httpGet('/status')).body)
  console.log(`activeSenders while publishing: ${during.activeSenders}`)
  assert.equal(during.activeSenders, 1, 'the sender should be active')

  if (HOLD_MS > 0) {
    console.log(`\nholding "${SENDER}" open for ${HOLD_MS}ms so a receiver can attach...`)
    const until = Date.now() + HOLD_MS
    while (Date.now() < until) await publish(30)
    console.log(`sent ${sent} frames total`)
    if (control.frames.length) {
      const problems = control.frames.map((f) => f.payload.toString('utf8'))
      throw new Error(`daemon reported: ${problems.join(' | ')}`)
    }
  }

  data.close()
  control.close()
  await sleep(200)
  stop()
  console.log('\nPUBLISHED OK')
}

main().catch((error) => {
  console.error(`\nCERTIFICATION FAILED: ${error.message}`)
  process.exitCode = 1
})
