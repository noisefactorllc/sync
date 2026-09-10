import assert from 'node:assert/strict';
import { request } from 'node:http';
import { test } from 'node:test';

import { startExampleServer } from '../../examples/server.mjs';

function get(url) {
  return new Promise((resolve, reject) => {
    const call = request(url, (response) => {
      const chunks = [];
      response.on('data', (chunk) => chunks.push(chunk));
      response.on('end', () => resolve({
        status: response.statusCode,
        headers: response.headers,
        body: Buffer.concat(chunks).toString('utf8'),
      }));
    });
    call.on('error', reject);
    call.end();
  });
}

test('serves the example and browser modules from loopback', async (context) => {
  const server = await startExampleServer({ port: 0 });
  context.after(() => new Promise((resolve) => server.close(resolve)));
  const { port } = server.address();

  const root = await get(`http://127.0.0.1:${port}/`);
  assert.equal(root.status, 200);
  assert.match(root.body, /Sync interoperability example/);
  assert.equal(root.headers['permissions-policy'], 'loopback-network=(self)');

  const module = await get(`http://127.0.0.1:${port}/browser/index.js`);
  assert.equal(module.status, 200);
  assert.match(module.body, /SyncBridgeClient/);
});

test('rejects content outside the two public roots', async (context) => {
  const server = await startExampleServer({ port: 0 });
  context.after(() => new Promise((resolve) => server.close(resolve)));
  const { port } = server.address();

  assert.equal((await get(`http://127.0.0.1:${port}/package.json`)).status, 404);
  assert.equal((await get(`http://127.0.0.1:${port}/..%2fREADME.md`)).status, 404);
});
