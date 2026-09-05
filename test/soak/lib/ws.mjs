// --- minimal RFC 6455 client over a raw socket ---------------------------

import { createHash, randomBytes } from 'node:crypto';
import assert from 'node:assert/strict';
import net from 'node:net';

const DEFAULT_TIMEOUT_MS = 5000;
const MAX_UPGRADE_BYTES = 16_384;

function validateTimeout(timeoutMs) {
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 2_147_483_647) {
    throw new RangeError('timeoutMs must be a positive timer-safe integer');
  }
}

function maskedFrame(opcode, payload) {
  // A zero mask key is a valid key and leaves the payload bytes unchanged, so
  // a multi-megabyte frame needs no per-byte work here.
  const length = payload.length;
  let header;
  if (length <= 125) {
    header = Buffer.from([0x80 | opcode, 0x80 | length, 0, 0, 0, 0]);
  } else if (length <= 65535) {
    header = Buffer.alloc(8);
    header[0] = 0x80 | opcode;
    header[1] = 0x80 | 126;
    header.writeUInt16BE(length, 2);
  } else {
    header = Buffer.alloc(14);
    header[0] = 0x80 | opcode;
    header[1] = 0x80 | 127;
    header.writeBigUInt64BE(BigInt(length), 2);
  }
  return [header, payload];
}

export class RawWebSocket {
  constructor(socket, remainder) {
    this.socket = socket;
    this.buffer = remainder;
    this.waiters = [];
    this.messages = [];
    this.closed = false;
    socket.on('data', (chunk) => {
      this.buffer = Buffer.concat([this.buffer, chunk]);
      this.drain();
    });
    socket.on('close', () => {
      this.closed = true;
      for (const waiter of this.waiters.splice(0)) waiter.reject(new Error('socket closed'));
    });
    socket.on('error', () => {});
    this.drain();
  }

  drain() {
    for (;;) {
      if (this.buffer.length < 2) return;
      const opcode = this.buffer[0] & 0x0f;
      let length = this.buffer[1] & 0x7f;
      let offset = 2;
      if (length === 126) {
        if (this.buffer.length < 4) return;
        length = this.buffer.readUInt16BE(2);
        offset = 4;
      } else if (length === 127) {
        if (this.buffer.length < 10) return;
        length = Number(this.buffer.readBigUInt64BE(2));
        offset = 10;
      }
      if (this.buffer.length < offset + length) return;
      const payload = this.buffer.subarray(offset, offset + length);
      this.buffer = this.buffer.subarray(offset + length);
      const message = { opcode, payload: Buffer.from(payload) };
      const waiter = this.waiters.shift();
      if (waiter) waiter.resolve(message);
      else this.messages.push(message);
    }
  }

  next(timeoutMs = 5000) {
    if (this.messages.length > 0) return Promise.resolve(this.messages.shift());
    if (this.closed) return Promise.reject(new Error('socket closed'));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.waiters = this.waiters.filter((w) => w.resolve !== wrapped);
        reject(new Error('timed out waiting for a message'));
      }, timeoutMs);
      const wrapped = (value) => { clearTimeout(timer); resolve(value); };
      this.waiters.push({ resolve: wrapped, reject: (e) => { clearTimeout(timer); reject(e); } });
    });
  }

  async nextJson() {
    for (;;) {
      const message = await this.next();
      if (message.opcode === 0x1) return JSON.parse(message.payload.toString('utf8'));
      if (message.opcode === 0x8) throw new Error(`peer closed: ${message.payload.toString('utf8', 2)}`);
    }
  }

  sendText(text) {
    const [header, payload] = maskedFrame(0x1, Buffer.from(text));
    this.socket.write(Buffer.concat([header, payload]));
  }

  sendBinary(payload, timeoutMs = DEFAULT_TIMEOUT_MS) {
    validateTimeout(timeoutMs);
    const [header] = maskedFrame(0x2, payload);
    // A write to a reset or destroyed socket must not look like success: the
    // callback carries the error, and a soak caller relies on the rejection
    // to notice the connection is dead rather than counting a phantom frame.
    return new Promise((resolve, reject) => {
      let settled = false;
      const finish = (error) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        this.socket.removeListener('error', failed);
        this.socket.removeListener('close', closed);
        if (error) {
          // A partial frame cannot be retried on this stream.
          this.socket.destroy();
          reject(error);
        } else resolve();
      };
      const failed = (error) => finish(error);
      const closed = () => finish(new Error('socket closed during binary write'));
      const timer = setTimeout(() => finish(new Error('binary write timed out')), timeoutMs);
      this.socket.once('error', failed);
      this.socket.once('close', closed);
      if (this.closed || this.socket.destroyed) { closed(); return; }
      try {
        this.socket.write(header, (headerError) => {
          if (settled) return;
          if (headerError) { finish(headerError); return; }
          try {
            this.socket.write(payload, finish);
          } catch (error) { finish(error); }
        });
      } catch (error) { finish(error); }
    });
  }

  close() {
    if (this.closed) return Promise.resolve();
    const [header, payload] = maskedFrame(0x8, Buffer.from([0x03, 0xe8]));
    this.socket.write(Buffer.concat([header, payload]));
    return new Promise((resolve) => {
      const done = () => resolve();
      this.socket.once('close', done);
      setTimeout(() => { this.socket.destroy(); }, 1000).unref();
    });
  }
}

