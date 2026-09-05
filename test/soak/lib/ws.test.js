import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import net from 'node:net';
import test from 'node:test';
import { connect, upgrade } from './ws.mjs';

function bounded(promise, timeoutMs = 1000) {
  let timer;
  return Promise.race([
    promise,
    new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('test watchdog expired')), timeoutMs);
    }),
  ]).finally(() => clearTimeout(timer));
}

function response(request) {
  const key = /Sec-WebSocket-Key: ([^\r]+)\r\n/i.exec(request)[1];
  const accept = createHash('sha1')
    .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  return Buffer.from('HTTP/1.1 101 Switching Protocols\r\n' +
    'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
    `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
}

async function peer(t, onRequest) {
  const sockets = new Set();
  let closed;
  const disconnected = new Promise((resolve) => { closed = resolve; });
  const server = net.createServer((socket) => {
    sockets.add(socket);
    socket.on('error', () => {});
    socket.once('close', () => { sockets.delete(socket); closed(); });
    let request = '';
    const read = (chunk) => {
      request += chunk.toString('latin1');
      if (!request.includes('\r\n\r\n')) return;
      socket.removeListener('data', read);
      onRequest(socket, request);
    };
    socket.on('data', read);
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  t.after(async () => {
    for (const socket of sockets) socket.destroy();
    await new Promise((resolve) => server.close(resolve));
  });
  return { port: server.address().port, disconnected };
}

test('connect removes its temporary error handler after success', async (t) => {
  const fixture = await peer(t, () => {});
  const socket = await connect(fixture.port);
  t.after(() => socket.destroy());
  assert.equal(socket.listenerCount('error'), 0);
});

test('an upgrade rejects promptly when the peer closes without a response', async (t) => {
  const fixture = await peer(t, (socket) => socket.end());
  await assert.rejects(bounded(upgrade(fixture.port, '/control')), /closed.*upgrade|upgrade.*closed/);
});

test('an unanswered upgrade expires and destroys the connection', async (t) => {
  const fixture = await peer(t, () => {});
  await assert.rejects(bounded(upgrade(fixture.port, '/control', { timeoutMs: 30 })),
    /upgrade.*timed out/);
  await bounded(fixture.disconnected);
});

test('a rejected upgrade destroys the connection', async (t) => {
  const fixture = await peer(t, (socket) => socket.write('HTTP/1.1 403 Forbidden\r\n\r\n'));
  await assert.rejects(upgrade(fixture.port, '/control'), /403 Forbidden/);
  await bounded(fixture.disconnected);
});

test('an oversized unfinished upgrade is rejected before its deadline', async (t) => {
  const fixture = await peer(t, (socket) => socket.write('HTTP/1.1 101 Switching Protocols\r\n' +
    'X-Padding: ' + 'a'.repeat(16_384)));
  await assert.rejects(bounded(upgrade(fixture.port, '/control')), /upgrade.*size limit/);
  await bounded(fixture.disconnected);
});

test('fragmented upgrade headers leave only the established socket handlers', async (t) => {
  const fixture = await peer(t, (socket, request) => {
    const head = response(request);
    let offset = 0;
    const timer = setInterval(() => {
      socket.write(head.subarray(offset, offset + 30));
      offset += 30;
      if (offset >= head.length) clearInterval(timer);
    }, 3);
    t.after(() => clearInterval(timer));
  });
  const ws = await bounded(upgrade(fixture.port, '/control'));
  t.after(() => ws.socket.destroy());
  assert.equal(ws.socket.listenerCount('error'), 1);
  assert.equal(ws.socket.listenerCount('data'), 1);
});

test('a message coalesced with the upgrade response is available immediately', async (t) => {
  const fixture = await peer(t, (socket, request) => {
    const text = Buffer.from('{"ready":true}');
    socket.write(Buffer.concat([response(request), Buffer.from([0x81, text.length]), text]));
  });
  const ws = await bounded(upgrade(fixture.port, '/control'));
  t.after(() => ws.socket.destroy());
  const message = await ws.next(100);
  assert.equal(message.opcode, 1);
  assert.deepEqual(JSON.parse(message.payload.toString()), { ready: true });
});

test('a binary write held in the socket buffer expires and destroys the stream', async (t) => {
  const fixture = await peer(t, (socket, request) => {
    socket.write(response(request));
  });
  const ws = await bounded(upgrade(fixture.port, '/control'));
  t.after(() => ws.socket.destroy());
  // Cork a real socket so its write callback cannot complete before timeout.
  // A fixed large payload can still fit in Windows' loopback send buffers.
  ws.socket.cork();
  await assert.rejects(bounded(ws.sendBinary(Buffer.from([1, 2, 3, 4]), 30)),
    /write.*timed out/);
  assert.equal(ws.socket.destroyed, true);
});

test('a successful binary write preserves its exact wire bytes and releases write handlers', async (t) => {
  let received;
  const wire = new Promise((resolve) => { received = resolve; });
  const fixture = await peer(t, (socket, request) => {
    socket.write(response(request));
    let bytes = Buffer.alloc(0);
    socket.on('data', (chunk) => {
      bytes = Buffer.concat([bytes, chunk]);
      if (bytes.length >= 10) received(bytes);
    });
  });
  const ws = await bounded(upgrade(fixture.port, '/control'));
  t.after(() => ws.socket.destroy());
  await ws.sendBinary(Buffer.from([10, 20, 30, 40]), 500);
  assert.deepEqual(await bounded(wire), Buffer.from([0x82, 0x84, 0, 0, 0, 0, 10, 20, 30, 40]));
  assert.equal(ws.socket.listenerCount('error'), 1);
  assert.equal(ws.socket.listenerCount('close'), 1);
  assert.equal(ws.socket.destroyed, false);
});

test('a binary write after socket destruction rejects without counting a send', async (t) => {
  const fixture = await peer(t, (socket, request) => socket.write(response(request)));
  const ws = await bounded(upgrade(fixture.port, '/control'));
  ws.socket.destroy();
  await assert.rejects(bounded(ws.sendBinary(Buffer.from([1]))), /socket closed/);
  assert.equal(ws.socket.listenerCount('error'), 1);
});
