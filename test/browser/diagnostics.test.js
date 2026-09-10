import assert from 'node:assert/strict';
import test from 'node:test';
import { createDiagnosticSnapshot } from '../../browser/diagnostics.js';

test('diagnostics retain transport stages and omit credentials and page data', async () => {
  const secret = 'do-not-copy-this-secret';
  const snapshot = await createDiagnosticSnapshot({
    client: { connected: true, token: secret, welcome: {
      version: '0.2.56', protocolVersion: 1, instanceId: secret,
      capabilities: { providers: [{ id: 'syphon', direction: 'send',
        available: true, selected: true, ticket: secret }] },
    } },
    sender: { id: secret, stats: { accepted: 8, sent: 6, failed: 0,
      droppedBusy: 1, droppedBackpressure: 1, token: secret },
      async getStats() { return { accepted: 4, dropped: 2, failed: 0, rejected: 0,
        lastSequence: 8, lastPresentationTimeUs: 10000, checksum: '0000000000000042', token: secret }; } },
    descriptor: { width: 10, height: 20, format: 'rgba8unorm',
      colorSpace: 'srgb', alphaMode: 'straight', fps: 30, url: secret },
    error: { code: 'SYNC_SENDER_LOST', message: secret, cause: { token: secret } },
  });
  assert.equal(snapshot.local.sent, 6);
  assert.equal(snapshot.native.accepted, 4);
  assert.equal(snapshot.sdkVersion, '0.2.0');
  assert.equal(snapshot.daemonVersion, '0.2.56');
  assert.equal(snapshot.error, 'SYNC_SENDER_LOST');
  assert.equal(JSON.stringify(snapshot).includes(secret), false);
  assert.deepEqual(snapshot.providers, [{ id: 'syphon', direction: 'send', available: true, selected: true }]);
});

test('diagnostics report a failed native request without copying its message', async () => {
  const snapshot = await createDiagnosticSnapshot({
    sender: { stats: { sent: 1 }, async getStats() {
      throw Object.assign(new Error('secret token'), { code: 'SYNC_LIFECYCLE' });
    } },
    error: { code: 'private-secret' },
  });
  assert.equal(snapshot.native, null);
  assert.equal(snapshot.nativeError, 'SYNC_LIFECYCLE');
  assert.equal(snapshot.error, null);
  assert.equal(snapshot.connected, false);
  assert.equal(snapshot.daemonVersion, null);
  assert.equal(JSON.stringify(snapshot).includes('secret'), false);
});
