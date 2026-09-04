import assert from "node:assert/strict";
import { execFile as execFileCallback, spawn } from "node:child_process";
import { randomBytes } from "node:crypto";
import { mkdtemp, mkdir, rm } from "node:fs/promises";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { promisify } from "node:util";

const execFile = promisify(execFileCallback);
const SYNCD = process.env.SYNC_DAEMON_PATH;
const SYNCCTL = process.env.SYNC_CTL_PATH;
const ORIGIN = "https://linux-control.example";

function withTimeout(promise, label, milliseconds = 5_000) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`timed out waiting for ${label}`)),
                       milliseconds);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

function once(emitter, event, failures = ["error"]) {
  return new Promise((resolve, reject) => {
    const cleanup = () => {
      emitter.off(event, success);
      for (const failure of failures) emitter.off(failure, failed);
    };
    const success = (...values) => {
      cleanup();
      resolve(values);
    };
    const failed = (value) => {
      cleanup();
      reject(value instanceof Error ? value : new Error(String(value)));
    };
    emitter.once(event, success);
    for (const failure of failures) emitter.once(failure, failed);
  });
}

async function freePort() {
  const server = net.createServer();
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const { port } = server.address();
  server.close();
  await once(server, "close");
  return port;
}

class WebSocketConnection {
  constructor(socket, initial) {
    this.socket = socket;
    this.buffer = initial;
  }

  sendJson(value) {
    const payload = Buffer.from(JSON.stringify(value));
    const mask = randomBytes(4);
    const header = payload.length <= 125
      ? Buffer.from([0x81, 0x80 | payload.length])
      : Buffer.from([0x81, 0xfe, payload.length >> 8, payload.length & 0xff]);
    const masked = Buffer.alloc(payload.length);
    for (let index = 0; index < payload.length; index += 1) {
      masked[index] = payload[index] ^ mask[index % 4];
    }
    this.socket.write(Buffer.concat([header, mask, masked]));
  }

  async readJson() {
    while (true) {
      if (this.buffer.length >= 2) {
        const opcode = this.buffer[0] & 0x0f;
        let length = this.buffer[1] & 0x7f;
        let offset = 2;
        if (length === 126 && this.buffer.length >= 4) {
          length = this.buffer.readUInt16BE(2);
          offset = 4;
        } else if (length === 127 && this.buffer.length >= 10) {
          const wide = this.buffer.readBigUInt64BE(2);
          assert.ok(wide <= 65_536n);
          length = Number(wide);
          offset = 10;
        } else if (length >= 126) {
          await this.readMore();
          continue;
        }
        if (this.buffer.length < offset + length) {
          await this.readMore();
          continue;
        }
        const payload = this.buffer.subarray(offset, offset + length);
        this.buffer = this.buffer.subarray(offset + length);
        assert.equal(opcode, 1, "daemon response is a text frame");
        return JSON.parse(payload.toString("utf8"));
      }
      await this.readMore();
    }
  }

  async readMore() {
    const [chunk] = await withTimeout(once(this.socket, "data", ["error", "close"]),
                                      "WebSocket frame");
    this.buffer = Buffer.concat([this.buffer, chunk]);
    assert.ok(this.buffer.length <= 1_048_576);
  }

  close() {
    this.socket.destroy();
  }
}

async function websocket(port, route, origin) {
  const socket = net.createConnection({ host: "127.0.0.1", port });
  await withTimeout(once(socket, "connect"), "loopback connection");
  const key = randomBytes(16).toString("base64");
  socket.write(
    `GET ${route} HTTP/1.1\r\n` +
    `Host: 127.0.0.1:${port}\r\n` +
    "Upgrade: websocket\r\nConnection: Upgrade\r\n" +
    `Origin: ${origin}\r\nSec-WebSocket-Version: 13\r\n` +
    `Sec-WebSocket-Key: ${key}\r\n\r\n`,
  );
  let response = Buffer.alloc(0);
  while (response.indexOf("\r\n\r\n") < 0) {
    const [chunk] = await withTimeout(once(socket, "data", ["error", "close"]),
                                      "WebSocket upgrade");
    response = Buffer.concat([response, chunk]);
    assert.ok(response.length <= 65_536);
  }
  const marker = response.indexOf("\r\n\r\n");
  assert.match(response.subarray(0, marker).toString("ascii"), /^HTTP\/1\.1 101 /);
  return new WebSocketConnection(socket, response.subarray(marker + 4));
}