export function connect(port, { timeoutMs = DEFAULT_TIMEOUT_MS } = {}) {
  validateTimeout(timeoutMs);
  return new Promise((resolve, reject) => {
    const socket = net.connect({ host: '127.0.0.1', port });
    socket.setNoDelay(true);
    let settled = false;
    const finish = (error) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      socket.removeListener('connect', connected);
      socket.removeListener('error', failed);
      socket.removeListener('close', closed);
      if (error) {
        socket.destroy();
        reject(error);
      } else resolve(socket);
    };
    const connected = () => finish();
    const failed = (error) => finish(error);
    const closed = () => finish(new Error('socket closed while connecting'));
    const timer = setTimeout(() => finish(new Error('socket connection timed out')), timeoutMs);
    socket.once('connect', connected);
    socket.once('error', failed);
    socket.once('close', closed);
  });
}

export async function upgrade(port, route, {
  origin, subprotocol, timeoutMs = DEFAULT_TIMEOUT_MS,
} = {}) {
  validateTimeout(timeoutMs);
  const deadline = performance.now() + timeoutMs;
  const socket = await connect(port, { timeoutMs });
  const key = randomBytes(16).toString('base64');
  return new Promise((resolve, reject) => {
    let bytes = Buffer.alloc(0);
    let settled = false;
    const cleanup = () => {
      clearTimeout(timer);
      socket.removeListener('data', received);
      socket.removeListener('error', failed);
      socket.removeListener('close', closed);
      socket.removeListener('end', closed);
    };
    const failed = (error) => {
      if (settled) return;
      settled = true;
      cleanup();
      socket.destroy();
      reject(error);
    };
    const closed = () => failed(new Error(`socket closed during upgrade ${route}`));
    const received = (chunk) => {
      try {
        bytes = Buffer.concat([bytes, chunk]);
        const marker = bytes.indexOf('\r\n\r\n');
        if ((marker < 0 ? bytes.length : marker + 4) > MAX_UPGRADE_BYTES) {
          throw new Error(`upgrade ${route} exceeds the header size limit`);
        }
        if (marker < 0) return;
        const head = bytes.subarray(0, marker).toString('latin1');
        assert.match(head, /^HTTP\/1\.1 101 /, `upgrade ${route}: ${head.split('\r\n')[0]}`);
        const expected = createHash('sha1')
          .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
        assert.ok(head.includes(`Sec-WebSocket-Accept: ${expected}`), 'accept key');
        const ws = new RawWebSocket(socket, bytes.subarray(marker + 4));
        settled = true;
        cleanup();
        resolve(ws);
      } catch (error) { failed(error); }
    };
    const timer = setTimeout(() => failed(new Error(`upgrade ${route} timed out`)),
      Math.max(1, Math.ceil(deadline - performance.now())));
    socket.on('data', received);
    socket.once('error', failed);
    socket.once('close', closed);
    socket.once('end', closed);
    if (socket.destroyed || socket.readableEnded) { closed(); return; }
    if (performance.now() >= deadline) {
      failed(new Error(`upgrade ${route} timed out`));
      return;
    }
    try {
      socket.write(
        `GET ${route} HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\nUpgrade: websocket\r\n` +
        `Connection: Upgrade\r\nOrigin: ${origin}\r\nSec-WebSocket-Version: 13\r\n` +
        `Sec-WebSocket-Key: ${key}\r\n` +
        (subprotocol ? `Sec-WebSocket-Protocol: ${subprotocol}\r\n` : '') + '\r\n',
      );
    } catch (error) { failed(error); }
  });
}
