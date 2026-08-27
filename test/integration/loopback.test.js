import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createHash, randomBytes } from "node:crypto";
import { mkdtemp, readFile, realpath, rm, writeFile } from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import {
  SyncAuthenticationError,
  SyncBridgeClient,
  SyncPairingDeniedError,
} from "../../browser/client.js";

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const DEFAULT_SYNCD = path.join(ROOT, "build", "syncd");
function resolveDaemonPath(value = process.env.SYNC_DAEMON_PATH) {
  if (value === undefined || value.length === 0) return DEFAULT_SYNCD;
  return path.isAbsolute(value) ? value : path.resolve(ROOT, value);
}
const SYNCD = resolveDaemonPath();
const PAIRING_TEST_SERVER = path.join(path.dirname(SYNCD), "sync_pairing_test_server");
const SYPHON_PROBE = path.join(path.dirname(SYNCD), "sync_syphon_discovery_probe");
const FIXTURE = path.join(ROOT, "test", "fixtures", "frame-v1.bin");
const ORIGIN = "https://client.example";
const TOKEN = "test-token-123";
const TIMEOUT_MS = 3_000;
const EXPECTED_PRODUCT_VERSION = process.env.SYNC_VERSION ?? "0.2.0";
// The providers a daemon configures when no --publisher is named. NDI is on
// both platforms because its send path takes a CPU frame and needs no
// platform-specific GPU layer; Syphon and Spout are each platform-bound.
const DEFAULT_PROVIDER_IDS = process.platform === "win32"
  ? ["spout", "ndi"]
  : ["syphon", "ndi"];

function withTimeout(promise, description, timeoutMs = TIMEOUT_MS) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`Timed out waiting for ${description}`)), timeoutMs);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

function onceEvent(emitter, successEvent, errorEvents = ["error"]) {
  return new Promise((resolve, reject) => {
    const cleanups = [];
    const cleanup = () => {
      for (const [event, listener] of cleanups) emitter.off(event, listener);
    };
    const success = (...args) => {
      cleanup();
      resolve(args);
    };
    emitter.once(successEvent, success);
    cleanups.push([successEvent, success]);
    for (const event of errorEvents) {
      const failure = (error) => {
        cleanup();
        reject(error instanceof Error ? error : new Error(String(error)));
      };
      emitter.once(event, failure);
      cleanups.push([event, failure]);
    }
  });
}

async function connect(host, port) {
  const socket = net.createConnection({ host, port });
  try {
    await withTimeout(onceEvent(socket, "connect"), `TCP connection to ${host}`);
    return socket;
  } catch (error) {
    socket.destroy();
    throw error;
  }
}

async function readHttpResponse(socket) {
  let bytes = Buffer.alloc(0);
  for await (const chunk of socket) {
    bytes = Buffer.concat([bytes, chunk]);
    if (bytes.length > 65_536) throw new Error("HTTP response exceeded test bound");
  }
  const marker = bytes.indexOf("\r\n\r\n");
  assert.notEqual(marker, -1, "HTTP response terminates its headers");
  const headerText = bytes.subarray(0, marker).toString("ascii");
  const body = bytes.subarray(marker + 4);
  const lines = headerText.split("\r\n");
  const status = Number(lines[0].split(" ")[1]);
  const headers = new Map();
  for (const line of lines.slice(1)) {
    const colon = line.indexOf(":");
    assert.ok(colon > 0, `valid response header: ${line}`);
    headers.set(line.slice(0, colon).toLowerCase(), line.slice(colon + 1).trim());
  }
  return { status, headers, body };
}

async function health(host, port, origin) {
  const socket = await connect(host, port);
  try {
    const hostHeader = host.includes(":") ? `[${host}]:${port}` : `${host}:${port}`;
    const originHeader = origin === undefined ? "" : `Origin: ${origin}\r\n`;
    socket.end(`GET /health HTTP/1.1\r\nHost: ${hostHeader}\r\n${originHeader}Connection: close\r\n\r\n`);
    return await withTimeout(readHttpResponse(socket), `health response from ${host}`);
  } catch (error) {
    socket.destroy();
    throw error;
  }
}

async function status(host, port, origin) {
  const socket = await connect(host, port);
  try {
    const hostHeader = host.includes(":") ? `[${host}]:${port}` : `${host}:${port}`;
    const originHeader = origin === undefined ? "" : `Origin: ${origin}\r\n`;
    socket.end(`GET /status HTTP/1.1\r\nHost: ${hostHeader}\r\n${originHeader}Connection: close\r\n\r\n`);
    return await withTimeout(readHttpResponse(socket), `status response from ${host}`);
  } catch (error) {
    socket.destroy();
    throw error;
  }
}

async function preflight(host, port, origin, requestPrivateNetwork = false) {
  const socket = await connect(host, port);
  try {
    const hostHeader = host.includes(":") ? `[${host}]:${port}` : `${host}:${port}`;
    const privateNetworkHeader = requestPrivateNetwork
      ? "Access-Control-Request-Private-Network: true\r\n"
      : "";
    socket.end(
      `OPTIONS /health HTTP/1.1\r\nHost: ${hostHeader}\r\n` +
      `Origin: ${origin}\r\nAccess-Control-Request-Method: GET\r\n` +
      privateNetworkHeader +
      "Connection: close\r\n\r\n",
    );
    return await withTimeout(readHttpResponse(socket), `health preflight from ${host}`);
  } catch (error) {
    socket.destroy();
    throw error;
  }
}

function parseHeaderBlock(headerBytes) {
  const lines = headerBytes.toString("ascii").split("\r\n");
  const status = Number(lines[0].split(" ")[1]);
  const headers = new Map();
  for (const line of lines.slice(1)) {
    const colon = line.indexOf(":");
    assert.ok(colon > 0, `valid upgrade response header: ${line}`);
    headers.set(line.slice(0, colon).toLowerCase(), line.slice(colon + 1).trim());
  }
  return { status, headers };
}

