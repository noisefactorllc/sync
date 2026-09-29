import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import net from 'node:net';
import test from 'node:test';

const executable = process.env.SYNC_UPDATE_TEST_SERVER;
async function start(mode) {
  const child = spawn(executable, [mode], { stdio: ['ignore', 'pipe', 'pipe'] });
  let text = '';
  let diagnostic = '';
  child.stderr.on('data', chunk => { diagnostic += chunk; });
  try {
    const ready = await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error(`startup timeout: ${diagnostic}`)), 5000);
      child.once('error', error => { clearTimeout(timer); reject(error); });
      child.once('exit', code => { clearTimeout(timer); reject(new Error(`early exit ${code}: ${diagnostic}`)); });
      child.stdout.on('data', chunk => {
        text += chunk;
        if (text.includes('\n')) { clearTimeout(timer); resolve(JSON.parse(text.split('\n')[0])); }
      });
    });
    return { child, port: ready.port };
  } catch (error) { child.kill(); throw error; }
}
async function request(port, upgrade) {
  const socket = net.connect({ host: '127.0.0.1', port });
  const chunks = [];
  try {
    await once(socket, 'connect');
    const head = await new Promise((resolve, reject) => {
      socket.setTimeout(5000, () => reject(new Error('response timeout')));
      socket.on('error', reject);
      socket.on('data', chunk => {
        chunks.push(chunk);
        const response = Buffer.concat(chunks).toString();
        if (response.includes('\r\n\r\n')) resolve(response);
      });
      socket.write(`GET ${upgrade ? '/control' : '/health'} HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\n` +
        'Origin: https://client.example\r\n' +
        (upgrade ? 'Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n' : 'Connection: close\r\n') + '\r\n');
    });
    return head;
  } finally { socket.destroy(); }
}
for (const mode of ['reserved', 'available']) {
  test(`update gate ${mode}: health works and WebSocket admission respects reservation`, async () => {
    assert.ok(executable, 'SYNC_UPDATE_TEST_SERVER must name the compiled fixture');
    const { child, port } = await start(mode);
    try {
      assert.match(await request(port, false), /^HTTP\/1\.1 200/);
      assert.match(await request(port, true), mode === 'reserved' ? /^HTTP\/1\.1 503/ : /^HTTP\/1\.1 101/);
      assert.equal(child.exitCode, null, 'update checks must not stop the daemon');
    } finally {
      const exited = once(child, 'exit');
      child.kill();
      await exited;
    }
  });
}
