import assert from 'node:assert/strict';
import http from 'node:http';
import test from 'node:test';
import { probeHealth } from './health.mjs';

async function peer(t, handler) {
  const server = http.createServer(handler);
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => {
    server.closeAllConnections();
    await new Promise((resolve) => server.close(resolve));
  });
  return server.address().port;
}

test('health probes avoid lazy Fetch initialization on the streaming event loop', async (t) => {
  // The five-second CI soak lost a whole sampling window at its first fetch.
  // Treat lazy HTTP-stack initialization as unavailable during streaming.
  t.mock.method(globalThis, 'fetch', () => { throw new Error('cold Fetch initialization'); });
  const port = await peer(t, (request, response) => {
    assert.equal(request.url, '/health');
    assert.equal(request.headers.origin, 'https://soak.example');
    response.end('{"status":"ok"}');
  });
  await probeHealth(port, { origin: 'https://soak.example', timeoutMs: 5000 });
});

test('health probes reject unsuccessful HTTP responses', async (t) => {
  const port = await peer(t, (_request, response) => {
    response.writeHead(503);
    response.end();
  });
  await assert.rejects(probeHealth(port), /503/);
});

for (const headersSent of [false, true]) {
  test(`health deadline covers a stalled ${headersSent ? 'body' : 'response'}`, async (t) => {
    let received;
    const requestReceived = new Promise((resolve) => { received = resolve; });
    const port = await peer(t, (_request, response) => {
      if (headersSent) {
        response.writeHead(200, { 'Content-Length': '100' });
        response.write('partial');
      }
      received();
    });
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const result = assert.rejects(probeHealth(port), /health.*timed out/);
    await requestReceived;
    t.mock.timers.tick(2000);
    await result;
  });
}

test('health probes reject truncated bodies', async (t) => {
  const port = await peer(t, (_request, response) => {
    response.writeHead(200, { 'Content-Length': '100' });
    response.write('partial');
    response.socket.end();
  });
  await assert.rejects(probeHealth(port));
});