async function upgrade({
  host = "127.0.0.1",
  port,
  route,
  origin,
  subprotocol,
  socket: suppliedSocket,
  pipelined,
}) {
  const socket = suppliedSocket ?? await connect(host, port);
  try {
    const key = randomBytes(16).toString("base64");
    const hostHeader = host.includes(":") ? `[${host}]:${port}` : `${host}:${port}`;
    const protocolHeader = subprotocol === undefined
      ? ""
      : `Sec-WebSocket-Protocol: ${subprotocol}\r\n`;
    const head = Buffer.from(
      `GET ${route} HTTP/1.1\r\n` +
      `Host: ${hostHeader}\r\n` +
      "Upgrade: websocket\r\n" +
      "Connection: Upgrade\r\n" +
      `Origin: ${origin}\r\n` +
      "Sec-WebSocket-Version: 13\r\n" +
      `Sec-WebSocket-Key: ${key}\r\n` +
      protocolHeader +
      "\r\n",
    );
    // `pipelined` lands in the same write as the handshake so the daemon reads
    // request head and WebSocket payload together.
    socket.write(pipelined === undefined ? head : Buffer.concat([head, pipelined]));

    const chunks = [];
    let total = 0;
    let marker = -1;
    while (marker < 0) {
      const [chunk] = await withTimeout(
        onceEvent(socket, "data", ["error", "close"]), "upgrade response",
      );
      chunks.push(chunk);
      total += chunk.length;
      assert.ok(total <= 65_536, "upgrade response stays bounded");
      marker = Buffer.concat(chunks, total).indexOf("\r\n\r\n");
    }
    const bytes = Buffer.concat(chunks, total);
    const parsed = parseHeaderBlock(bytes.subarray(0, marker));
    const remainder = bytes.subarray(marker + 4);
    if (parsed.status === 101) {
      const expectedAccept = createHash("sha1")
        .update(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
        .digest("base64");
      assert.equal(parsed.headers.get("sec-websocket-accept"), expectedAccept);
      return { ...parsed, socket, client: new WebSocketClient(socket, remainder) };
    }
    socket.destroy();
    return { ...parsed, socket, client: null };
  } catch (error) {
    socket.destroy();
    throw error;
  }
}

function maskedClientFrame(opcode, payload = Buffer.alloc(0)) {
  payload = Buffer.from(payload);
  const mask = randomBytes(4);
  let header;
  if (payload.length <= 125) {
    header = Buffer.from([0x80 | opcode, 0x80 | payload.length]);
  } else if (payload.length <= 0xffff) {
    header = Buffer.alloc(4);
    header[0] = 0x80 | opcode;
    header[1] = 0x80 | 126;
    header.writeUInt16BE(payload.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | opcode;
    header[1] = 0x80 | 127;
    header.writeBigUInt64BE(BigInt(payload.length), 2);
  }
  const masked = Buffer.alloc(payload.length);
  for (let index = 0; index < payload.length; index += 1) {
    masked[index] = payload[index] ^ mask[index % mask.length];
  }
  return Buffer.concat([header, mask, masked]);
}

function maskedClientFrameHeader(opcode, payloadBytes, final = true) {
  const first = Buffer.from([(final ? 0x80 : 0) | opcode]);
  let length;
  if (payloadBytes <= 125) {
    length = Buffer.from([0x80 | payloadBytes]);
  } else if (payloadBytes <= 65_535) {
    length = Buffer.alloc(3);
    length[0] = 0xfe;
    length.writeUInt16BE(payloadBytes, 1);
  } else {
    length = Buffer.alloc(9);
    length[0] = 0xff;
    length.writeBigUInt64BE(BigInt(payloadBytes), 1);
  }
  return Buffer.concat([
    first,
    length,
    Buffer.from([0x11, 0x22, 0x33, 0x44]),
  ]);
}

class WebSocketClient {
  constructor(socket, initialBytes) {
    this.socket = socket;
    this.buffer = Buffer.from(initialBytes);
    this.frames = [];
    this.waiters = [];
    this.closed = socket.destroyed;
    socket.on("data", (chunk) => {
      this.buffer = Buffer.concat([this.buffer, chunk]);
      this.#decode();
    });
    socket.on("close", () => {
      this.closed = true;
      for (const waiter of this.waiters.splice(0)) waiter.reject(new Error("WebSocket closed"));
    });
    this.#decode();
  }

  #decode() {
    while (this.buffer.length >= 2) {
      const first = this.buffer[0];
      const second = this.buffer[1];
      assert.equal(first & 0x70, 0, "server frame has no reserved bits");
      assert.equal(second & 0x80, 0, "server frame is unmasked");
      let length = second & 0x7f;
      let headerBytes = 2;
      if (length === 126) {
        if (this.buffer.length < 4) return;
        length = this.buffer.readUInt16BE(2);
        headerBytes = 4;
      } else if (length === 127) {
        if (this.buffer.length < 10) return;
        const wide = this.buffer.readBigUInt64BE(2);
        assert.ok(wide <= BigInt(Number.MAX_SAFE_INTEGER));
        length = Number(wide);
        headerBytes = 10;
      }
      if (this.buffer.length < headerBytes + length) return;
      const frame = {
        final: (first & 0x80) !== 0,
        opcode: first & 0x0f,
        payload: Buffer.from(this.buffer.subarray(headerBytes, headerBytes + length)),
      };
      this.buffer = this.buffer.subarray(headerBytes + length);
      const waiter = this.waiters.shift();
      if (waiter) waiter.resolve(frame);
      else this.frames.push(frame);
    }
  }

  send(opcode, payload) {
    assert.equal(this.closed, false, "cannot send after close");
    this.socket.write(maskedClientFrame(opcode, payload));
  }

  sendJson(value) {
    this.send(0x1, Buffer.from(JSON.stringify(value), "utf8"));
  }

  async nextFrame(description = "WebSocket frame", timeoutMs = TIMEOUT_MS) {
    if (this.frames.length > 0) return this.frames.shift();
    const promise = new Promise((resolve, reject) => this.waiters.push({ resolve, reject }));
    return withTimeout(promise, description, timeoutMs);
  }

  async nextJson(description = "control response") {
    const frame = await this.nextFrame(description);
    assert.equal(frame.final, true);
    assert.equal(frame.opcode, 0x1);
    return JSON.parse(frame.payload.toString("utf8"));
  }

  async waitClosed() {
    if (this.closed) return;
    await withTimeout(onceEvent(this.socket, "close", ["error"]), "WebSocket close");
  }

  destroy() {
    this.socket.destroy();
  }
}

function webSocketForOrigin(origin) {
  return class OriginWebSocket {
    static CONNECTING = 0;
    static OPEN = 1;
    static CLOSING = 2;
    static CLOSED = 3;

    constructor(url, protocols) {
      this.url = url;
      this.protocol = "";
      this.readyState = OriginWebSocket.CONNECTING;
      this.bufferedAmount = 0;
      this._protocols = protocols;
      this._listeners = new Map();
      this._client = null;
      this._closeRequested = false;
      void this._connect();
    }

    addEventListener(type, listener) {
      const listeners = this._listeners.get(type) ?? new Set();
      listeners.add(listener);
      this._listeners.set(type, listeners);
    }

    removeEventListener(type, listener) {
      this._listeners.get(type)?.delete(listener);
    }

    send(value) {
      if (this.readyState !== OriginWebSocket.OPEN || !this._client) {
        throw new Error("WebSocket is not open");
      }
      if (typeof value !== "string") throw new TypeError("test WebSocket accepts text only");
      this._client.send(0x1, Buffer.from(value, "utf8"));
    }

    close() {
      if (this.readyState === OriginWebSocket.CLOSED || this._closeRequested) return;
      this._closeRequested = true;
      this.readyState = OriginWebSocket.CLOSING;
      if (this._client) {
        try { this._client.send(0x8, Buffer.alloc(0)); } catch {}
        this._client.destroy();
        this._finishClose();
      }
    }

    _emit(type, event = {}) {
      for (const listener of [...(this._listeners.get(type) ?? [])]) {
        listener.call(this, event);
      }
    }

    _finishClose() {
      if (this.readyState === OriginWebSocket.CLOSED) return;
      this.readyState = OriginWebSocket.CLOSED;
      this._emit("close", { code: 1000, reason: "" });
    }

    async _connect() {
      try {
        const url = new URL(this.url);
        assert.equal(url.protocol, "ws:");
        const host = url.hostname.startsWith("[")
          ? url.hostname.slice(1, -1)
          : url.hostname;
        const protocols = Array.isArray(this._protocols) ? this._protocols : [this._protocols];
        const subprotocol = protocols.find((value) => value !== undefined);
        const upgraded = await upgrade({
          host,
          port: Number(url.port),
          route: `${url.pathname}${url.search}`,
          origin,
          subprotocol,
        });
        if (this._closeRequested) {
          upgraded.client?.destroy();
          this._finishClose();
          return;
        }
        if (upgraded.status !== 101 || !upgraded.client) {
          throw new Error(`WebSocket upgrade failed with HTTP ${upgraded.status}`);
        }
        this._client = upgraded.client;
        this.protocol = upgraded.headers.get("sec-websocket-protocol") ?? "";
        this.readyState = OriginWebSocket.OPEN;
        this._emit("open");
        void this._pump();
      } catch (error) {
        if (!this._closeRequested) this._emit("error", { error });
        this._finishClose();
      }
    }

    async _pump() {
      try {
        while (this.readyState === OriginWebSocket.OPEN) {
          const frame = await this._client.nextFrame("SDK WebSocket frame");
          if (frame.opcode === 0x1) {
            this._emit("message", { data: frame.payload.toString("utf8") });
          } else if (frame.opcode === 0x2) {
            this._emit("message", {
              data: frame.payload.buffer.slice(
                frame.payload.byteOffset,
                frame.payload.byteOffset + frame.payload.byteLength,
              ),
            });
          } else if (frame.opcode === 0x8) {
            this.readyState = OriginWebSocket.CLOSING;
            try { this._client.send(0x8, frame.payload); } catch {}
            await this._client.waitClosed().catch(() => {});
            this._finishClose();
            return;
          } else if (frame.opcode === 0x9) {
            this._client.send(0xa, frame.payload);
          }
        }
      } catch (error) {
        if (!this._closeRequested && this.readyState === OriginWebSocket.OPEN &&
            !this._client.closed) {
          this._emit("error", { error });
        }
        this._finishClose();
      }
    }
  };
}

async function spawnDaemon({
  executable = SYNCD,
  arguments: daemonArguments = [
    "--port", "0",
    "--test-origin", ORIGIN,
    "--test-token", TOKEN,
    "--test-receiver",
  ],
  env = process.env,
} = {}) {
  const child = spawn(executable, daemonArguments,
                      { cwd: ROOT, env, stdio: ["ignore", "pipe", "pipe"] });
  let stdout = "";
  let stderr = "";
  child.stdout.setEncoding("utf8");
  child.stderr.setEncoding("utf8");
  child.stdout.on("data", (chunk) => { stdout += chunk; });
  child.stderr.on("data", (chunk) => { stderr = (stderr + chunk).slice(-16_384); });

  let removeReadyListeners = () => {};
  try {
    const readyPromise = new Promise((resolve, reject) => {
      const inspect = () => {
        const newline = stdout.indexOf("\n");
        if (newline < 0) return;
        try {
          assert.equal(stdout.slice(newline + 1), "", "syncd emits only one startup stdout line");
          resolve(JSON.parse(stdout.slice(0, newline)));
        } catch (error) {
          reject(error);
        }
      };
      const failed = (error) => reject(error);
      const exited = (code, signal) => {
        reject(new Error(`syncd exited before ready: code=${code} signal=${signal} stderr=${stderr}`));
      };
      removeReadyListeners = () => {
        child.stdout.off("data", inspect);
        child.off("error", failed);
        child.off("exit", exited);
      };
      child.stdout.on("data", inspect);
      child.once("error", failed);
      child.once("exit", exited);
    });
    const ready = await withTimeout(readyPromise, "syncd ready record");
    removeReadyListeners();
    return { child, ready, stderr: () => stderr, stdout: () => stdout };
  } catch (error) {
    removeReadyListeners();
    await terminateChild(child);
    throw error;
  }
}

async function spawnPairingDaemon(storePath, mode) {
  return spawnDaemon({
    executable: PAIRING_TEST_SERVER,
    arguments: [storePath, mode],
  });
}

async function terminateChild(child, cleanupTimeoutMs = 1_000) {
  if (child.exitCode !== null || child.signalCode !== null || child.pid === undefined) return;
  let exitPromise = onceEvent(child, "exit", ["error"]);
  child.kill("SIGTERM");
  try {
    await withTimeout(exitPromise, "failed child cleanup", cleanupTimeoutMs);
  } catch {
    if (child.exitCode !== null || child.signalCode !== null) return;
    exitPromise = onceEvent(child, "exit", ["error"]);
    child.kill("SIGKILL");
    await withTimeout(exitPromise, "forced child cleanup", cleanupTimeoutMs);
  }
}

async function pairWithDaemon(ready, origin, name = "Noisedeck") {
  const upgraded = await upgrade({ port: ready.port, route: "/pair", origin });
  assert.equal(upgraded.status, 101);
  upgraded.client.sendJson({ type: "pair", protocolVersions: [1], name });
  const response = await upgraded.client.nextJson("pairing response");
  const close = await upgraded.client.nextFrame("pairing close");
  assert.equal(close.opcode, 0x8);
  upgraded.client.send(0x8, close.payload);
  await upgraded.client.waitClosed();
  return response;
}

async function authenticateControl(ready, origin, token) {
  const upgraded = await upgrade({ port: ready.port, route: "/control", origin });
  assert.equal(upgraded.status, 101);
  upgraded.client.sendJson({ type: "hello", token, protocolVersions: [1] });
  return upgraded;
}

async function createTestSender(control, port, name) {
  control.sendJson({ type: "createSender", name });
  const created = await control.nextJson(`${name} creation`);
  assert.equal(created.type, "senderCreated");
  const upgraded = await upgrade({
    port,
    route: created.path,
    origin: ORIGIN,
    subprotocol: `sync.sender.${created.ticket}`,
  });
  assert.equal(upgraded.status, 101);
  return { created, data: upgraded.client };
}

// Windows has no deliverable SIGTERM. Node's child.kill() there is always
// TerminateProcess regardless of the signal name passed, and the one event
// that does reach a libuv loop -- CTRL_BREAK_EVENT -- can only be sent by a
// process sharing the target's console, which Node cannot do. So on Windows
// these tests stop the daemon by terminating it and assert only that it
// stops; they cannot assert it shut down gracefully.
//
// Graceful shutdown IS covered on Windows, by scripts/smoke-windows-app.ps1:
// the tray app spawns its helper into a new process group and stops it with
// CTRL_BREAK_EVENT, which the daemon receives as SIGBREAK. That is the real
// shipped path, exercised against the real binaries.
const GRACEFUL_SHUTDOWN_IS_OBSERVABLE = process.platform !== "win32";

// The default pairing store lives under $HOME on POSIX and %LOCALAPPDATA%
// on Windows, so a test that wants an isolated store has to redirect
// whichever one this platform actually reads. Setting only HOME would
// silently let a Windows run touch the real user store.
function isolatedStoreEnvironment(directory) {
  return process.platform === "win32"
    ? { ...process.env, LOCALAPPDATA: directory }
    : { ...process.env, HOME: directory };
}

// `expectedStderr` stays "" for every ordinary run, so unexpected noise is
// still a failure everywhere. A caller that selects a provider it knows is
// unavailable passes the diagnostic it requires instead: the daemon is
// obliged to say why it degraded, and silence there is the bug this option
// exists to catch.
async function stopDaemon(child, stderr, stdout, ready, options) {
  const {
    shutdownTimeoutMs = TIMEOUT_MS,
    cleanupTimeoutMs = 1_000,
    expectedStderr = "",
  } = options ?? {};
  const assertStderr = () => {
    if (expectedStderr instanceof RegExp) {
      assert.match(stderr(), expectedStderr, "syncd explains the diagnostic it emitted");
      return;
    }
    assert.equal(stderr(), expectedStderr, "syncd emits no diagnostics during a clean run");
  };
  if (child.exitCode !== null || child.signalCode !== null) {
    if (GRACEFUL_SHUTDOWN_IS_OBSERVABLE) {
      assert.equal(child.signalCode, null, `syncd handles shutdown itself; stderr=${stderr()}`);
      assert.equal(child.exitCode, 0, `syncd exited cleanly; stderr=${stderr()}`);
    }
    assert.equal(stdout(), `${JSON.stringify(ready)}\n`, "syncd emits exactly one stdout record");
    assertStderr();
    return;
  }
  const exitPromise = onceEvent(child, "exit", ["error"]);
  child.kill("SIGTERM");
  let code;
  let signal;
  try {
    [code, signal] = await withTimeout(exitPromise, "syncd shutdown", shutdownTimeoutMs);
  } catch (shutdownError) {
    try {
      await terminateChild(child, cleanupTimeoutMs);
    } catch (cleanupError) {
      if (shutdownError instanceof Error) shutdownError.cause = cleanupError;
    }
    throw shutdownError;
  }
  if (GRACEFUL_SHUTDOWN_IS_OBSERVABLE) {
    assert.equal(signal, null, `syncd handles SIGTERM itself; stderr=${stderr()}`);
    assert.equal(code, 0, `syncd exits cleanly; stderr=${stderr()}`);
  } else {
    // Terminated rather than asked to stop, so there is no exit code to
    // check -- only that it is no longer running.
    assert.ok(signal !== null || code !== null, "syncd stopped");
  }
  assert.equal(stdout(), `${JSON.stringify(ready)}\n`, "syncd emits exactly one stdout record");
  assertStderr();
}

function fnv1a32(bytes) {
  let hash = 0x811c9dc5;
  for (const byte of bytes) {
    hash ^= byte;
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
  return hash;
}

async function runToExit(executable, arguments_, { env = process.env } = {}) {
  const child = spawn(executable, arguments_, {
    cwd: ROOT,
    env,
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stdout = "";
  let stderr = "";
  child.stdout.setEncoding("utf8");
  child.stderr.setEncoding("utf8");
  child.stdout.on("data", (chunk) => { stdout += chunk; });
  child.stderr.on("data", (chunk) => { stderr += chunk; });
  const [code, signal] = await withTimeout(
    onceEvent(child, "close", ["error"]), `process exit: ${path.basename(executable)}`,
  );
  return { code, signal, stdout, stderr };
}

async function listenGuard(host, port) {
  const server = net.createServer();
  const listening = onceEvent(server, "listening", ["error"]);
  server.listen({ host, port, ipv6Only: host === "::1" });
  await withTimeout(listening, `guard listener on ${host}:${port}`);
  return server;
}

async function closeGuard(server) {
  if (!server.listening) return;
  const closed = onceEvent(server, "close", ["error"]);
  server.close();
  await withTimeout(closed, "guard listener close");
}

async function unusedDualLoopbackPort() {
  for (let attempt = 0; attempt < 20; attempt += 1) {
    const ipv4 = await listenGuard("127.0.0.1", 0);
    const port = ipv4.address().port;
    let ipv6;
    try {
      ipv6 = await listenGuard("::1", port);
      await closeGuard(ipv6);
      await closeGuard(ipv4);
      return port;
    } catch {
      if (ipv6) await closeGuard(ipv6);
      await closeGuard(ipv4);
    }
  }
  throw new Error("could not reserve a dual-loopback test port");
}

test("daemon path override accepts absolute and workspace-relative paths", () => {
  assert.equal(resolveDaemonPath(), SYNCD);
  assert.equal(resolveDaemonPath("/tmp/custom-syncd"), "/tmp/custom-syncd");
  assert.equal(resolveDaemonPath("build-sanitize/syncd"),
               path.join(ROOT, "build-sanitize", "syncd"));
});

test("malformed readiness output rejects only after the spawned child is cleaned up", async () => {
  const outcome = await spawnDaemon({
    executable: process.execPath,
    arguments: ["-e", "process.stdout.write('not-json\\n'); setInterval(() => {}, 1000)"],
  }).then((daemon) => ({ daemon }), (error) => ({ error }));
  if (outcome.daemon) {
    await stopDaemon(outcome.daemon.child,
                     outcome.daemon.stderr,
                     outcome.daemon.stdout,
                     outcome.daemon.ready);
  }
  assert.ok(outcome.error instanceof SyntaxError,
            "malformed readiness is rejected after terminating its child");
});

test("browser SDK performs a bounded health probe against the real loopback daemon", async () => {
  const daemon = await spawnDaemon();
  try {
    const bridge = new SyncBridgeClient({
      endpoint: `http://127.0.0.1:${daemon.ready.port}`,
      token: TOKEN,
      timeoutMs: TIMEOUT_MS,
    });
    const result = await bridge.probe();
    assert.equal(result.available, true, result.message);
    assert.equal(result.health.product, "Sync");
    assert.equal(result.health.instanceId, daemon.ready.instanceId);
    assert.equal(result.health.protocolVersions.includes(1), true);
    assert.equal(result.health.capabilities.providers.length, 1);
    bridge.close();
  } finally {
    await stopDaemon(daemon.child, daemon.stderr, daemon.stdout, daemon.ready);
  }
});

test("browser SDK pairs, authenticates, rotates, revokes, and reports native denial", async () => {
  const temporaryRoot = await realpath(os.tmpdir());
  const directory = await mkdtemp(path.join(temporaryRoot, "sync-browser-sdk-pairing-"));
  const storePath = path.join(directory, "state", "sdk.store");
  const origin = "https://sdk-client.example";
  const WebSocket = webSocketForOrigin(origin);
  const clients = new Set();
  let daemon;

  const bridge = (token) => {
    const client = new SyncBridgeClient({
      endpoint: `http://127.0.0.1:${daemon.ready.port}`,
      token,
      WebSocket,
      timeoutMs: TIMEOUT_MS,
      pairingTimeoutMs: 5_000,
    });
    clients.add(client);
    return client;
  };
  const stop = async () => {
    if (!daemon) return;
    await stopDaemon(daemon.child, daemon.stderr, daemon.stdout, daemon.ready);
    daemon = undefined;
  };

  try {
    daemon = await spawnPairingDaemon(storePath, "approve");
    const first = await bridge().pair("Noisedeck");
    assert.match(first.token, /^[a-f0-9]{64}$/);
    assert.equal((await bridge(first.token).connect()).type, "welcome");
    for (const client of clients) client.close();
    clients.clear();
    await stop();

    daemon = await spawnPairingDaemon(storePath, "approve");
    const rotated = await bridge().pair("Noisedeck restarted");
    assert.match(rotated.token, /^[a-f0-9]{64}$/);
    assert.notEqual(rotated.token, first.token);
    await assert.rejects(bridge(first.token).connect(), SyncAuthenticationError);
    assert.equal((await bridge(rotated.token).connect()).type, "welcome");
    for (const client of clients) client.close();
    clients.clear();

    const revocation = await runToExit(PAIRING_TEST_SERVER, [storePath, "revoke", origin]);
    assert.deepEqual(revocation, { code: 0, signal: null, stdout: "", stderr: "" });
    await assert.rejects(bridge(rotated.token).connect(), SyncAuthenticationError);
    await stop();

    daemon = await spawnPairingDaemon(path.join(directory, "state", "denied.store"), "deny");
    await assert.rejects(bridge().pair("Noisedeck"), SyncPairingDeniedError);
  } finally {
    for (const client of clients) client.close();
    await stop();
    await rm(directory, { recursive: true });
  }
});

test("dynamic pairing binds durable tokens and sender tickets to normalized origins", async () => {
  const temporaryRoot = await realpath(os.tmpdir());
  const directory = await mkdtemp(path.join(temporaryRoot, "sync-pairing-loopback-"));
  const activeDaemons = new Set();
  const activeClients = new Set();
  const authorityReleasePaths = new Set();

  const start = async (filename, mode) => {
    const storePath = path.join(directory, "state", filename);
    const daemon = await spawnPairingDaemon(storePath, mode);
    daemon.authorityReleasePath = `${storePath}.release`;
    authorityReleasePaths.add(daemon.authorityReleasePath);
    activeDaemons.add(daemon);
    return daemon;
  };
  const releaseAuthority = async (daemon) => {
    await writeFile(daemon.authorityReleasePath, "release", { mode: 0o600 });
  };
  const waitForPrecommit = async (daemon) => {
    const deadline = Date.now() + TIMEOUT_MS;
    while (Date.now() < deadline) {
      try {
        assert.equal(
          await readFile(`${daemon.authorityReleasePath}.entered`, "utf8"),
          "entered",
        );
        return;
      } catch {
        await new Promise((resolve) => setTimeout(resolve, 5));
      }
    }
    assert.fail("timed out waiting for the store precommit hook");
  };
  const stop = async (daemon) => {
    await stopDaemon(daemon.child, daemon.stderr, daemon.stdout, daemon.ready);
    activeDaemons.delete(daemon);
  };
  const finishPairingClient = async (client, description) => {
    const response = await client.nextJson(description);
    const close = await client.nextFrame(`${description} close`);
    assert.equal(close.opcode, 0x8);
    client.send(0x8, close.payload);
    await client.waitClosed();
    activeClients.delete(client);
    return response;
  };

  try {
    const originA = "https://deck-a.example";
    const originB = "https://deck-b.example";
    const originC = "app://noisedeck";
    const originD = "https://deck-d.example";
    let daemon = await start("approved.store", "approve");

    for (const host of ["127.0.0.1", "::1"]) {
      const response = await health(host, daemon.ready.port, originB);
      assert.equal(response.status, 200);
      assert.equal(response.headers.get("access-control-allow-origin"), originB);
      assert.equal(response.headers.get("vary"), "Origin");
    }
    const anonymousHealth = await health("127.0.0.1", daemon.ready.port);
    assert.equal(anonymousHealth.status, 200);
    assert.equal(anonymousHealth.headers.has("access-control-allow-origin"), false);
    const privateNetwork = await preflight("127.0.0.1", daemon.ready.port, originA, true);
    assert.equal(privateNetwork.status, 204);
    assert.equal(privateNetwork.body.length, 0);
    assert.equal(privateNetwork.headers.get("access-control-allow-origin"), originA);
    assert.equal(privateNetwork.headers.get("access-control-allow-methods"), "GET");
    assert.equal(privateNetwork.headers.get("access-control-allow-private-network"), "true");
    assert.equal(
      privateNetwork.headers.get("vary"),
      "Origin, Access-Control-Request-Method, Access-Control-Request-Private-Network",
    );

    const paired = await pairWithDaemon(daemon.ready, originA);
    assert.deepEqual(Object.keys(paired).sort(), ["protocolVersion", "token", "type"]);
    assert.equal(paired.type, "paired");
    assert.equal(paired.protocolVersion, 1);
    assert.match(paired.token, /^[a-f0-9]{64}$/);
    assert.equal((await pairWithDaemon(daemon.ready, originA, "Immediate rotation")).code,
                 "pairing_cooldown",
                 "successful prompts also cool down before token rotation");

    const correctControl = await authenticateControl(daemon.ready, originA, paired.token);
    activeClients.add(correctControl.client);
    assert.equal((await correctControl.client.nextJson("paired welcome")).type, "welcome");

    const wrongOriginControl = await authenticateControl(daemon.ready, originB, paired.token);
    activeClients.add(wrongOriginControl.client);
    assert.equal(
      (await wrongOriginControl.client.nextJson("wrong-origin authentication")).code,
      "authentication_failed",
    );
    const wrongOriginClose = await wrongOriginControl.client.nextFrame("wrong-origin close");
    wrongOriginControl.client.send(0x8, wrongOriginClose.payload);
    await wrongOriginControl.client.waitClosed();
    activeClients.delete(wrongOriginControl.client);

    correctControl.client.sendJson({ type: "createSender", name: "Origin-bound sender" });
    const sender = await correctControl.client.nextJson("origin-bound sender creation");
    assert.equal(sender.type, "senderCreated");
    const crossOriginData = await upgrade({
      port: daemon.ready.port,
      route: sender.path,
      origin: originB,
      subprotocol: `sync.sender.${sender.ticket}`,
    });
    assert.notEqual(crossOriginData.status, 101,
                    "origin B cannot consume a sender ticket owned by origin A");
    const ownedData = await upgrade({
      port: daemon.ready.port,
      route: sender.path,
      origin: originA,
      subprotocol: `sync.sender.${sender.ticket}`,
    });
    assert.equal(ownedData.status, 101);
    activeClients.add(ownedData.client);
    correctControl.client.sendJson({ type: "closeSender", senderId: sender.id });
    assert.equal((await correctControl.client.nextJson("origin-bound sender close")).type,
                 "senderClosed");
    const ownedDataClose = await ownedData.client.nextFrame("origin-bound data close");
    ownedData.client.send(0x8, ownedDataClose.payload);
    await ownedData.client.waitClosed();
    activeClients.delete(ownedData.client);
    correctControl.client.destroy();
    activeClients.delete(correctControl.client);

    const oversized = await upgrade({
      port: daemon.ready.port,
      route: "/pair",
      origin: originB,
    });
    assert.equal(oversized.status, 101);
    activeClients.add(oversized.client);
    oversized.client.send(0x1, Buffer.alloc(1025, 0x20));
    assert.deepEqual(await finishPairingClient(oversized.client, "oversized pairing request"), {
      type: "error",
      code: "request_too_large",
      message: "Pairing request exceeds 1024 bytes",
    });

    await stop(daemon);
    daemon = await start("approved.store", "approve");
    const persistedControl = await authenticateControl(daemon.ready, originA, paired.token);
    activeClients.add(persistedControl.client);
    assert.equal((await persistedControl.client.nextJson("persisted token welcome")).type,
                 "welcome");
    persistedControl.client.destroy();
    activeClients.delete(persistedControl.client);
    const rotated = await pairWithDaemon(daemon.ready, originA, "Noisedeck restarted");
    assert.match(rotated.token, /^[a-f0-9]{64}$/);
    assert.notEqual(rotated.token, paired.token);
    const retiredControl = await authenticateControl(daemon.ready, originA, paired.token);
    activeClients.add(retiredControl.client);
    assert.equal((await retiredControl.client.nextJson("rotated token rejection")).code,
                 "authentication_failed");
    retiredControl.client.destroy();
    activeClients.delete(retiredControl.client);
    const rotatedControl = await authenticateControl(daemon.ready, originA, rotated.token);
    activeClients.add(rotatedControl.client);
    assert.equal((await rotatedControl.client.nextJson("rotated token welcome")).type, "welcome");
    rotatedControl.client.destroy();
    activeClients.delete(rotatedControl.client);
    const revocation = await runToExit(PAIRING_TEST_SERVER, [
      path.join(directory, "state", "approved.store"), "revoke", originA,
    ]);
    assert.deepEqual(revocation, { code: 0, signal: null, stdout: "", stderr: "" });
    const revokedControl = await authenticateControl(daemon.ready, originA, rotated.token);
    activeClients.add(revokedControl.client);
    assert.equal((await revokedControl.client.nextJson("revoked token rejection")).code,
                 "authentication_failed");
    revokedControl.client.destroy();
    activeClients.delete(revokedControl.client);
    await stop(daemon);

    daemon = await start("denied.store", "deny");
    assert.equal((await pairWithDaemon(daemon.ready, originA)).code, "pairing_denied");
    assert.equal((await pairWithDaemon(daemon.ready, originB, "Different origin")).code,
                 "pairing_cooldown",
                 "one webpage cannot replace another prompt during the global cooldown");
    await stop(daemon);

    daemon = await start("native-timeout.store", "timeout");
    assert.equal((await pairWithDaemon(daemon.ready, originA)).code, "pairing_timeout");
    await stop(daemon);

    daemon = await start("late-approval.store", "late-approve");
    const lateApproval = await pairWithDaemon(daemon.ready, originA, "Too late");
    assert.equal(lateApproval.code, "pairing_timeout");
    assert.equal(Object.hasOwn(lateApproval, "token"), false);
    const lateCount = await runToExit(PAIRING_TEST_SERVER, [
      path.join(directory, "state", "late-approval.store"), "count", "0",
    ]);
    assert.deepEqual(lateCount, { code: 0, signal: null, stdout: "", stderr: "" },
                     "approval observed after the deadline never issues or mutates the store");
    await stop(daemon);

    daemon = await start("uncertain.store", "uncertain");
    const uncertain = await pairWithDaemon(daemon.ready, originA);
    assert.equal(uncertain.code, "store_durability_uncertain");
    assert.equal(Object.hasOwn(uncertain, "token"), false);
    await stop(daemon);

    daemon = await start("held-auth.store", "hold-auth");
    const heldAuthToken = await pairWithDaemon(daemon.ready, originA, "Held auth seed");
    const responsiveControl = await authenticateControl(
      daemon.ready, originA, heldAuthToken.token,
    );
    activeClients.add(responsiveControl.client);
    assert.equal((await responsiveControl.client.nextJson("held-auth seed welcome")).type,
                 "welcome");
    responsiveControl.client.sendJson({ type: "createSender", name: "Responsive sender" });
    const responsiveSender = await responsiveControl.client.nextJson("responsive sender");
    const responsiveData = await upgrade({
      port: daemon.ready.port,
      route: responsiveSender.path,
      origin: originA,
      subprotocol: `sync.sender.${responsiveSender.ticket}`,
    });
    assert.equal(responsiveData.status, 101);
    activeClients.add(responsiveData.client);

    const pendingAuthentication = await authenticateControl(
      daemon.ready, originA, heldAuthToken.token,
    );
    activeClients.add(pendingAuthentication.client);
    await new Promise((resolve) => setTimeout(resolve, 50));
    pendingAuthentication.client.sendJson({
      type: "hello", token: heldAuthToken.token, protocolVersions: [1],
    });
    responsiveControl.client.send(0x9, Buffer.from("auth-ping"));
    const authPong = await withTimeout(
      responsiveControl.client.nextFrame("pong while authentication is held"),
      "prompt pong while authentication is held", 250,
    );
    assert.equal(authPong.opcode, 0xa);
    assert.equal(authPong.payload.toString("utf8"), "auth-ping");
    responsiveData.client.send(0x2, await readFile(FIXTURE));
    responsiveControl.client.sendJson({ type: "getStats", senderId: responsiveSender.id });
    assert.equal((await withTimeout(
      responsiveControl.client.nextJson("stats while authentication is held"),
      "stats while authentication is held", 250,
    )).accepted, 1);
    await releaseAuthority(daemon);
    assert.equal((await pendingAuthentication.client.nextJson("released authentication")).type,
                 "welcome");
    await new Promise((resolve) => setTimeout(resolve, 100));
    assert.equal(
      await readFile(`${daemon.authorityReleasePath}.auth-count`, "utf8"),
      "2",
      "multiple messages on a pending connection schedule only one authentication",
    );
    pendingAuthentication.client.destroy();
    activeClients.delete(pendingAuthentication.client);
    responsiveControl.client.destroy();
    activeClients.delete(responsiveControl.client);
    responsiveData.client.destroy();
    activeClients.delete(responsiveData.client);
    await stop(daemon);

    daemon = await start("saturated-authority-seed.store", "approve");
    const saturatedAuthorityToken = await pairWithDaemon(
      daemon.ready, originA, "Authority saturation seed",
    );
    await stop(daemon);
    daemon = await start("saturated-authority-seed.store", "hold-first-auth");
    for (let index = 0;
         index < 64;
         index += 1) {
      const pending = await authenticateControl(
        daemon.ready, originA, saturatedAuthorityToken.token,
      );
      pending.client.send(0x8, Buffer.from([0x03, 0xe8]));
      assert.equal((await pending.client.nextFrame("pending auth close")).opcode, 0x8);
      await pending.client.waitClosed();
    }
    const saturatedAuthentication = await authenticateControl(
      daemon.ready, originA, saturatedAuthorityToken.token,
    );
    activeClients.add(saturatedAuthentication.client);
    await releaseAuthority(daemon);
    assert.equal(
      (await saturatedAuthentication.client.nextJson("authority cancel recovery")).type,
      "welcome",
      "canceling disconnected queued requests immediately recovers bounded capacity",
    );
    saturatedAuthentication.client.destroy();
    activeClients.delete(saturatedAuthentication.client);
    await stop(daemon);

    daemon = await start("held-issue-seed.store", "approve");
    const heldIssueToken = await pairWithDaemon(daemon.ready, originA, "Held issue seed");
    await stop(daemon);
    daemon = await start("held-issue-seed.store", "hold-issue");
    const issueControl = await authenticateControl(daemon.ready, originA, heldIssueToken.token);
    activeClients.add(issueControl.client);
    assert.equal((await issueControl.client.nextJson("held-issue welcome")).type, "welcome");
    issueControl.client.sendJson({ type: "createSender", name: "Issue responsive sender" });
    const issueSender = await issueControl.client.nextJson("held-issue sender");
    const issueData = await upgrade({
      port: daemon.ready.port,
      route: issueSender.path,
      origin: originA,
      subprotocol: `sync.sender.${issueSender.ticket}`,
    });
    assert.equal(issueData.status, 101);
    activeClients.add(issueData.client);
    const pendingIssue = await upgrade({
      port: daemon.ready.port, route: "/pair", origin: originB,
    });
    activeClients.add(pendingIssue.client);
    pendingIssue.client.sendJson({
      type: "pair", protocolVersions: [1], name: "Held issue",
    });
    await new Promise((resolve) => setTimeout(resolve, 50));
    issueControl.client.send(0x9, Buffer.from("issue-ping"));
    const issuePong = await withTimeout(
      issueControl.client.nextFrame("pong while issue is held"),
      "pong while issue is held", 250,
    );
    assert.equal(issuePong.opcode, 0xa);
    issueData.client.send(0x2, await readFile(FIXTURE));
    issueControl.client.sendJson({ type: "getStats", senderId: issueSender.id });
    assert.equal((await withTimeout(
      issueControl.client.nextJson("stats while issue is held"),
      "stats while issue is held", 250,
    )).accepted, 1);
    const heldIssueResponse = await finishPairingClient(
      pendingIssue.client, "held issue timeout",
    );
    assert.equal(heldIssueResponse.code, "pairing_timeout");
    assert.equal(Object.hasOwn(heldIssueResponse, "token"), false);
    await releaseAuthority(daemon);
    await new Promise((resolve) => setTimeout(resolve, 100));
    assert.equal((await health("127.0.0.1", daemon.ready.port, originA)).status, 200,
                 "late issue results are dropped without destabilizing the loop");
    issueControl.client.destroy();
    activeClients.delete(issueControl.client);
    issueData.client.destroy();
    activeClients.delete(issueData.client);
    await stop(daemon);

    daemon = await start("precommit-timeout.store", "hold-precommit");
    const precommitTimeout = await upgrade({
      port: daemon.ready.port, route: "/pair", origin: originA,
    });
    activeClients.add(precommitTimeout.client);
    precommitTimeout.client.sendJson({
      type: "pair", protocolVersions: [1], name: "Precommit timeout",
    });
    await waitForPrecommit(daemon);
    const precommitTimeoutResponse = await finishPairingClient(
      precommitTimeout.client, "precommit timeout",
    );
    assert.equal(precommitTimeoutResponse.code, "pairing_timeout");
    assert.equal(Object.hasOwn(precommitTimeoutResponse, "token"), false);
    await releaseAuthority(daemon);
    await new Promise((resolve) => setTimeout(resolve, 100));
    const precommitTimeoutCount = await runToExit(PAIRING_TEST_SERVER, [
      path.join(directory, "state", "precommit-timeout.store"), "count", "0",
    ]);
    assert.deepEqual(
      precommitTimeoutCount,
      { code: 0, signal: null, stdout: "", stderr: "" },
      "timeout cancellation observed before commit leaves an empty store empty",
    );
    await stop(daemon);

    daemon = await start("precommit-disconnect.store", "approve");
    const precommitOldToken = await pairWithDaemon(
      daemon.ready, originA, "Precommit disconnect seed",
    );
    await stop(daemon);
    const precommitStorePath = path.join(directory, "state", "precommit-disconnect.store");
    const precommitBytesBefore = await readFile(precommitStorePath);
    daemon = await start("precommit-disconnect.store", "hold-precommit");
    const precommitDisconnect = await upgrade({
      port: daemon.ready.port, route: "/pair", origin: originA,
    });
    precommitDisconnect.client.sendJson({
      type: "pair", protocolVersions: [1], name: "Canceled rotation",
    });
    await waitForPrecommit(daemon);
    precommitDisconnect.client.destroy();
    await new Promise((resolve) => setTimeout(resolve, 50));
    await releaseAuthority(daemon);
    await new Promise((resolve) => setTimeout(resolve, 100));
    assert.deepEqual(
      await readFile(precommitStorePath),
      precommitBytesBefore,
      "disconnect cancellation before commit preserves exact store bytes",
    );
    const precommitOldControl = await authenticateControl(
      daemon.ready, originA, precommitOldToken.token,
    );
    activeClients.add(precommitOldControl.client);
    assert.equal(
      (await precommitOldControl.client.nextJson("old token after canceled rotation")).type,
      "welcome",
    );
    precommitOldControl.client.destroy();
    activeClients.delete(precommitOldControl.client);
    await stop(daemon);

    daemon = await start("disconnected-issue.store", "hold-issue");
    const disconnectedIssue = await upgrade({
      port: daemon.ready.port, route: "/pair", origin: originC,
    });
    disconnectedIssue.client.sendJson({
      type: "pair", protocolVersions: [1], name: "Disconnected issue",
    });
    await new Promise((resolve) => setTimeout(resolve, 50));
    disconnectedIssue.client.destroy();
    await new Promise((resolve) => setTimeout(resolve, 50));
    await releaseAuthority(daemon);
    await new Promise((resolve) => setTimeout(resolve, 100));
    assert.equal((await health("127.0.0.1", daemon.ready.port, originC)).status, 200,
                 "a late issue result for a disconnected client is discarded");
    await stop(daemon);

    daemon = await start("hang.store", "hang");
    const cancelled = await upgrade({ port: daemon.ready.port, route: "/pair", origin: originA });
    assert.equal(cancelled.status, 101);
    cancelled.client.sendJson({ type: "pair", protocolVersions: [1], name: "Cancelled" });
    await new Promise((resolve) => setTimeout(resolve, 100));
    cancelled.client.destroy();
    await new Promise((resolve) => setTimeout(resolve, 100));
    assert.equal((await pairWithDaemon(daemon.ready, originA, "Retry")).code,
                 "pairing_cooldown");
    await stop(daemon);

    daemon = await start("duplicate.store", "hang");
    const duplicate = await upgrade({ port: daemon.ready.port, route: "/pair", origin: originD });
    assert.equal(duplicate.status, 101);
    activeClients.add(duplicate.client);
    duplicate.socket.write(Buffer.concat([
      maskedClientFrame(0x1, Buffer.from(JSON.stringify({
        type: "pair", protocolVersions: [1], name: "First request",
      }))),
      maskedClientFrame(0x1, Buffer.from(JSON.stringify({
        type: "pair", protocolVersions: [1], name: "Duplicate request",
      }))),
    ]));
    assert.equal((await finishPairingClient(duplicate.client, "duplicate pairing request")).code,
                 "duplicate_request");
    await stop(daemon);

    daemon = await start("pending.store", "hang");
    const pending = await upgrade({ port: daemon.ready.port, route: "/pair", origin: originB });
    assert.equal(pending.status, 101);
    activeClients.add(pending.client);
    pending.client.sendJson({ type: "pair", protocolVersions: [1], name: "Pending" });
    await new Promise((resolve) => setTimeout(resolve, 100));
    assert.equal((await pairWithDaemon(daemon.ready, originC, "Saturated")).code,
                 "prompt_saturated");
    assert.equal((await pairWithDaemon(daemon.ready, originC, "Saturated retry")).code,
                 "pairing_cooldown");
    assert.equal((await finishPairingClient(pending.client, "prompt timeout")).code,
                 "pairing_timeout");
    await stop(daemon);

    daemon = await start("saturated.store", "saturate");
    assert.equal((await pairWithDaemon(daemon.ready, originA)).code, "prompt_saturated");
    assert.equal((await pairWithDaemon(daemon.ready, originA, "Immediate retry")).code,
                 "pairing_cooldown");
    await stop(daemon);
  } finally {
    for (const releasePath of authorityReleasePaths) {
      await writeFile(releasePath, "release", { mode: 0o600 }).catch(() => {});
    }
    for (const client of activeClients) client.destroy();
    for (const daemon of activeDaemons) {
      await stopDaemon(daemon.child, daemon.stderr, daemon.stdout, daemon.ready);
    }
    await rm(directory, { recursive: true, force: true });
  }
});

// This exercises the harness's own force-reap path by spawning a child that
// deliberately ignores the stop signal. Nothing can ignore a stop on Windows:
// child.kill() there is TerminateProcess regardless of the signal named, so
// the child dies instantly and the timeout under test can never be reached.
test("graceful shutdown timeout force-reaps the child and preserves the timeout error", {
  skip: process.platform === "win32"
    ? "a Windows process cannot ignore termination, so the timeout cannot occur"
    : false,
}, async () => {
  const ready = {
    type: "ready",
    port: 1,
    protocolVersions: [1],
    instanceId: "shutdown-timeout-helper",
  };
  const script =
    `const ready=${JSON.stringify(JSON.stringify(ready))};` +
    "process.on('SIGTERM', () => {});" +
    "process.stdout.write(ready + '\\n');" +
    "setInterval(() => {}, 1000);";
  const daemon = await spawnDaemon({
    executable: process.execPath,
    arguments: ["-e", script],
  });
  try {
    assert.equal(stopDaemon.length, 5, "stopDaemon accepts bounded shutdown options");
    let shutdownError;
    try {
      await stopDaemon(daemon.child,
                       daemon.stderr,
                       daemon.stdout,
                       daemon.ready,
                       { shutdownTimeoutMs: 25, cleanupTimeoutMs: 25 });
    } catch (error) {
      shutdownError = error;
    }
    assert.match(shutdownError?.message ?? "", /Timed out waiting for syncd shutdown/);
    assert.equal(daemon.child.signalCode, "SIGKILL");
    assert.equal(daemon.child.exitCode, null);
  } finally {
    if (daemon.child.exitCode === null && daemon.child.signalCode === null) {
      const exited = onceEvent(daemon.child, "exit", ["error"]);
      daemon.child.kill("SIGKILL");
      await withTimeout(exited, "shutdown helper emergency cleanup", 500);
    }
  }
});

test("syncd serves bounded loopback health, authenticated control, and dedicated frame data", async () => {
  const sockets = new Set();
  let daemon;
  try {
    daemon = await spawnDaemon();
    const { child, ready } = daemon;
    assert.deepEqual(Object.keys(ready).sort(),
                     ["instanceId", "loopback", "port", "protocolVersions", "type"]);
    assert.equal(ready.type, "ready");
    // The daemon reports the stacks it actually bound, so a host that fell back
    // to IPv4 is distinguishable from a dual-stack one instead of both looking
    // identical. IPv4 is the contract and is always present.
    assert.ok(Array.isArray(ready.loopback));
    assert.ok(ready.loopback.includes("127.0.0.1"));
    assert.deepEqual(ready.loopback, ready.loopback.filter(
      (address) => address === "127.0.0.1" || address === "::1"));
    assert.ok(Number.isInteger(ready.port) && ready.port > 0 && ready.port <= 65_535);
    assert.deepEqual(ready.protocolVersions, [1]);
    assert.equal(typeof ready.instanceId, "string");
    assert.ok(ready.instanceId.length >= 32);

    const expectedHealth = {
      product: "Sync",
      status: "ok",
      version: EXPECTED_PRODUCT_VERSION,
      protocolVersions: [1],
      instanceId: ready.instanceId,
      capabilities: {
        send: true,
        receive: false,
        providers: [{ id: "test", direction: "send", available: true, selected: true }],
      },
    };
    for (const host of ["127.0.0.1", "::1"]) {
      const response = await health(host, ready.port, ORIGIN);
      assert.equal(response.status, 200);
      assert.equal(response.headers.get("content-type"), "application/json");
      assert.equal(response.headers.get("connection"), "close");
      assert.equal(Number(response.headers.get("content-length")), response.body.length);
      assert.equal(response.headers.get("access-control-allow-origin"), ORIGIN);
      assert.equal(response.headers.get("vary"), "Origin");
      assert.deepEqual(JSON.parse(response.body.toString("utf8")), expectedHealth);
    }

    const publicHealth = await health("127.0.0.1", ready.port, "https://evil.example");
    assert.equal(publicHealth.status, 200);
    assert.equal(publicHealth.headers.get("access-control-allow-origin"),
                 "https://evil.example");
    assert.equal(publicHealth.headers.get("vary"), "Origin");
    const initialStatus = await status("127.0.0.1", ready.port);
    assert.equal(initialStatus.status, 200);
    assert.deepEqual(JSON.parse(initialStatus.body), {
      ...expectedHealth,
      activeSenders: 0,
    });
    assert.equal((await status("127.0.0.1", ready.port, ORIGIN)).status, 403,
                 "native status does not opt into browser CORS");

    const deniedUpgrade = await upgrade({
      port: ready.port,
      route: "/control",
      origin: "https://evil.example",
    });
    assert.notEqual(deniedUpgrade.status, 101);

    const wrongToken = await upgrade({ port: ready.port, route: "/control", origin: ORIGIN });
    assert.equal(wrongToken.status, 101);
    sockets.add(wrongToken.client);
    wrongToken.client.sendJson({ type: "hello", token: "wrong-token", protocolVersions: [1] });
    const authError = await wrongToken.client.nextJson("authentication error");
    assert.deepEqual(authError, {
      type: "error",
      code: "authentication_failed",
      message: "Invalid token or protocol version",
    });
    const authClose = await wrongToken.client.nextFrame("authentication close");
    assert.equal(authClose.opcode, 0x8);
    wrongToken.client.send(0x8, authClose.payload);
    await wrongToken.client.waitClosed();
    sockets.delete(wrongToken.client);

    const control = await upgrade({ port: ready.port, route: "/control", origin: ORIGIN });
    assert.equal(control.status, 101);
    sockets.add(control.client);
    control.client.sendJson({ type: "hello", token: TOKEN, protocolVersions: [1] });
    assert.deepEqual(await control.client.nextJson("welcome"), {
      type: "welcome",
      protocolVersion: 1,
      version: EXPECTED_PRODUCT_VERSION,
      instanceId: ready.instanceId,
      capabilities: expectedHealth.capabilities,
    });
    control.client.sendJson({ type: "hello", token: TOKEN, protocolVersions: [1] });
    assert.deepEqual(await control.client.nextJson("out-of-order hello error"), {
      type: "error",
      code: "out_of_order",
      message: "Hello is only valid as the first message",
    });
    control.client.send(0x1, Buffer.from(
      '{"type":"createSender","name":"Not Created","extra":true}', "utf8",
    ));
    assert.deepEqual(await control.client.nextJson("strict malformed control error"), {
      type: "error",
      code: "bad_request",
      message: "Malformed control message",
    });

    control.client.sendJson({ type: "createSender", name: "__sync_test_reject_open__" });
    assert.deepEqual(await control.client.nextJson("publisher open rejection"), {
      type: "error",
      code: "publisher_unavailable",
      message: "Publisher is unavailable",
    });
    control.client.sendJson({ type: "createSender", name: "After Rejected Open" });
    const afterRejected = await control.client.nextJson("creation after rejected open");
    assert.equal(afterRejected.type, "senderCreated");
    control.client.sendJson({ type: "closeSender", senderId: afterRejected.id });
    assert.deepEqual(await control.client.nextJson("close after rejected open"), {
      type: "senderClosed",
      id: afterRejected.id,
    });

    control.client.sendJson({ type: "createSender", name: "__sync_test_backpressure_once__" });
    const pressured = await control.client.nextJson("one-shot backpressure sender creation");
    assert.equal(pressured.type, "senderCreated");
    const pressureData = await upgrade({
      port: ready.port,
      route: pressured.path,
      origin: ORIGIN,
      subprotocol: `sync.sender.${pressured.ticket}`,
    });
    assert.equal(pressureData.status, 101);
    sockets.add(pressureData.client);
    const pressureFixture = await readFile(FIXTURE);
    pressureData.client.send(0x2, pressureFixture);
    const droppedStats = await withTimeout((async () => {
      while (true) {
        control.client.sendJson({ type: "getStats", senderId: pressured.id });
        const current = await control.client.nextJson("backpressure drop statistics");
        if (current.dropped === 1) return current;
        assert.equal(current.dropped, 0);
      }
    })(), "synchronous backpressure drop");
    assert.equal(droppedStats.accepted, 0);
    assert.equal(droppedStats.rejected, 0);
    assert.equal(droppedStats.failed, 0);
    assert.equal(droppedStats.lastSequence, 4294967301);
    assert.equal(droppedStats.lastPresentationTimeUs, 1723305600123456);
    assert.equal(droppedStats.checksum, 0);

    const secondPressureFrame = Buffer.from(pressureFixture);
    secondPressureFrame.writeBigUInt64LE(4294967302n, 36);
    secondPressureFrame.writeBigUInt64LE(1723305600123457n, 44);
    pressureData.client.send(0x2, secondPressureFrame);
    const acceptedPressureStats = await withTimeout((async () => {
      while (true) {
        control.client.sendJson({ type: "getStats", senderId: pressured.id });
        const current = await control.client.nextJson("post-backpressure statistics");
        if (current.accepted === 1) return current;
        assert.equal(current.accepted, 0);
      }
    })(), "second frame acceptance after backpressure");
    assert.equal(acceptedPressureStats.dropped, 1);
    assert.equal(acceptedPressureStats.lastSequence, 4294967302);
    assert.equal(acceptedPressureStats.lastPresentationTimeUs, 1723305600123457);
    assert.equal(acceptedPressureStats.checksum, fnv1a32(secondPressureFrame.subarray(64)));
    await new Promise((resolve) => setTimeout(resolve, 100));
    control.client.sendJson({ type: "getStats", senderId: pressured.id });
    assert.deepEqual(await control.client.nextJson("bounded no-retry observation"),
                     acceptedPressureStats);
    pressureData.client.send(0x9, Buffer.from("pressure-open", "ascii"));
    assert.equal((await pressureData.client.nextFrame("backpressure data socket pong")).opcode,
                 0xa);
    control.client.sendJson({ type: "closeSender", senderId: pressured.id });
    assert.equal((await control.client.nextJson("backpressure sender close")).type,
                 "senderClosed");
    const pressureClose = await pressureData.client.nextFrame("backpressure data close");
    assert.equal(pressureClose.opcode, 0x8);
    pressureData.client.send(0x8, pressureClose.payload);
    await pressureData.client.waitClosed();
    sockets.delete(pressureData.client);

    control.client.sendJson({ type: "createSender", name: "Integration Sender" });
    const created = await control.client.nextJson("sender creation");
    assert.equal(created.type, "senderCreated");
    assert.equal(created.name, "Integration Sender");
    assert.match(created.id, /^[A-Za-z0-9_-]{1,128}$/);
    assert.equal(created.path, `/senders/${created.id}`);
    assert.match(created.ticket, /^[a-f0-9]{32,}$/);
    assert.equal(created.path.includes(created.ticket), false);
    assert.equal(
      JSON.parse((await status("127.0.0.1", ready.port)).body).activeSenders,
      1,
      "status counts a sender immediately after creation",
    );

    const selectedProtocol = `sync.sender.${created.ticket}`;
    const data = await upgrade({
      port: ready.port,
      route: created.path,
      origin: ORIGIN,
      subprotocol: selectedProtocol,
    });
    assert.equal(data.status, 101);
    assert.equal(data.headers.get("sec-websocket-protocol"), selectedProtocol);
    assert.notEqual(data.socket, control.socket, "data uses a distinct TCP connection");
    sockets.add(data.client);

    const replay = await upgrade({
      port: ready.port,
      route: created.path,
      origin: ORIGIN,
      subprotocol: selectedProtocol,
    });
    assert.notEqual(replay.status, 101, "a consumed ticket cannot be replayed");

    const fixture = await readFile(FIXTURE);
    assert.equal(fixture.length, 80);
    const expectedChecksum = fnv1a32(fixture.subarray(64));
    data.client.send(0x2, fixture);

    const stats = await withTimeout((async () => {
      while (true) {
        control.client.sendJson({ type: "getStats", senderId: created.id });
        const current = await control.client.nextJson("receiver statistics");
        if (current.accepted === 1) return current;
        assert.equal(current.accepted, 0, "statistics do not skip accepted frame count");
      }
    })(), "frame receipt across separate sockets");
    assert.deepEqual(stats, {
      type: "stats",
      id: created.id,
      accepted: 1,
      dropped: 0,
      rejected: 0,
      failed: 0,
      lastSequence: 4294967301,
      lastPresentationTimeUs: 1723305600123456,
      checksum: expectedChecksum,
    });

    const pingPayload = Buffer.from("sync-ping", "ascii");
    control.client.send(0x9, pingPayload);
    const pong = await control.client.nextFrame("pong");
    assert.equal(pong.opcode, 0xa);
    assert.deepEqual(pong.payload, pingPayload);

    control.client.sendJson({ type: "closeSender", senderId: created.id });
    assert.deepEqual(await control.client.nextJson("sender close acknowledgement"), {
      type: "senderClosed",
      id: created.id,
    });
    const dataClose = await data.client.nextFrame("data channel close");
    assert.equal(dataClose.opcode, 0x8);
    data.client.send(0x8, dataClose.payload);
    await data.client.waitClosed();
    sockets.delete(data.client);
    assert.equal(
      JSON.parse((await status("127.0.0.1", ready.port)).body).activeSenders,
      0,
      "status removes a sender immediately after closure",
    );

    control.client.sendJson({ type: "createSender", name: "Revoked Before Connect" });
    const revoked = await control.client.nextJson("unused-ticket sender creation");
    control.client.sendJson({ type: "createSender", name: "Revoked Before Connect" });
    assert.deepEqual(await control.client.nextJson("duplicate sender-name error"), {
      type: "error",
      code: "duplicate_sender",
      message: "Sender name already exists",
    });
    control.client.sendJson({ type: "closeSender", senderId: revoked.id });
    assert.deepEqual(await control.client.nextJson("unused-ticket sender close"), {
      type: "senderClosed",
      id: revoked.id,
    });
    const revokedUpgrade = await upgrade({
      port: ready.port,
      route: revoked.path,
      origin: ORIGIN,
      subprotocol: `sync.sender.${revoked.ticket}`,
    });
    assert.notEqual(revokedUpgrade.status, 101, "closing a sender revokes its unused ticket");

    control.client.sendJson({ type: "createSender", name: "Close Deadline" });
    const deadlineSender = await control.client.nextJson("close-deadline sender creation");
    const deadlineData = await upgrade({
      port: ready.port,
      route: deadlineSender.path,
      origin: ORIGIN,
      subprotocol: `sync.sender.${deadlineSender.ticket}`,
    });
    assert.equal(deadlineData.status, 101);
    sockets.add(deadlineData.client);
    control.client.sendJson({ type: "closeSender", senderId: deadlineSender.id });
    assert.equal((await control.client.nextJson("close-deadline sender close")).type,
                 "senderClosed");
    const forcedClose = await deadlineData.client.nextFrame("server-initiated close without reply");
    assert.equal(forcedClose.opcode, 0x8);
    const forcedCloseStarted = performance.now();
    await deadlineData.client.waitClosed();
    const forcedCloseElapsed = performance.now() - forcedCloseStarted;
    assert.ok(forcedCloseElapsed >= 400,
              `server waited for peer Close before deadline (${forcedCloseElapsed}ms)`);
    assert.ok(forcedCloseElapsed < 2_000,
              `server force-closed by its bounded deadline (${forcedCloseElapsed}ms)`);
    sockets.delete(deadlineData.client);

    // Sender limits are live limits, not process-lifetime quotas. Repeatedly create,
    // publish through, and close enough distinct senders to cross every 64-entry
    // native table boundary. The same-socket pong is an ordering barrier before
    // querying statistics on the separate control connection.
    for (let index = 0; index < 64; index += 1) {
      control.client.sendJson({ type: "createSender", name: `Churn ${index}` });
      const churnCreated = await control.client.nextJson(`churn sender ${index} creation`);
      assert.equal(churnCreated.type, "senderCreated");
      const churnProtocol = `sync.sender.${churnCreated.ticket}`;
      const churnData = await upgrade({
        port: ready.port,
        route: churnCreated.path,
        origin: ORIGIN,
        subprotocol: churnProtocol,
      });
      assert.equal(churnData.status, 101);
      sockets.add(churnData.client);
      churnData.client.send(0x2, fixture);
      churnData.client.send(0x9, Buffer.from([index]));
      const barrier = await churnData.client.nextFrame(`churn sender ${index} ordering pong`);
      assert.equal(barrier.opcode, 0xa);
      control.client.sendJson({ type: "getStats", senderId: churnCreated.id });
      const churnStats = await control.client.nextJson(`churn sender ${index} statistics`);
      assert.equal(churnStats.accepted, 1);
      assert.equal(churnStats.lastSequence, 4294967301);
      assert.equal(churnStats.checksum, expectedChecksum);
      control.client.sendJson({ type: "closeSender", senderId: churnCreated.id });
      assert.equal((await control.client.nextJson(`churn sender ${index} close`)).type,
                   "senderClosed");
      const churnClose = await churnData.client.nextFrame(`churn sender ${index} data close`);
      assert.equal(churnClose.opcode, 0x8);
      churnData.client.send(0x8, churnClose.payload);
      await churnData.client.waitClosed();
      sockets.delete(churnData.client);
    }

    control.client.send(0x9, Buffer.from("still-open", "ascii"));
    assert.equal((await control.client.nextFrame("control remains open")).opcode, 0xa);
    control.client.send(0x8, Buffer.from([0x03, 0xe8]));
    assert.equal((await control.client.nextFrame("normal control close")).opcode, 0x8);
    await control.client.waitClosed();
    sockets.delete(control.client);

    await stopDaemon(child, daemon.stderr, daemon.stdout, daemon.ready);
  } finally {
    for (const client of sockets) client.destroy();
    if (daemon) await stopDaemon(daemon.child, daemon.stderr, daemon.stdout, daemon.ready);
  }
});

test("aggregate data payload exhaustion isolates the offender and reclaims capacity", async () => {
  const sockets = new Set();
  let daemon;
  try {
    daemon = await spawnDaemon();
    const { ready } = daemon;
    const control = await authenticateControl(ready, ORIGIN, TOKEN);
    sockets.add(control.client);
    assert.equal((await control.client.nextJson("budget control welcome")).type,
                 "welcome");

    const first = await createTestSender(control.client, ready.port, "Budget holder");
    const second = await createTestSender(control.client, ready.port, "Budget offender");
    sockets.add(first.data);
    sockets.add(second.data);

    const declared = 64 * 1024 * 1024 + 64;
    first.data.socket.write(maskedClientFrameHeader(0x2, declared));
    second.data.socket.write(maskedClientFrameHeader(0x2, declared));

    const rejected = await second.data.nextFrame("aggregate budget close");
    assert.equal(rejected.opcode, 0x8);
    assert.equal(rejected.payload.readUInt16BE(0), 1013);
    assert.equal(rejected.payload.subarray(2).toString("utf8"),
                 "inbound_budget_exhausted");
    second.data.send(0x8, rejected.payload);
    await second.data.waitClosed();
    sockets.delete(second.data);

    const concurrentHealth = await health("127.0.0.1", ready.port, ORIGIN);
    assert.equal(concurrentHealth.status, 200);
    assert.equal(JSON.parse(concurrentHealth.body).status, "ok");
    control.client.send(0x9, Buffer.from("budget-control", "ascii"));
    assert.equal((await control.client.nextFrame("control under exhaustion")).opcode,
                 0xa);

    control.client.sendJson({ type: "closeSender", senderId: second.created.id });
    assert.equal((await control.client.nextJson("budget offender cleanup")).type,
                 "senderClosed");
    control.client.sendJson({ type: "closeSender", senderId: first.created.id });
    assert.equal((await control.client.nextJson("budget holder cleanup")).type,
                 "senderClosed");
    const holderClose = await first.data.nextFrame("budget holder close");
    assert.equal(holderClose.opcode, 0x8);
    first.data.send(0x8, holderClose.payload);
    await first.data.waitClosed();
    sockets.delete(first.data);

    const replacement = await createTestSender(
      control.client, ready.port, "Budget replacement",
    );
    sockets.add(replacement.data);
    replacement.data.socket.write(maskedClientFrameHeader(0x2, declared));
    await new Promise((resolve) => setTimeout(resolve, 150));
    assert.equal(replacement.data.closed, false,
                 "reclaimed capacity admits a replacement declaration");
    assert.equal(replacement.data.frames.length, 0,
                 "replacement is not rejected as over budget");

    replacement.data.destroy();
    await replacement.data.waitClosed();
    sockets.delete(replacement.data);
    control.client.sendJson({ type: "closeSender", senderId: replacement.created.id });
    assert.equal((await control.client.nextJson("budget replacement cleanup")).type,
                 "senderClosed");

    control.client.send(0x8, Buffer.from([0x03, 0xe8]));
    assert.equal((await control.client.nextFrame("budget control close")).opcode, 0x8);
    await control.client.waitClosed();
    sockets.delete(control.client);
  } finally {
    for (const client of sockets) client.destroy();
    if (daemon) await stopDaemon(daemon.child, daemon.stderr, daemon.stdout, daemon.ready);
  }
});

test("an incomplete data message times out without expiring an idle sender", async () => {
  const sockets = new Set();
  let daemon;
  try {
    daemon = await spawnDaemon();
    const { ready } = daemon;
    const control = await authenticateControl(ready, ORIGIN, TOKEN);
    sockets.add(control.client);
    assert.equal((await control.client.nextJson("timeout control welcome")).type,
                 "welcome");

    const partial = await createTestSender(control.client, ready.port, "Partial frame");
    const idle = await createTestSender(control.client, ready.port, "Idle sender");
    sockets.add(partial.data);
    sockets.add(idle.data);

    const started = performance.now();
    partial.data.socket.write(Buffer.from([0x82]));

    const concurrentHealth = await health("127.0.0.1", ready.port, ORIGIN);
    assert.equal(concurrentHealth.status, 200);
    control.client.send(0x9, Buffer.from("timeout-control", "ascii"));
    assert.equal((await control.client.nextFrame("control during partial frame")).opcode,
                 0xa);

    const timedOut = await partial.data.nextFrame("incomplete frame timeout", 4_000);
    const elapsed = performance.now() - started;
    assert.equal(timedOut.opcode, 0x8);
    assert.equal(timedOut.payload.readUInt16BE(0), 1008);
    assert.equal(timedOut.payload.subarray(2).toString("utf8"),
                 "incomplete_frame_timeout");
    assert.ok(elapsed >= 1_800 && elapsed < 2_900,
              `incomplete frame timeout was ${elapsed}ms`);
    partial.data.send(0x8, timedOut.payload);
    await partial.data.waitClosed();
    sockets.delete(partial.data);

    idle.data.send(0x9, Buffer.from("idle-survives", "ascii"));
    assert.equal((await idle.data.nextFrame("idle data pong after timeout")).opcode,
                 0xa);

    control.client.sendJson({ type: "closeSender", senderId: partial.created.id });
    assert.equal((await control.client.nextJson("partial sender cleanup")).type,
                 "senderClosed");
    control.client.sendJson({ type: "closeSender", senderId: idle.created.id });
    assert.equal((await control.client.nextJson("idle sender cleanup")).type,
                 "senderClosed");
    const idleClose = await idle.data.nextFrame("idle sender close");
    idle.data.send(0x8, idleClose.payload);
    await idle.data.waitClosed();
    sockets.delete(idle.data);

    control.client.send(0x8, Buffer.from([0x03, 0xe8]));
    assert.equal((await control.client.nextFrame("timeout control close")).opcode,
                 0x8);
    await control.client.waitClosed();
    sockets.delete(control.client);
  } finally {
    for (const client of sockets) client.destroy();
    if (daemon) await stopDaemon(daemon.child, daemon.stderr, daemon.stdout, daemon.ready);
  }
});

test("an upgrade pipelined with WebSocket payload is not judged an oversized header", async () => {
  let daemon;
  const sockets = new Set();
  try {
    daemon = await spawnDaemon();
    const { ready } = daemon;

    // A client may send its first WebSocket frames in the same segment as the
    // handshake. Only the request head belongs to the header budget; counting
    // the trailing payload against it rejects a valid connection.
    const hello = maskedClientFrame(
      0x1,
      Buffer.from(JSON.stringify({ type: "hello", token: TOKEN, protocolVersions: [1] }), "utf8"),
    );
    const pingCount = 200;
    const filler = maskedClientFrame(0x9, Buffer.alloc(125, 0x70));
    const pipelined = Buffer.concat([
      hello,
      ...Array.from({ length: pingCount }, () => filler),
    ]);
    assert.ok(pipelined.length > 16_384, "payload exceeds the header budget on its own");

    const accepted = await upgrade({
      port: ready.port,
      route: "/control",
      origin: ORIGIN,
      pipelined,
    });
    sockets.add(accepted.socket);
    assert.equal(accepted.status, 101);
    const welcome = await accepted.client.nextJson("welcome after pipelined hello");
    assert.equal(welcome.type, "welcome");
    // Every pipelined frame is processed, not just the ones that fit the head.
    for (let index = 0; index < pingCount; index += 1) {
      const pong = await accepted.client.nextFrame(`pong ${index}`);
      assert.equal(pong.opcode, 0xa);
      assert.equal(pong.payload.length, 125);
    }

    // A request head that genuinely exceeds the budget still gets 431.
    const socket = await connect("127.0.0.1", ready.port);
    sockets.add(socket);
    socket.write(
      "GET /control HTTP/1.1\r\n" +
      `Host: 127.0.0.1:${ready.port}\r\n` +
      `X-Padding: ${"p".repeat(20_000)}\r\n`,
    );
    const rejected = await withTimeout(readHttpResponse(socket), "oversized header response");
    assert.equal(rejected.status, 431);
    assert.deepEqual(JSON.parse(rejected.body.toString("utf8")), { error: "headers_too_large" });
  } finally {
    for (const socket of sockets) socket.destroy();
    if (daemon) await stopDaemon(daemon.child, daemon.stderr, daemon.stdout, daemon.ready);
  }
});

test("one control sustains all 64 sender data sockets while health remains responsive", async () => {
  const sockets = new Set();
  let daemon;
  try {
    daemon = await spawnDaemon();
    const { ready } = daemon;
    const control = await upgrade({ port: ready.port, route: "/control", origin: ORIGIN });
    assert.equal(control.status, 101);
    sockets.add(control.client);
    control.client.sendJson({ type: "hello", token: TOKEN, protocolVersions: [1] });
    assert.equal((await control.client.nextJson("maximum-sender welcome")).type, "welcome");

    const senders = [];
    for (let index = 0; index < 64; index += 1) {
      control.client.sendJson({ type: "createSender", name: `Concurrent ${index}` });
      const created = await control.client.nextJson(`concurrent sender ${index} creation`);
      assert.equal(created.type, "senderCreated");
      const data = await upgrade({
        port: ready.port,
        route: created.path,
        origin: ORIGIN,
        subprotocol: `sync.sender.${created.ticket}`,
      });
      assert.equal(data.status, 101, `sender ${index + 1} has a dedicated data connection`);
      sockets.add(data.client);
      senders.push({ created, data: data.client });
    }

    const concurrentHealth = await health("127.0.0.1", ready.port, ORIGIN);
    assert.equal(concurrentHealth.status, 200, "health retains bounded connection headroom");
    assert.equal(JSON.parse(concurrentHealth.body).status, "ok");
    control.client.send(0x9, Buffer.from("sixty-four-live", "ascii"));
    assert.equal((await control.client.nextFrame("control with 64 live senders")).opcode, 0xa);

    for (const { created, data } of senders) {
      control.client.sendJson({ type: "closeSender", senderId: created.id });
      assert.equal((await control.client.nextJson(`close ${created.id}`)).type, "senderClosed");
      const close = await data.nextFrame(`data close ${created.id}`);
      assert.equal(close.opcode, 0x8);
      data.send(0x8, close.payload);
      await data.waitClosed();
      sockets.delete(data);
    }

    control.client.send(0x8, Buffer.from([0x03, 0xe8]));
    assert.equal((await control.client.nextFrame("maximum-sender control close")).opcode, 0x8);
    await control.client.waitClosed();
    sockets.delete(control.client);
  } finally {
    for (const client of sockets) client.destroy();
    if (daemon) await stopDaemon(daemon.child, daemon.stderr, daemon.stdout, daemon.ready);
  }
});

test("pre-authentication deadlines recover the fixed connection table", async () => {
  const sockets = new Set();
  let daemon;
  try {
    daemon = await spawnDaemon();
    const { ready } = daemon;

    const partial = await connect("127.0.0.1", ready.port);
    sockets.add(partial);
    partial.write(`GET /health HTTP/1.1\r\nHost: 127.0.0.1:${ready.port}\r\n`);
    await withTimeout(onceEvent(partial, "close", ["error"]), "partial HTTP deadline close");
    sockets.delete(partial);

    const idleControl = await upgrade({ port: ready.port, route: "/control", origin: ORIGIN });
    assert.equal(idleControl.status, 101);
    sockets.add(idleControl.client);
    const idleClose = await idleControl.client.nextFrame("unauthenticated control deadline close");
    assert.equal(idleClose.opcode, 0x8);
    assert.equal(idleClose.payload.readUInt16BE(0), 1008);
    idleControl.client.send(0x8, idleClose.payload);
    await idleControl.client.waitClosed();
    sockets.delete(idleControl.client);

    const recovered = await health("127.0.0.1", ready.port, ORIGIN);
    assert.equal(recovered.status, 200);
    assert.equal(JSON.parse(recovered.body).status, "ok");
  } finally {
    for (const socket of sockets) socket.destroy();
    if (daemon) await stopDaemon(daemon.child, daemon.stderr, daemon.stdout, daemon.ready);
  }
});

test("a control Close immediately revokes every unused sender ticket", async () => {
  const sockets = new Set();
  let daemon;
  try {
    daemon = await spawnDaemon();
    const { ready } = daemon;
    for (let index = 0; index < 65; index += 1) {
      const control = await upgrade({ port: ready.port, route: "/control", origin: ORIGIN });
      assert.equal(control.status, 101);
      sockets.add(control.client);
      control.client.sendJson({ type: "hello", token: TOKEN, protocolVersions: [1] });
      assert.equal((await control.client.nextJson(`revocation welcome ${index}`)).type, "welcome");
      control.client.sendJson({ type: "createSender", name: `Revocation ${index}` });
      const sender = await control.client.nextJson(`revocation sender ${index}`);
      assert.equal(sender.type, "senderCreated",
                   `control disconnect recycled publisher state through sender ${index}`);

      control.client.send(0x8, Buffer.from([0x03, 0xe8]));

      // Racing the revocation against a data socket that was already
      // connected and waiting proves revocation happens on the close itself
      // rather than on some later sweep. It depends on the daemon seeing the
      // control close before the data upgrade, which is an event-ordering
      // property of the platform, not a guarantee: two separate TCP
      // connections have no ordering relative to each other. libuv on macOS
      // orders them consistently; IOCP on Windows does not, and a ticket
      // that is still usable in that window is not a leak -- the client
      // asked to close, but the connection it belongs to is genuinely still
      // open until the daemon processes that. So the race is asserted only
      // where it is deterministic.
      if (process.platform !== "win32") {
        const stagedDataSocket = await connect("127.0.0.1", ready.port);
        sockets.add(stagedDataSocket);
        const racedUpgrade = await upgrade({
          port: ready.port,
          route: sender.path,
          origin: ORIGIN,
          subprotocol: `sync.sender.${sender.ticket}`,
          socket: stagedDataSocket,
        });
        assert.notEqual(racedUpgrade.status, 101,
                        `unused ticket ${index} was revoked before data authentication`);
        sockets.delete(stagedDataSocket);
      }

      const closeReply = await control.client.nextFrame(`revocation control close ${index}`);
      assert.equal(closeReply.opcode, 0x8);
      await control.client.waitClosed();
      sockets.delete(control.client);

      // The property that has to hold on every platform: once the control
      // connection is actually gone, its unused tickets are dead. This is
      // deterministic -- the close handshake has completed before the
      // upgrade is attempted -- so it is asserted unconditionally.
      const afterCloseUpgrade = await upgrade({
        port: ready.port,
        route: sender.path,
        origin: ORIGIN,
        subprotocol: `sync.sender.${sender.ticket}`,
      });
      assert.notEqual(afterCloseUpgrade.status, 101,
                      `ticket ${index} outlived the control connection that owned it`);
      if (afterCloseUpgrade.client) afterCloseUpgrade.client.destroy();
    }
  } finally {
    for (const socket of sockets) socket.destroy();
    if (daemon) await stopDaemon(daemon.child, daemon.stderr, daemon.stdout, daemon.ready);
  }
});

test("syncd rejects incomplete test authorization arguments without becoming ready", async () => {
  const child = spawn(SYNCD, ["--port", "0", "--test-origin", ORIGIN, "--test-receiver"], {
    cwd: ROOT,
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stdout = "";
  let stderr = "";
  child.stdout.setEncoding("utf8");
  child.stderr.setEncoding("utf8");
  child.stdout.on("data", (chunk) => { stdout += chunk; });
  child.stderr.on("data", (chunk) => { stderr += chunk; });
  try {
    const [code, signal] = await withTimeout(onceEvent(child, "close", ["error"]),
                                             "invalid syncd invocation exit");
    assert.equal(signal, null);
    assert.notEqual(code, 0);
    assert.equal(stdout, "");
    assert.match(stderr, /^usage: syncd /);
  } finally {
    await terminateChild(child);
  }
});

test("syncd Syphon mode reports truthful healthy degradation or sender availability", async () => {
  if (process.platform !== "darwin") return;
  const probe = await runToExit(SYPHON_PROBE, []);
  assert.equal(probe.signal, null);
  assert.equal(probe.code, 0, probe.stderr);
  assert.equal(probe.stderr, "");
  const discovery = JSON.parse(probe.stdout.trim());
  assert.equal(typeof discovery.available, "boolean");

  let daemon;
  // The daemon's own verdict, not the probe's: the probe only inspects the
  // Syphon half, and an unusable Metal device would sink the provider with a
  // discoverable framework in place.
  let daemonCanSend = true;
  const sockets = new Set();
  try {
    daemon = await spawnDaemon({
      arguments: [
        "--port", "0",
        "--test-origin", ORIGIN,
        "--test-token", TOKEN,
        "--publisher", "syphon",
      ],
    });
    const response = await health("127.0.0.1", daemon.ready.port, ORIGIN);
    assert.equal(response.status, 200);
    const healthBody = JSON.parse(response.body.toString("utf8"));
    daemonCanSend = healthBody.capabilities.send;
    assert.equal(healthBody.version, EXPECTED_PRODUCT_VERSION);
    assert.equal(healthBody.capabilities.receive, false);
    assert.deepEqual(healthBody.capabilities.providers, [{
      id: "syphon",
      direction: "send",
      available: healthBody.capabilities.send,
      selected: true,
    }]);
    if (!discovery.available) assert.equal(healthBody.capabilities.send, false);

    const control = await upgrade({
      port: daemon.ready.port,
      route: "/control",
      origin: ORIGIN,
    });
    assert.equal(control.status, 101);
    sockets.add(control.client);
    control.client.sendJson({ type: "hello", token: TOKEN, protocolVersions: [1] });
    const welcome = await control.client.nextJson("Syphon welcome");
    assert.equal(welcome.version, EXPECTED_PRODUCT_VERSION);
    assert.deepEqual(welcome.capabilities, healthBody.capabilities);
    control.client.sendJson({ type: "createSender", name: "Syphon Integration" });
    const creation = await control.client.nextJson("Syphon sender result");
    if (healthBody.capabilities.send) {
      assert.equal(creation.type, "senderCreated");
      control.client.sendJson({ type: "closeSender", senderId: creation.id });
      assert.deepEqual(await control.client.nextJson("Syphon sender close"), {
        type: "senderClosed",
        id: creation.id,
      });
    } else {
      assert.deepEqual(creation, {
        type: "error",
        code: "publisher_unavailable",
        message: "Publisher is unavailable",
      });
    }
    control.client.send(0x8, Buffer.from([0x03, 0xe8]));
    assert.equal((await control.client.nextFrame("Syphon control close")).opcode, 0x8);
    await control.client.waitClosed();
    sockets.delete(control.client);
  } finally {
    for (const client of sockets) client.destroy();
    if (daemon) {
      await stopDaemon(daemon.child, daemon.stderr, daemon.stdout, daemon.ready, {
        expectedStderr: daemonCanSend
          ? ""
          : /^syncd: provider "syphon" is selected but unavailable: \S.*\n$/,
      });
    }
  }
});

test("syncd rejects every invalid publisher-mode CLI shape without a ready record", async () => {
  const common = ["--port", "0", "--test-origin", ORIGIN, "--test-token", TOKEN];
  const invalidArgumentSets = [
    common,
    [...common, "--test-receiver", "--publisher", "syphon"],
    [...common, "--publisher", "unknown"],
    [...common, "--publisher"],
    [...common, "--test-receiver", "--test-receiver"],
    [...common, "--publisher", "syphon", "--publisher", "syphon"],
    [...common, "--syphon-framework", "/tmp/Syphon.framework"],
    [...common, "--test-receiver", "--syphon-framework", "/tmp/Syphon.framework"],
    [...common, "--publisher", "syphon", "--syphon-framework"],
    [...common, "--publisher", "syphon", "--syphon-framework", "/one",
      "--syphon-framework", "/two"],
    [...common, "--publisher", "ndi", "--syphon-framework", "/S.framework"],
    [...common, "--publisher", "syphon", "--ndi-runtime", "/opt/ndi"],
    [...common, "--publisher", "syphon", "--spout-library", "C:/x.dll"],
    [...common, "--publisher", "ndi", "--publisher", "ndi"],
    [...common, "--publisher", "ndi", "--ndi-runtime"],
    [...common, "--publisher", "ndi", "--ndi-runtime", "/one",
      "--ndi-runtime", "/two"],
    [...common, "--ndi-runtime", "/opt/ndi"],
    [...common, "--spout-library", "C:/x.dll"],
    [...common, "--test-receiver", "--unknown"],
    ["--port", "0", "--port", "1", "--test-origin", ORIGIN, "--test-token", TOKEN,
      "--test-receiver"],
  ];

  for (const arguments_ of invalidArgumentSets) {
    const result = await runToExit(SYNCD, arguments_);
    assert.equal(result.signal, null, `signal for ${arguments_.join(" ")}`);
    assert.equal(result.code, 2, `exit code for ${arguments_.join(" ")}: ${result.stderr}`);
    assert.equal(result.stdout, "", `no ready record for ${arguments_.join(" ")}`);
    assert.match(result.stderr, /^usage: syncd /);
  }
});

test("syncd no-argument production mode uses the default port and dynamic pairing", async () => {
  const temporaryHome = await realpath(await mkdtemp(
    path.join(os.tmpdir(), "sync-production-home-"),
  ));
  let ipv4Guard;
  let ipv6Guard;
  let daemon;
  try {
    ipv4Guard = await listenGuard("127.0.0.1", 53979);
    ipv6Guard = await listenGuard("::1", 53979);
    await closeGuard(ipv6Guard);
    ipv6Guard = undefined;
    await closeGuard(ipv4Guard);
    ipv4Guard = undefined;

    daemon = await spawnDaemon({
      arguments: [],
      env: isolatedStoreEnvironment(temporaryHome),
    });
    assert.equal(daemon.ready.port, 53979);
    for (const host of ["127.0.0.1", "::1"]) {
      const response = await health(host, 53979, ORIGIN);
      assert.equal(response.status, 200);
      const body = JSON.parse(response.body.toString("utf8"));
      const providers = body.capabilities.providers;
      assert.deepEqual(providers.map((provider) => provider.id).sort(),
                       [...DEFAULT_PROVIDER_IDS].sort());
      for (const provider of providers) {
        assert.equal(provider.direction, "send");
        // Naming no publisher configures every platform provider, so each
        // is selected; whether its runtime loaded is a separate question.
        assert.equal(provider.selected, true, `${provider.id} must be selected`);
        assert.equal(typeof provider.available, "boolean");
      }
      assert.equal(body.capabilities.send,
                   providers.some((provider) => provider.available));
    }
    const unknownControl = await upgrade({
      port: 53979,
      route: "/control",
      origin: ORIGIN,
    });
    assert.equal(unknownControl.status, 101);
    unknownControl.client.sendJson({
      type: "hello",
      token: "0".repeat(64),
      protocolVersions: [1],
    });
    assert.equal((await unknownControl.client.nextJson("production auth rejection")).code,
                 "authentication_failed");
    unknownControl.client.destroy();
  } finally {
    if (daemon) {
      await stopDaemon(daemon.child, daemon.stderr, daemon.stdout, daemon.ready);
    }
    if (ipv6Guard) await closeGuard(ipv6Guard);
    if (ipv4Guard) await closeGuard(ipv4Guard);
    await rm(temporaryHome, { recursive: true, force: true });
  }
});

test("syncd production --port remains healthy when explicit Syphon discovery is absent", async () => {
  const temporaryHome = await realpath(await mkdtemp(
    path.join(os.tmpdir(), "sync-degraded-home-"),
  ));
  const port = await unusedDualLoopbackPort();
  let daemon;
  try {
    daemon = await spawnDaemon({
      arguments: [
        "--port", String(port),
        "--publisher", "syphon",
        "--syphon-framework", path.join(temporaryHome, "missing", "Syphon.framework"),
      ],
      env: isolatedStoreEnvironment(temporaryHome),
    });
    assert.equal(daemon.ready.port, port);
    for (const host of ["127.0.0.1", "::1"]) {
      const response = await health(host, port, ORIGIN);
      assert.equal(response.status, 200);
      const body = JSON.parse(response.body.toString("utf8"));
      assert.equal(body.capabilities.send, false);
      assert.deepEqual(body.capabilities.providers, [{
        id: "syphon",
        direction: "send",
        available: false,
        selected: true,
      }]);
    }
  } finally {
    if (daemon) {
      // Pointed at a framework that is not there, so the reason is not merely
      // present but known. `available:false` with no explanation is the defect
      // this asserts against.
      await stopDaemon(daemon.child, daemon.stderr, daemon.stdout, daemon.ready, {
        expectedStderr: 'syncd: provider "syphon" is selected but unavailable: '
          + "no Syphon.framework was found in any searched location\n",
      });
    }
    await rm(temporaryHome, { recursive: true, force: true });
  }
});

test("syncd management lists and revokes the isolated default store without opening listeners", async () => {
  const temporaryHome = await realpath(await mkdtemp(
    path.join(os.tmpdir(), "sync-management-home-"),
  ));
  const storePath = path.join(
    temporaryHome,
    ...(process.platform === "win32"
      ? ["Noisefactor Sync"]
      : ["Library", "Application Support", "Noisefactor Sync"]),
    "pairings.v1",
  );
  const managementOrigin = "https://management.example";
  let seedDaemon;
  let ipv4Guard;
  let ipv6Guard;
  try {
    seedDaemon = await spawnPairingDaemon(storePath, "approve");
    const paired = await pairWithDaemon(seedDaemon.ready, managementOrigin, "Management Seed");
    assert.equal(paired.type, "paired");
    await stopDaemon(seedDaemon.child, seedDaemon.stderr, seedDaemon.stdout, seedDaemon.ready);
    seedDaemon = undefined;

    ipv4Guard = await listenGuard("127.0.0.1", 53979);
    ipv6Guard = await listenGuard("::1", 53979);
    const isolatedEnvironment = isolatedStoreEnvironment(temporaryHome);
    const listed = await runToExit(SYNCD, ["--list-pairings"], {
      env: isolatedEnvironment,
    });
    assert.equal(listed.signal, null);
    assert.equal(listed.code, 0, listed.stderr);
    assert.equal(listed.stderr, "");
    assert.deepEqual(JSON.parse(listed.stdout), {
      type: "pairings",
      origins: [managementOrigin],
    });
    assert.equal(listed.stdout.includes(paired.token), false);
    assert.equal(listed.stdout.includes(createHash("sha256").update(
      Buffer.from(paired.token, "hex"),
    ).digest("hex")), false);

    const revoked = await runToExit(
      SYNCD,
      ["--revoke-origin", "HTTPS://MANAGEMENT.EXAMPLE:443"],
      { env: isolatedEnvironment },
    );
    assert.equal(revoked.signal, null);
    assert.equal(revoked.code, 0, revoked.stderr);
    assert.equal(revoked.stderr, "");
    assert.deepEqual(JSON.parse(revoked.stdout), {
      type: "revocation",
      origin: managementOrigin,
      status: "revoked",
    });
    assert.equal(revoked.stdout.includes(paired.token), false);

    const absent = await runToExit(
      SYNCD,
      ["--revoke-origin", managementOrigin],
      { env: isolatedEnvironment },
    );
    assert.equal(absent.code, 0, absent.stderr);
    assert.deepEqual(JSON.parse(absent.stdout), {
      type: "revocation",
      origin: managementOrigin,
      status: "not_found",
    });
  } finally {
    if (seedDaemon) {
      await stopDaemon(seedDaemon.child, seedDaemon.stderr,
                       seedDaemon.stdout, seedDaemon.ready);
    }
    if (ipv6Guard) await closeGuard(ipv6Guard);
    if (ipv4Guard) await closeGuard(ipv4Guard);
    await rm(temporaryHome, { recursive: true, force: true });
  }
});

test("syncd rejects production, static-test, and management mode mixtures", async () => {
  const invalidArgumentSets = [
    ["--port", "0"],
    ["--list-pairings", "--port", "53979"],
    ["--list-pairings", "--revoke-origin", ORIGIN],
    ["--revoke-origin", ORIGIN, "--publisher", "syphon"],
    ["--test-origin", ORIGIN, "--test-token", TOKEN, "--publisher", "syphon"],
  ];
  for (const arguments_ of invalidArgumentSets) {
    const result = await runToExit(SYNCD, arguments_);
    assert.equal(result.signal, null);
    assert.equal(result.code, 2, `${arguments_.join(" ")}: ${result.stderr}`);
    assert.equal(result.stdout, "");
    assert.match(result.stderr, /^usage: syncd /);
  }
});