async function runCtl(arguments_, env) {
  const result = await execFile(SYNCCTL, arguments_, { env, timeout: 5_000 });
  assert.equal(result.stderr, "");
  return result.stdout;
}

async function stop(child) {
  if (child.exitCode !== null || child.signalCode !== null) return;
  const exited = once(child, "exit");
  child.kill("SIGTERM");
  await withTimeout(exited, "syncd shutdown");
}

async function waitForExit(child, label) {
  if (child.exitCode !== null) return [child.exitCode, null];
  if (child.signalCode !== null) return [null, child.signalCode];
  return withTimeout(once(child, "exit"), label);
}

test("Linux production daemon pairs, authenticates, lists, and revokes through syncctl",
     { skip: process.platform !== "linux" || !SYNCD || !SYNCCTL }, async () => {
  const root = await mkdtemp(path.join(os.tmpdir(), "sync-linux-control-"));
  const home = path.join(root, "home");
  const runtime = path.join(root, "runtime");
  await mkdir(home, { mode: 0o700 });
  await mkdir(runtime, { mode: 0o700 });
  const port = await freePort();
  const env = { ...process.env, HOME: home, XDG_RUNTIME_DIR: runtime };
  const daemon = spawn(SYNCD, ["--port", String(port), "--publisher", "ndi"], {
    env,
    stdio: ["ignore", "pipe", "pipe"],
  });
  let daemonStdout = "";
  let daemonStderr = "";
  daemon.stdout.setEncoding("utf8");
  daemon.stderr.setEncoding("utf8");
  daemon.stdout.on("data", chunk => { daemonStdout += chunk; });
  daemon.stderr.on("data", chunk => { daemonStderr += chunk; });

  try {
    await withTimeout(new Promise((resolve, reject) => {
      const inspect = () => daemonStdout.includes("\n") && resolve();
      daemon.stdout.on("data", inspect);
      daemon.once("exit", code => reject(new Error(
        `syncd exited before ready (${code}): ${daemonStderr}`)));
    }), "syncd ready record");

    const pairingCtl = spawn(SYNCCTL, ["pair"], {
      env,
      stdio: ["pipe", "pipe", "pipe"],
    });
    let pairOutput = "";
    let pairError = "";
    pairingCtl.stdout.setEncoding("utf8");
    pairingCtl.stderr.setEncoding("utf8");
    pairingCtl.stdout.on("data", chunk => { pairOutput += chunk; });
    pairingCtl.stderr.on("data", chunk => { pairError += chunk; });

    const pairing = await websocket(port, "/pair", ORIGIN);
    pairing.sendJson({ type: "pair", protocolVersions: [1], name: "Noisedeck A" });
    await withTimeout(new Promise(resolve => {
      const inspect = () => pairOutput.includes("Allow this browser") && resolve();
      pairingCtl.stdout.on("data", inspect);
      inspect();
    }), "terminal pairing prompt");
    assert.match(pairOutput, /Origin: https:\/\/linux-control\.example/);
    assert.match(pairOutput, /Name: Noisedeck A/);
    pairingCtl.stdin.end("y\n");
    const paired = await pairing.readJson();
    assert.equal(paired.type, "paired");
    assert.match(paired.token, /^[a-f0-9]{64}$/);
    pairing.close();
    const [pairExit] = await waitForExit(pairingCtl, "syncctl pair exit");
    assert.equal(pairExit, 0, pairError);
    assert.ok(!pairOutput.includes(paired.token), "syncctl output never includes token");

    const pairings = JSON.parse(await runCtl(["pairings", "--json"], env));
    assert.deepEqual(pairings.origins, [ORIGIN]);

    const authenticated = await websocket(port, "/control", ORIGIN);
    authenticated.sendJson({ type: "hello", token: paired.token, protocolVersions: [1] });
    assert.equal((await authenticated.readJson()).type, "welcome");
    authenticated.close();

    const revoked = JSON.parse(await runCtl(["revoke", ORIGIN, "--json"], env));
    assert.equal(revoked.status, "revoked");
    const rejected = await websocket(port, "/control", ORIGIN);
    rejected.sendJson({ type: "hello", token: paired.token, protocolVersions: [1] });
    assert.equal((await rejected.readJson()).code, "authentication_failed");
    rejected.close();
  } finally {
    await stop(daemon);
    assert.ok(!daemonStdout.includes("token"), "daemon stdout contains no token field");
    await rm(root, { recursive: true, force: true });
  }
});
