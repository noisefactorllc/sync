import assert from 'node:assert/strict';
import test from 'node:test';

import {
  SYNC_DEFAULT_ENDPOINT,
  SYNC_ERROR_CODE,
  SyncAuthenticationError,
  SyncBridgeClient,
  SyncCapabilityError,
  SyncConfigurationError,
  SyncLifecycleError,
  SyncPermissionDeniedError,
  SyncPermissionRequiredError,
  SyncPairingBusyError,
  SyncPairingDeniedError,
  SyncPairingDurabilityError,
  SyncPairingOriginLimitError,
  SyncPairingStoreError,
  SyncProtocolError,
  SyncTimeoutError,
  SyncUnavailableError,
} from '../../browser/client.js';

const ENDPOINT = 'http://127.0.0.1:39079';
const TOKEN = 'test-token-123';
const PAIRED_TOKEN = 'a'.repeat(64);
const INSTANCE_ID = '0123456789abcdef0123456789abcdef';
const CAPABILITIES = Object.freeze({
  send: true,
  receive: false,
  providers: Object.freeze([
    Object.freeze({ id: 'test', direction: 'send', available: true, selected: true }),
  ]),
});
const HEALTH = Object.freeze({
  product: 'Sync',
  status: 'ok',
  version: '0.1.1',
  protocolVersions: Object.freeze([1]),
  instanceId: INSTANCE_ID,
  capabilities: CAPABILITIES,
});
const WELCOME = Object.freeze({
  type: 'welcome',
  protocolVersion: 1,
  version: '0.1.1',
  instanceId: INSTANCE_ID,
  capabilities: CAPABILITIES,
});

function utf8Bytes(value) {
  return new TextEncoder().encode(value);
}

function response(value, { status = 200, contentType = 'application/json', chunks } = {}) {
  const bytes = typeof value === 'string' ? utf8Bytes(value) : utf8Bytes(JSON.stringify(value));
  const parts = chunks ?? [bytes];
  let index = 0;
  return {
    status,
    ok: status >= 200 && status < 300,
    headers: {
      get(name) {
        if (name.toLowerCase() === 'content-type') return contentType;
        if (name.toLowerCase() === 'content-length') return String(bytes.byteLength);
        return null;
      },
    },
    body: {
      getReader() {
        return {
          async read() {
            if (index === parts.length) return { done: true, value: undefined };
            return { done: false, value: parts[index++] };
          },
          async cancel() {},
          releaseLock() {},
        };
      },
    },
  };
}

class FakeWebSocket {
  static CONNECTING = 0;
  static OPEN = 1;
  static CLOSING = 2;
  static CLOSED = 3;
  static instances = [];
  static onConstruct = null;

  static reset(onConstruct) {
    this.instances.length = 0;
    this.onConstruct = onConstruct;
  }

  constructor(url, protocols) {
    this.url = url;
    this.protocols = protocols;
    this.protocol = '';
    this.readyState = FakeWebSocket.CONNECTING;
    this.bufferedAmount = 0;
    this.sent = [];
    this.closeCalls = 0;
    this.listeners = new Map();
    FakeWebSocket.instances.push(this);
    queueMicrotask(() => FakeWebSocket.onConstruct?.(this));
  }

  addEventListener(type, listener) {
    const listeners = this.listeners.get(type) ?? new Set();
    listeners.add(listener);
    this.listeners.set(type, listeners);
  }

  removeEventListener(type, listener) {
    this.listeners.get(type)?.delete(listener);
  }

  get listenerCount() {
    let count = 0;
    for (const listeners of this.listeners.values()) count += listeners.size;
    return count;
  }

  emit(type, event = {}) {
    for (const listener of [...(this.listeners.get(type) ?? [])]) listener.call(this, event);
  }

  open(protocol = '') {
    this.protocol = protocol;
    this.readyState = FakeWebSocket.OPEN;
    this.emit('open');
  }

  message(value) {
    this.emit('message', { data: value });
  }

  fail() {
    this.emit('error', {});
  }

  remoteClose() {
    this.readyState = FakeWebSocket.CLOSED;
    this.emit('close', { code: 1006, reason: '' });
  }

  send(value) {
    if (this.readyState !== FakeWebSocket.OPEN) throw new Error('socket is not open');
    this.sent.push(value);
    this.onSend?.(value);
  }

  close() {
    this.closeCalls += 1;
    if (this.readyState === FakeWebSocket.CLOSED) return;
    this.readyState = FakeWebSocket.CLOSED;
    this.emit('close', { code: 1000, reason: '' });
  }
}

class ExportQueue {
  constructor() {
    this.available = true;
    this.closeCalls = 0;
    this.closeOptions = [];
  }
  configure() {}
  enqueue() { return true; }
  poll() {}
  close(options) {
    this.closeCalls += 1;
    this.closeOptions.push(options);
  }
}

function client(options = {}) {
  return new SyncBridgeClient({
    endpoint: ENDPOINT,
    token: TOKEN,
    fetch: async () => response(HEALTH),
    WebSocket: FakeWebSocket,
    timeoutMs: 50,
    ...options,
  });
}

function permissionScript(steps) {
  const calls = [];
  return {
    calls,
    permissions: {
      async query(descriptor) {
        calls.push(descriptor);
        const step = steps.shift();
        if (step instanceof Error) throw step;
        return { state: step };
      },
    },
  };
}

function unsupportedPermission(message = 'unsupported permission descriptor') {
  return new TypeError(message);
}

async function flushUntil(predicate, description, maximumTurns = 20) {
  for (let turn = 0; turn < maximumTurns; turn += 1) {
    if (predicate()) return;
    await Promise.resolve();
  }
  assert.fail(`did not observe ${description}`);
}

async function outcomeWithinTurns(promise, maximumTurns = 30) {
  let outcome;
  promise.then(
    (value) => { outcome = { status: 'fulfilled', value }; },
    (error) => { outcome = { status: 'rejected', error }; },
  );
  for (let turn = 0; turn < maximumTurns && outcome === undefined; turn += 1) {
    await Promise.resolve();
  }
  return outcome;
}

function scriptedControl({ createResponses = [], onControl } = {}) {
  let senderIndex = 0;
  FakeWebSocket.reset((socket) => {
    if (socket.url.endsWith('/control')) {
      socket.open();
      socket.onSend = (text) => {
        const message = JSON.parse(text);
        onControl?.(message, socket);
        if (message.type === 'hello') socket.message(JSON.stringify(WELCOME));
        if (message.type === 'createSender') {
          const reply = createResponses[senderIndex++] ?? {
            type: 'senderCreated',
            id: `sender_${senderIndex}`,
            name: message.name,
            path: `/senders/sender_${senderIndex}`,
            ticket: `${String(senderIndex).padStart(32, 'a')}`,
          };
          socket.message(JSON.stringify(reply));
        }
        if (message.type === 'closeSender') {
          socket.message(JSON.stringify({ type: 'senderClosed', id: message.senderId }));
        }
      };
      return;
    }
    const protocol = Array.isArray(socket.protocols) ? socket.protocols[0] : socket.protocols;
    socket.open(protocol);
  });
}

function scriptedPairing({ messages = [{
  type: 'paired', protocolVersion: 1, token: PAIRED_TOKEN,
}], close = true, onPair } = {}) {
  FakeWebSocket.reset((socket) => {
    assert.equal(socket.url, 'ws://127.0.0.1:39079/pair');
    socket.onSend = (text) => {
      onPair?.(JSON.parse(text), socket);
      for (const message of messages) {
        socket.message(typeof message === 'string' ? message : JSON.stringify(message));
      }
      if (close) socket.remoteClose();
    };
    socket.open();
  });
}

test('constructor accepts explicit IPv4 and IPv6 loopback endpoints and rejects unsafe endpoints', async () => {
  const requested = [];
  const fetch = async (url) => {
    requested.push(url);
    return response(HEALTH);
  };
  assert.equal((await client({ fetch }).probe()).available, true);
  assert.equal(requested[0], 'http://127.0.0.1:39079/health');

  const ipv6 = client({ endpoint: 'http://[::1]:49079/', fetch });
  assert.equal((await ipv6.probe()).available, true);
  assert.equal(requested[1], 'http://[::1]:49079/health');

  const defaulted = new SyncBridgeClient({
    fetch,
    WebSocket: FakeWebSocket,
    timeoutMs: 50,
  });
  assert.equal((await defaulted.probe()).available, true);
  assert.equal(requested[2], 'http://127.0.0.1:53979/health');
  assert.equal(SYNC_DEFAULT_ENDPOINT, 'http://127.0.0.1:53979');

  for (const endpoint of [
    '',
    'http://localhost:39079',
    'https://127.0.0.1:39079',
    'http://192.168.1.2:39079',
    'http://user:pass@127.0.0.1:39079',
    'http://127.0.0.1',
    'http://127.0.0.1:39079/base',
    'http://127.0.0.1:39079/?token=secret',
    'http://127.0.0.1:39079/#fragment',
  ]) {
    assert.throws(() => client({ endpoint }), SyncConfigurationError, String(endpoint));
  }
});

test('probe sends a credentialless bounded loopback request and validates exact health', async () => {
  let request;
  const result = await client({
    fetch: async (url, options) => {
      request = { url, options };
      const bytes = utf8Bytes(JSON.stringify(HEALTH));
      return response(HEALTH, { chunks: [bytes.subarray(0, 7), bytes.subarray(7)] });
    },
  }).probe();

  assert.deepEqual(result, { available: true, health: HEALTH });
  assert.equal(request.url, `${ENDPOINT}/health`);
  assert.equal(request.options.method, 'GET');
  assert.equal(request.options.credentials, 'omit');
  assert.equal(request.options.cache, 'no-store');
  assert.equal(request.options.redirect, 'error');
  assert.equal(request.options.targetAddressSpace, 'loopback');
  assert.equal(request.options.headers.Accept, 'application/json');
  assert.ok(request.options.signal instanceof AbortSignal);
});

test('probe never throws for absence, HTTP/CORS failure, timeout, oversized data, or malformed health', async () => {
  const cases = [
    async () => { throw new TypeError('Failed to fetch'); },
    async () => response({ nope: true }, { status: 403 }),
    async () => new Promise(() => {}),
    async () => response('x'.repeat(65_537)),
    async () => response('{}', {
      chunks: Array.from({ length: 1_025 }, () => new Uint8Array()),
    }),
    async () => response('{'),
    async () => response({ ...HEALTH, protocolVersions: [2] }),
    async () => response({ ...HEALTH, extra: true }),
    async () => response({
      ...HEALTH,
      capabilities: {
        ...CAPABILITIES,
        providers: Array.from({ length: 5 }, () => CAPABILITIES.providers[0]),
      },
    }),
  ];
  for (const fetch of cases) {
    const result = await client({ fetch, timeoutMs: 5 }).probe();
    assert.equal(result.available, false);
    assert.equal(typeof result.code, 'string');
    assert.equal(typeof result.message, 'string');
  }
});

test('passive probe gates prompt and denied permission without any health request', async () => {
  for (const [state, code] of [
    ['prompt', SYNC_ERROR_CODE.PERMISSION_REQUIRED],
    ['denied', SYNC_ERROR_CODE.PERMISSION_DENIED],
  ]) {
    let fetchCalls = 0;
    const scripted = permissionScript([state]);
    const result = await client({
      permissions: scripted.permissions,
      fetch: async () => {
        fetchCalls += 1;
        return response(HEALTH);
      },
    }).probe();
    assert.deepEqual(scripted.calls, [{ name: 'loopback-network' }]);
    assert.equal(fetchCalls, 0);
    assert.equal(result.available, false);
    assert.equal(result.code, code);
  }
});

test('permission discovery uses legacy only for genuine unsupported and otherwise fails closed', async () => {
  const legacy = permissionScript([unsupportedPermission(), 'granted']);
  let legacyFetches = 0;
  assert.equal((await client({
    permissions: legacy.permissions,
    fetch: async () => {
      legacyFetches += 1;
      return response(HEALTH);
    },
  }).probe()).available, true);
  assert.deepEqual(legacy.calls, [
    { name: 'loopback-network' },
    { name: 'local-network-access' },
  ]);
  assert.equal(legacyFetches, 1);

  const unsupported = permissionScript([
    unsupportedPermission('new unsupported'),
    Object.assign(new Error('legacy unsupported'), { name: 'NotSupportedError' }),
  ]);
  let fallbackFetches = 0;
  assert.equal((await client({
    permissions: unsupported.permissions,
    fetch: async () => {
      fallbackFetches += 1;
      return response(HEALTH);
    },
  }).probe()).available, true);
  assert.equal(fallbackFetches, 1);

  for (const first of [new Error('query failed'), 'unexpected-state']) {
    const failed = permissionScript([first]);
    let fetchCalls = 0;
    const result = await client({
      permissions: failed.permissions,
      fetch: async () => {
        fetchCalls += 1;
        return response(HEALTH);
      },
    }).probe();
    assert.equal(result.available, false);
    assert.equal(result.code, SYNC_ERROR_CODE.UNAVAILABLE);
    assert.equal(fetchCalls, 0);
    assert.deepEqual(failed.calls, [{ name: 'loopback-network' }]);
  }
});

test('a stalled passive probe permission query is bounded without making a health request', async () => {
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  const timers = new Set();
  let bridge;
  let fetchCalls = 0;
  try {
    globalThis.setTimeout = (callback) => {
      const timer = { active: true };
      timers.add(timer);
      queueMicrotask(() => {
        if (timer.active) callback();
      });
      return timer;
    };
    globalThis.clearTimeout = (timer) => {
      timer.active = false;
      timers.delete(timer);
    };
    bridge = client({
      permissions: { query: async () => new Promise(() => {}) },
      fetch: async () => {
        fetchCalls += 1;
        return response(HEALTH);
      },
    });
    const outcome = await outcomeWithinTurns(bridge.probe());
    assert.equal(outcome?.status, 'fulfilled');
    assert.equal(outcome.value.available, false);
    assert.equal(outcome.value.code, SYNC_ERROR_CODE.TIMEOUT);
    assert.equal(fetchCalls, 0);
    assert.equal(timers.size, 0);
  } finally {
    bridge?.close();
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
  }
});

test('a stalled passive connect permission query is bounded without creating a socket', async () => {
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  const timers = new Set();
  let bridge;
  try {
    globalThis.setTimeout = (callback) => {
      const timer = { active: true };
      timers.add(timer);
      queueMicrotask(() => {
        if (timer.active) callback();
      });
      return timer;
    };
    globalThis.clearTimeout = (timer) => {
      timer.active = false;
      timers.delete(timer);
    };
    FakeWebSocket.reset(() => {});
    bridge = client({ permissions: { query: async () => new Promise(() => {}) } });
    const outcome = await outcomeWithinTurns(bridge.connect());
    assert.equal(outcome?.status, 'rejected');
    assert.ok(outcome.error instanceof SyncTimeoutError);
    assert.equal(FakeWebSocket.instances.length, 0);
    assert.equal(timers.size, 0);
  } finally {
    bridge?.close();
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
  }
});

test('an already-expired passive gate never invokes the Permissions API', async () => {
  const originalPerformance = globalThis.performance;
  const nowValues = [0, 51];
  let queryCalls = 0;
  let fetchCalls = 0;
  try {
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: { now: () => nowValues.shift() ?? 51 },
    });
    const bridge = client({
      permissions: {
        async query() {
          queryCalls += 1;
          return { state: 'granted' };
        },
      },
      fetch: async () => {
        fetchCalls += 1;
        return response(HEALTH);
      },
    });
    const result = await bridge.probe();
    assert.equal(result.available, false);
    assert.equal(result.code, SYNC_ERROR_CODE.TIMEOUT);
    assert.equal(queryCalls, 0);
    assert.equal(fetchCalls, 0);
    bridge.close();
  } finally {
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: originalPerformance,
    });
  }
});

test('an expired permission query never advances to the legacy descriptor fallback', async () => {
  const originalPerformance = globalThis.performance;
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  const timers = [];
  const queries = [];
  let now = 0;
  let rejectFirst;
  let bridge;
  try {
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: { now: () => now },
    });
    globalThis.setTimeout = (callback, delay) => {
      const timer = { active: true, callback, delay };
      timers.push(timer);
      return timer;
    };
    globalThis.clearTimeout = (timer) => { timer.active = false; };
    bridge = client({
      permissions: {
        query(descriptor) {
          queries.push(descriptor);
          if (descriptor.name === 'loopback-network') {
            return new Promise((_resolve, reject) => { rejectFirst = reject; });
          }
          return Promise.resolve({ state: 'granted' });
        },
      },
    });
    const probing = bridge.probe();
    await flushUntil(() => rejectFirst !== undefined, 'new permission descriptor query');
    now = 51;
    rejectFirst(unsupportedPermission());
    const outcome = await outcomeWithinTurns(probing);
    assert.equal(outcome?.status, 'fulfilled');
    assert.equal(outcome.value.available, false);
    assert.equal(outcome.value.code, SYNC_ERROR_CODE.TIMEOUT);
    assert.deepEqual(queries, [{ name: 'loopback-network' }]);
    assert.equal(timers.every((timer) => timer.active === false), true);
  } finally {
    bridge?.close();
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: originalPerformance,
    });
  }
});

test('close owns passive permission queries and prevents their late network continuation', async () => {
  const permissionResolvers = [];
  const permissions = {
    query() {
      return new Promise((resolve) => permissionResolvers.push(resolve));
    },
  };
  let fetchCalls = 0;
  scriptedControl();
  const bridge = client({
    permissions,
    fetch: async () => {
      fetchCalls += 1;
      return response(HEALTH);
    },
  });
  const connecting = bridge.connect();
  const probing = bridge.probe();
  await flushUntil(() => permissionResolvers.length === 2, 'both passive permission queries');

  bridge.close();
  const connectOutcome = await outcomeWithinTurns(connecting);
  const probeOutcome = await outcomeWithinTurns(probing);
  for (const resolve of permissionResolvers) resolve({ state: 'granted' });
  await Promise.resolve();
  await Promise.resolve();

  assert.equal(connectOutcome?.status, 'rejected');
  assert.ok(connectOutcome.error instanceof SyncLifecycleError);
  assert.equal(probeOutcome?.status, 'fulfilled');
  assert.equal(probeOutcome.value.available, false);
  assert.equal(probeOutcome.value.code, SYNC_ERROR_CODE.LIFECYCLE);
  assert.equal(typeof probeOutcome.value.message, 'string');
  assert.equal(FakeWebSocket.instances.length, 0);
  assert.equal(fetchCalls, 0);
});

test('connect enforces configuration before permission and passively gates socket creation', async () => {
  {
    const scripted = permissionScript(['prompt']);
    const bridge = new SyncBridgeClient({
      endpoint: ENDPOINT,
      permissions: scripted.permissions,
      WebSocket: FakeWebSocket,
    });
    FakeWebSocket.reset(() => {});
    await assert.rejects(bridge.connect(), SyncConfigurationError);
    assert.equal(scripted.calls.length, 0);
    assert.equal(FakeWebSocket.instances.length, 0);
  }
  for (const [state, ErrorClass] of [
    ['prompt', SyncPermissionRequiredError],
    ['denied', SyncPermissionDeniedError],
  ]) {
    const scripted = permissionScript([state]);
    FakeWebSocket.reset(() => {});
    await assert.rejects(client({ permissions: scripted.permissions }).connect(), ErrorClass);
    assert.deepEqual(scripted.calls, [{ name: 'loopback-network' }]);
    assert.equal(FakeWebSocket.instances.length, 0);
  }
});

test('connect retains the direct socket path when both permission descriptors are unsupported', async () => {
  const scripted = permissionScript([
    unsupportedPermission(),
    Object.assign(new Error('unsupported'), { name: 'NotSupportedError' }),
  ]);
  scriptedControl();
  const bridge = client({ permissions: scripted.permissions });
  assert.deepEqual(await bridge.connect(), WELCOME);
  assert.deepEqual(scripted.calls, [
    { name: 'loopback-network' },
    { name: 'local-network-access' },
  ]);
  assert.equal(FakeWebSocket.instances.length, 1);
  bridge.close();
});

test('pair validates a bounded UTF-8 app label before any network operation', async () => {
  for (const name of ['', 'x'.repeat(65), 'bad\nname', 'bad\u0085name', '\ud800',
                      '😀'.repeat(17)]) {
    let fetchCalls = 0;
    FakeWebSocket.reset(() => {});
    const bridge = client({
      fetch: async () => {
        fetchCalls += 1;
        return response(HEALTH);
      },
    });
    assert.throws(() => bridge.pair(name), SyncConfigurationError, JSON.stringify(name));
    assert.equal(fetchCalls, 0);
    assert.equal(FakeWebSocket.instances.length, 0);
  }
});

test('pair performs health first and sends the exact request on a separate credentialless socket', async () => {
  const events = [];
  const permission = permissionScript(['prompt']);
  let pairingRequest;
  scriptedPairing({
    onPair(message, socket) {
      events.push('pair-send');
      pairingRequest = message;
      assert.equal(socket.protocols, undefined);
    },
  });
  const bridge = client({
    permissions: permission.permissions,
    fetch: async (url, options) => {
      events.push('health');
      assert.equal(url, `${ENDPOINT}/health`);
      assert.equal(options.credentials, 'omit');
      assert.equal(options.cache, 'no-store');
      assert.equal(options.redirect, 'error');
      assert.equal(options.targetAddressSpace, 'loopback');
      assert.deepEqual(options.headers, { Accept: 'application/json' });
      return response(HEALTH);
    },
  });

  assert.deepEqual(await bridge.pair('Noisedeck'), {
    protocolVersion: 1,
    token: PAIRED_TOKEN,
  });
  assert.deepEqual(events, ['health', 'pair-send']);
  assert.deepEqual(pairingRequest, {
    type: 'pair', protocolVersions: [1], name: 'Noisedeck',
  });
  assert.equal(permission.calls.length, 0, 'successful explicit health needs no passive query');
  const socket = FakeWebSocket.instances[0];
  assert.equal(socket.url.includes('Noisedeck'), false);
  assert.equal(socket.url.includes(PAIRED_TOKEN), false);
  assert.equal(socket.protocols, undefined);
  assert.equal(socket.listenerCount, 0);
  assert.equal(socket.closeCalls <= 1, true);
  bridge.close();
});

test('pair maps a failed health request to the post-failure permission state', async () => {
  for (const [state, ErrorClass, code] of [
    ['prompt', SyncPermissionRequiredError, SYNC_ERROR_CODE.PERMISSION_REQUIRED],
    ['denied', SyncPermissionDeniedError, SYNC_ERROR_CODE.PERMISSION_DENIED],
    ['granted', SyncUnavailableError, SYNC_ERROR_CODE.UNAVAILABLE],
  ]) {
    const events = [];
    const permission = {
      async query(descriptor) {
        events.push(`query:${descriptor.name}`);
        return { state };
      },
    };
    FakeWebSocket.reset(() => {});
    const bridge = client({
      permissions: permission,
      fetch: async () => {
        events.push('health');
        throw new TypeError('permission or daemon failure');
      },
    });
    await assert.rejects(bridge.pair('Noisedeck'), (error) => {
      assert.ok(error instanceof ErrorClass);
      assert.equal(error.code, code);
      return true;
    });
    assert.deepEqual(events, ['health', 'query:loopback-network']);
    assert.equal(FakeWebSocket.instances.length, 0);
  }
});

test('pair returns a capability without mutating the configured control token', async () => {
  let hello;
  FakeWebSocket.reset((socket) => {
    socket.onSend = (text) => {
      const message = JSON.parse(text);
      if (socket.url.endsWith('/pair')) {
        socket.message(JSON.stringify({
          type: 'paired', protocolVersion: 1, token: PAIRED_TOKEN,
        }));
        socket.remoteClose();
      } else {
        hello = message;
        socket.message(JSON.stringify(WELCOME));
      }
    };
    socket.open();
  });
  const bridge = client();
  const paired = await bridge.pair('Noisedeck');
  assert.equal(paired.token, PAIRED_TOKEN);
  await bridge.connect();
  assert.equal(hello.token, TOKEN);
  assert.equal(hello.token === paired.token, false);
  bridge.close();
});

test('pair maps every daemon outcome to a dedicated typed error and preserves daemonCode', async () => {
  const cases = [
    ['pairing_denied', SyncPairingDeniedError, SYNC_ERROR_CODE.PAIRING_DENIED],
    ['pairing_timeout', SyncTimeoutError, SYNC_ERROR_CODE.TIMEOUT],
    ['pairing_cooldown', SyncPairingBusyError, SYNC_ERROR_CODE.PAIRING_BUSY],
    ['prompt_saturated', SyncPairingBusyError, SYNC_ERROR_CODE.PAIRING_BUSY],
    ['store_failure', SyncPairingStoreError, SYNC_ERROR_CODE.PAIRING_STORE],
    ['store_durability_uncertain', SyncPairingDurabilityError,
      SYNC_ERROR_CODE.PAIRING_DURABILITY],
    ['origin_limit', SyncPairingOriginLimitError, SYNC_ERROR_CODE.PAIRING_ORIGIN_LIMIT],
    ['bad_request', SyncProtocolError, SYNC_ERROR_CODE.PROTOCOL],
    ['request_too_large', SyncProtocolError, SYNC_ERROR_CODE.PROTOCOL],
  ];
  for (const [daemonCode, ErrorClass, code] of cases) {
    scriptedPairing({ messages: [{
      type: 'error', code: daemonCode, message: `daemon ${daemonCode}`,
    }] });
    const bridge = client();
    await assert.rejects(bridge.pair('Noisedeck'), (error) => {
      assert.ok(error instanceof ErrorClass, daemonCode);
      assert.equal(error.code, code, daemonCode);
      assert.equal(error.daemonCode, daemonCode);
      return true;
    });
    assert.equal(FakeWebSocket.instances[0].listenerCount, 0);
  }
});

test('pair rejects malformed, binary, oversized, noncanonical, and duplicate responses', async () => {
  const invalidResponses = [
    ['{'],
    ['x'.repeat(1_025)],
    [new Uint8Array([1, 2, 3])],
    [{ type: 'paired', protocolVersion: 2, token: PAIRED_TOKEN }],
    [{ type: 'paired', protocolVersion: 1, token: 'A'.repeat(64) }],
    [{ type: 'paired', protocolVersion: 1, token: PAIRED_TOKEN, extra: true }],
    [
      { type: 'paired', protocolVersion: 1, token: PAIRED_TOKEN },
      { type: 'paired', protocolVersion: 1, token: 'b'.repeat(64) },
    ],
  ];
  for (const messages of invalidResponses) {
    FakeWebSocket.reset((socket) => {
      socket.onSend = () => {
        for (const message of messages) {
          socket.message(typeof message === 'string' || message instanceof Uint8Array
            ? message
            : JSON.stringify(message));
        }
        socket.remoteClose();
      };
      socket.open();
    });
    const bridge = client();
    await assert.rejects(bridge.pair('Noisedeck'), SyncProtocolError);
    assert.equal(FakeWebSocket.instances[0].listenerCount, 0);
  }
});

test('pair classifies early close, socket error, and local response timeout without lifecycle ambiguity', async () => {
  const cases = [
    {
      setup(socket) { socket.remoteClose(); },
      ErrorClass: SyncUnavailableError,
    },
    {
      setup(socket) { socket.fail(); },
      ErrorClass: SyncUnavailableError,
    },
    {
      setup(socket) { socket.open(); },
      ErrorClass: SyncTimeoutError,
    },
  ];
  for (const { setup, ErrorClass } of cases) {
    FakeWebSocket.reset(setup);
    const bridge = client({ pairingTimeoutMs: 5 });
    await assert.rejects(bridge.pair('Noisedeck'), ErrorClass);
    const socket = FakeWebSocket.instances[0];
    assert.equal(socket.listenerCount, 0);
    assert.equal(socket.closeCalls <= 1, true);
  }
});

test('pair owns CONNECTING and OPEN sockets and close cancels either state exactly once', async () => {
  for (const open of [false, true]) {
    FakeWebSocket.reset((socket) => {
      if (open) socket.open();
    });
    const bridge = client({ pairingTimeoutMs: 1_000 });
    const pairing = bridge.pair('Noisedeck');
    await flushUntil(() => FakeWebSocket.instances.length === 1, 'pairing socket construction');
    const socket = FakeWebSocket.instances[0];
    if (open) await flushUntil(() => socket.sent.length === 1, 'pairing request send');
    bridge.close();
    await assert.rejects(pairing, SyncLifecycleError);
    assert.equal(socket.closeCalls, 1);
    assert.equal(socket.listenerCount, 0);
    socket.message(JSON.stringify({
      type: 'paired', protocolVersion: 1, token: PAIRED_TOKEN,
    }));
    assert.equal(socket.listenerCount, 0);
  }
});

test('close aborts pending explicit health and removes its end-to-end timer immediately', async () => {
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  const timers = new Set();
  let requestSignal;
  try {
    globalThis.setTimeout = () => {
      const timer = {};
      timers.add(timer);
      return timer;
    };
    globalThis.clearTimeout = (timer) => timers.delete(timer);
    FakeWebSocket.reset(() => {});
    const bridge = client({
      pairingTimeoutMs: 1_000,
      fetch: async (_url, options) => {
        requestSignal = options.signal;
        return new Promise(() => {});
      },
    });
    const pairing = bridge.pair('Noisedeck');
    await flushUntil(() => requestSignal !== undefined, 'explicit health request');
    assert.equal(timers.size, 1);
    bridge.close();
    await assert.rejects(pairing, SyncLifecycleError);
    await flushUntil(() => timers.size === 0, 'explicit health timer cleanup');
    assert.equal(requestSignal.aborted, true);
    assert.equal(FakeWebSocket.instances.length, 0);
  } finally {
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
  }
});

test('pair rejects a concurrent attempt as pairing busy without disturbing the first', async () => {
  FakeWebSocket.reset((socket) => socket.open());
  const bridge = client({ pairingTimeoutMs: 1_000 });
  const first = bridge.pair('First');
  await flushUntil(() => FakeWebSocket.instances.length === 1, 'first pairing socket');
  await assert.rejects(bridge.pair('Second'), SyncPairingBusyError);
  assert.equal(FakeWebSocket.instances.length, 1);
  bridge.close();
  await assert.rejects(first, SyncLifecycleError);
});

test('pairingTimeoutMs is one end-to-end budget and also governs explicit health permission', async () => {
  const originalPerformance = globalThis.performance;
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  const nowValues = [0, 0, 20];
  const delays = [];
  try {
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: { now: () => nowValues.shift() ?? 20 },
    });
    globalThis.setTimeout = (callback, delay) => {
      delays.push(delay);
      const timer = { delay };
      if (delay === 15) queueMicrotask(callback);
      return timer;
    };
    globalThis.clearTimeout = () => {};
    FakeWebSocket.reset((socket) => socket.open());
    const bridge = client({ timeoutMs: 5, pairingTimeoutMs: 35 });
    await assert.rejects(bridge.pair('Noisedeck'), SyncTimeoutError);
    assert.deepEqual(delays, [35, 15],
      'health gets the human-scale budget and the socket receives only what remains');
    bridge.close();
  } finally {
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: originalPerformance,
    });
  }
});

test('pair checks the absolute deadline after successful health before WebSocket capability', async () => {
  const originalPerformance = globalThis.performance;
  let now = 0;
  let bridge;
  try {
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: { now: () => now },
    });
    FakeWebSocket.reset(() => {});
    bridge = client({
      pairingTimeoutMs: 35,
      WebSocket: null,
      fetch: async () => {
        now = 36;
        return response(HEALTH);
      },
    });
    await assert.rejects(bridge.pair('Noisedeck'), SyncTimeoutError);
    assert.equal(FakeWebSocket.instances.length, 0);
  } finally {
    bridge?.close();
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: originalPerformance,
    });
  }
});

test('pair rejects a response observed past its absolute deadline before the delayed timer callback', async () => {
  const originalPerformance = globalThis.performance;
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  const timers = [];
  let now = 0;
  let bridge;
  try {
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: { now: () => now },
    });
    globalThis.setTimeout = (callback, delay) => {
      const timer = { active: true, callback, delay };
      timers.push(timer);
      return timer;
    };
    globalThis.clearTimeout = (timer) => { timer.active = false; };
    FakeWebSocket.reset((socket) => {
      socket.onSend = () => {
        now = 36;
        socket.message(JSON.stringify({
          type: 'paired', protocolVersion: 1, token: PAIRED_TOKEN,
        }));
        socket.remoteClose();
      };
      socket.open();
    });
    bridge = client({ pairingTimeoutMs: 35 });
    const outcome = await outcomeWithinTurns(bridge.pair('Noisedeck'));
    assert.equal(outcome?.status, 'rejected');
    assert.ok(outcome.error instanceof SyncTimeoutError);
    const socket = FakeWebSocket.instances[0];
    assert.equal(socket.closeCalls, 1);
    assert.equal(socket.listenerCount, 0);
    assert.equal(timers.every((timer) => timer.active === false), true);
  } finally {
    bridge?.close();
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: originalPerformance,
    });
  }
});

test('pair lets the absolute deadline outrank a delayed socket send failure', async () => {
  const originalPerformance = globalThis.performance;
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  const timers = [];
  let now = 0;
  let bridge;
  try {
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: { now: () => now },
    });
    globalThis.setTimeout = (callback, delay) => {
      const timer = { active: true, callback, delay };
      timers.push(timer);
      return timer;
    };
    globalThis.clearTimeout = (timer) => { timer.active = false; };
    FakeWebSocket.reset((socket) => {
      socket.send = () => {
        now = 36;
        throw new Error('delayed send failure');
      };
      socket.open();
    });
    bridge = client({ pairingTimeoutMs: 35 });
    const outcome = await outcomeWithinTurns(bridge.pair('Noisedeck'));
    assert.equal(outcome?.status, 'rejected');
    assert.ok(outcome.error instanceof SyncTimeoutError);
    const socket = FakeWebSocket.instances[0];
    assert.equal(socket.closeCalls, 1);
    assert.equal(socket.listenerCount, 0);
    assert.equal(timers.every((timer) => timer.active === false), true);
  } finally {
    bridge?.close();
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: originalPerformance,
    });
  }
});

test('pair resolves neither a stored native error nor token when close crosses the absolute deadline', async () => {
  const originalPerformance = globalThis.performance;
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  const timers = [];
  let now = 0;
  let bridge;
  try {
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: { now: () => now },
    });
    globalThis.setTimeout = (callback, delay) => {
      const timer = { active: true, callback, delay };
      timers.push(timer);
      return timer;
    };
    globalThis.clearTimeout = (timer) => { timer.active = false; };
    FakeWebSocket.reset((socket) => {
      socket.onSend = () => {
        socket.message(JSON.stringify({
          type: 'error', code: 'pairing_denied', message: 'Pairing denied',
        }));
        now = 36;
        socket.remoteClose();
      };
      socket.open();
    });
    bridge = client({ pairingTimeoutMs: 35 });
    const outcome = await outcomeWithinTurns(bridge.pair('Noisedeck'));
    assert.equal(outcome?.status, 'rejected');
    assert.ok(outcome.error instanceof SyncTimeoutError);
    assert.equal(outcome.error.daemonCode, undefined);
    const socket = FakeWebSocket.instances[0];
    assert.equal(socket.listenerCount, 0);
    assert.equal(timers.every((timer) => timer.active === false), true);
  } finally {
    bridge?.close();
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: originalPerformance,
    });
  }
});

test('pair lets the absolute deadline outrank delayed invalid-response settlement', async () => {
  const originalPerformance = globalThis.performance;
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  const timers = [];
  let nowCalls = 0;
  let bridge;
  try {
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: { now: () => (++nowCalls >= 6 ? 36 : 0) },
    });
    globalThis.setTimeout = (callback, delay) => {
      const timer = { active: true, callback, delay };
      timers.push(timer);
      return timer;
    };
    globalThis.clearTimeout = (timer) => { timer.active = false; };
    FakeWebSocket.reset((socket) => {
      socket.onSend = () => socket.message(JSON.stringify({
        type: 'paired', protocolVersion: 2, token: PAIRED_TOKEN,
      }));
      socket.open();
    });
    bridge = client({ pairingTimeoutMs: 35 });
    const outcome = await outcomeWithinTurns(bridge.pair('Noisedeck'));
    assert.equal(outcome?.status, 'rejected');
    assert.ok(outcome.error instanceof SyncTimeoutError);
    const socket = FakeWebSocket.instances[0];
    assert.equal(socket.closeCalls, 1);
    assert.equal(socket.listenerCount, 0);
    assert.equal(timers.every((timer) => timer.active === false), true);
  } finally {
    bridge?.close();
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: originalPerformance,
    });
  }
});

test('pair classifies a permission result observed past the absolute deadline as timeout', async () => {
  const originalPerformance = globalThis.performance;
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  const timers = [];
  let now = 0;
  let resolvePermission;
  let bridge;
  try {
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: { now: () => now },
    });
    globalThis.setTimeout = (callback, delay) => {
      const timer = { active: true, callback, delay };
      timers.push(timer);
      return timer;
    };
    globalThis.clearTimeout = (timer) => { timer.active = false; };
    FakeWebSocket.reset(() => {});
    bridge = client({
      pairingTimeoutMs: 35,
      fetch: async () => { throw new TypeError('health permission failure'); },
      permissions: {
        query() {
          return new Promise((resolve) => { resolvePermission = resolve; });
        },
      },
    });
    const pairing = bridge.pair('Noisedeck');
    await flushUntil(() => resolvePermission !== undefined, 'post-health permission query');
    now = 36;
    resolvePermission({ state: 'prompt' });
    const outcome = await outcomeWithinTurns(pairing);
    assert.equal(outcome?.status, 'rejected');
    assert.ok(outcome.error instanceof SyncTimeoutError);
    assert.equal(FakeWebSocket.instances.length, 0);
    assert.equal(timers.every((timer) => timer.active === false), true);
  } finally {
    bridge?.close();
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: originalPerformance,
    });
  }
});

test('an expired post-health pairing phase never invokes the Permissions API', async () => {
  const originalPerformance = globalThis.performance;
  let now = 0;
  let queryCalls = 0;
  let bridge;
  try {
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: { now: () => now },
    });
    FakeWebSocket.reset(() => {});
    bridge = client({
      pairingTimeoutMs: 35,
      fetch: async () => {
        now = 36;
        throw new TypeError('health permission failure');
      },
      permissions: {
        async query() {
          queryCalls += 1;
          return { state: 'prompt' };
        },
      },
    });
    await assert.rejects(bridge.pair('Noisedeck'), SyncTimeoutError);
    assert.equal(queryCalls, 0);
    assert.equal(FakeWebSocket.instances.length, 0);
  } finally {
    bridge?.close();
    Object.defineProperty(globalThis, 'performance', {
      configurable: true,
      value: originalPerformance,
    });
  }
});

test('pairingTimeoutMs is independent and bounded', () => {
  for (const pairingTimeoutMs of [0, -1, 120_001, 1.5, Number.NaN]) {
    assert.throws(() => client({ pairingTimeoutMs }), SyncConfigurationError);
  }
  assert.doesNotThrow(() => client({ timeoutMs: 5, pairingTimeoutMs: 35_000 }));
});

test('connect opens only /control, sends the exact hello, and returns a validated welcome', async () => {
  let hello;
  scriptedControl({ onControl: (message) => { if (message.type === 'hello') hello = message; } });
  const bridge = client();

  assert.deepEqual(await bridge.connect(), WELCOME);
  assert.deepEqual(hello, { type: 'hello', token: TOKEN, protocolVersions: [1] });
  assert.equal(FakeWebSocket.instances.length, 1);
  assert.equal(FakeWebSocket.instances[0].url, 'ws://127.0.0.1:39079/control');
  assert.equal(FakeWebSocket.instances[0].protocols, undefined);
  assert.deepEqual(await bridge.connect(), WELCOME, 'connect is idempotent');
  bridge.close();
  bridge.close();
  assert.equal(FakeWebSocket.instances[0].closeCalls, 1);
});

test('connect classifies authentication and rejects malformed, oversized, early-close, and timed-out sessions', async () => {
  const scenarios = [
    {
      setup(socket) {
        socket.open();
        socket.onSend = () => socket.message(JSON.stringify({
          type: 'error', code: 'authentication_failed', message: 'Invalid token or protocol version',
        }));
      },
      ErrorClass: SyncAuthenticationError,
    },
    { setup(socket) { socket.open(); socket.onSend = () => socket.message('{'); }, ErrorClass: SyncProtocolError },
    {
      setup(socket) {
        socket.open();
        socket.onSend = () => socket.message('x'.repeat(16_385));
      },
      ErrorClass: SyncProtocolError,
    },
    {
      setup(socket) {
        socket.open();
        socket.onSend = () => socket.message(JSON.stringify({
          type: 'error', code: 'authentication_failed', message: 'no', extra: true,
        }));
      },
      ErrorClass: SyncProtocolError,
    },
    { setup(socket) { socket.remoteClose(); }, ErrorClass: Error },
    { setup() {}, ErrorClass: SyncTimeoutError },
  ];
  for (const { setup, ErrorClass } of scenarios) {
    FakeWebSocket.reset(setup);
    await assert.rejects(client({ timeoutMs: 5 }).connect(), ErrorClass);
    assert.equal(FakeWebSocket.instances[0].closeCalls <= 1, true);
  }
});

test('client close owns and immediately cancels a CONNECTING control socket', async () => {
  FakeWebSocket.reset(() => {});
  const bridge = client({ timeoutMs: 1_000 });
  const connecting = bridge.connect();
  await flushUntil(() => FakeWebSocket.instances.length === 1, 'control socket construction');
  const socket = FakeWebSocket.instances[0];
  assert.equal(socket.readyState, FakeWebSocket.CONNECTING);

  bridge.close();
  await assert.rejects(connecting, SyncLifecycleError);
  assert.equal(socket.closeCalls, 1);
  assert.equal(socket.listenerCount, 0);
});

test('createSender serializes control requests and uses a distinct ticket-subprotocol socket', async () => {
  const commands = [];
  scriptedControl({ onControl: (message) => commands.push(message) });
  const bridge = client();
  await bridge.connect();

  const firstPromise = bridge.createSender('First', {
    exportQueue: new ExportQueue(), maxBufferedBytes: 1024, clock: { timeOrigin: 0 },
  });
  const secondPromise = bridge.createSender('Second', {
    exportQueue: new ExportQueue(), maxBufferedBytes: 1024, clock: { timeOrigin: 0 },
  });
  const [first, second] = await Promise.all([firstPromise, secondPromise]);

  assert.deepEqual(commands.map(({ type }) => type), ['hello', 'createSender', 'createSender']);
  assert.equal(FakeWebSocket.instances.length, 3);
  for (const socket of FakeWebSocket.instances.slice(1)) {
    assert.equal(socket.url.includes('ticket'), false);
    assert.equal(socket.url.includes('aaaa'), false);
    assert.match(socket.protocols, /^sync\.sender\.[A-Za-z0-9._-]{32,128}$/);
    assert.notEqual(socket, FakeWebSocket.instances[0]);
  }
  assert.equal(first.submit('texture', 1), true);
  assert.equal(second.submit('texture', 2), true);
  first.close();
  second.close();
  await Promise.all([first.closed, second.closed]);
  assert.deepEqual(commands.map(({ type }) => type), [
    'hello', 'createSender', 'createSender', 'closeSender', 'closeSender',
  ]);
  bridge.close();
});

test('createSender forwards a live one-frame pressure policy to its real sink', async () => {
  scriptedControl();
  const bridge = client();
  await bridge.connect();
  const sender = await bridge.createSender('Resizable', {
    exportQueue: new ExportQueue(),
    maxBufferedFrames: 1,
    clock: { timeOrigin: 0 },
  });
  sender.configure({
    width: 2,
    height: 2,
    format: 'rgba8unorm',
    colorSpace: 'srgb',
    alphaMode: 'premultiplied',
    fps: 60,
  });
  FakeWebSocket.instances[1].bufferedAmount = 1;

  assert.equal(sender.submit('texture', 1), false);
  assert.equal(sender.stats.droppedBackpressure, 1);
  sender.close();
  await sender.closed;
  bridge.close();
});

test('concurrent sender creation keeps exactly one FIFO control request in flight', async () => {
  const commands = [];
  const replies = [];
  FakeWebSocket.reset((socket) => {
    if (!socket.url.endsWith('/control')) {
      socket.open(socket.protocols);
      return;
    }
    socket.open();
    socket.onSend = (text) => {
      const message = JSON.parse(text);
      commands.push(message);
      if (message.type === 'hello') socket.message(JSON.stringify(WELCOME));
      if (message.type === 'createSender') {
        const index = replies.length + 1;
        replies.push(() => socket.message(JSON.stringify({
          type: 'senderCreated', id: `sender_${index}`, name: message.name,
          path: `/senders/sender_${index}`, ticket: String(index).padStart(32, 'a'),
        })));
      }
    };
  });
  const bridge = client();
  await bridge.connect();
  const first = bridge.createSender('First', {
    exportQueue: new ExportQueue(), maxBufferedBytes: 1, clock: { timeOrigin: 0 },
  });
  const second = bridge.createSender('Second', {
    exportQueue: new ExportQueue(), maxBufferedBytes: 1, clock: { timeOrigin: 0 },
  });
  await Promise.resolve();
  await Promise.resolve();
  assert.deepEqual(commands.map(({ type, name }) => [type, name]), [
    ['hello', undefined], ['createSender', 'First'],
  ]);

  replies.shift()();
  await first;
  await Promise.resolve();
  assert.deepEqual(commands.map(({ type, name }) => [type, name]), [
    ['hello', undefined], ['createSender', 'First'], ['createSender', 'Second'],
  ]);
  replies.shift()();
  await second;
  bridge.close();
});

test('an unsolicited out-of-order control response terminates the session', async () => {
  scriptedControl();
  const bridge = client();
  await bridge.connect();
  const control = FakeWebSocket.instances[0];
  control.message(JSON.stringify({ type: 'senderClosed', id: 'sender_1' }));
  assert.equal(control.closeCalls, 1);
  assert.equal(bridge.connected, false);
});

test('an unexpected control close synchronously stops every active data sink', async () => {
  scriptedControl();
  const bridge = client();
  await bridge.connect();
  const sender = await bridge.createSender('Sender', {
    exportQueue: new ExportQueue(), maxBufferedBytes: 1, clock: { timeOrigin: 0 },
  });
  FakeWebSocket.instances[0].remoteClose();
  assert.equal(FakeWebSocket.instances[1].closeCalls, 1);
  assert.equal(sender.submit('texture', 0), false);
  await assert.rejects(sender.closed);
});

test('control loss cancels a post-allocation CONNECTING data socket in the same session', async () => {
  FakeWebSocket.reset((socket) => {
    if (!socket.url.endsWith('/control')) return;
    socket.open();
    socket.onSend = (text) => {
      const message = JSON.parse(text);
      if (message.type === 'hello') socket.message(JSON.stringify(WELCOME));
      if (message.type === 'createSender') socket.message(JSON.stringify({
        type: 'senderCreated', id: 'sender_1', name: message.name,
        path: '/senders/sender_1', ticket: 'a'.repeat(32),
      }));
    };
  });
  const bridge = client({ timeoutMs: 1_000 });
  await bridge.connect();
  const creation = bridge.createSender('Sender', {
    exportQueue: new ExportQueue(), maxBufferedBytes: 1, clock: { timeOrigin: 0 },
  });
  await flushUntil(() => FakeWebSocket.instances.length === 2, 'data socket construction');
  const data = FakeWebSocket.instances[1];
  assert.equal(data.readyState, FakeWebSocket.CONNECTING);

  FakeWebSocket.instances[0].remoteClose();
  await assert.rejects(creation, SyncLifecycleError);
  assert.equal(data.closeCalls, 1);
  assert.equal(data.listenerCount, 0);
  assert.equal(bridge.connected, false);
});

test('closeSender is not blocked behind an unrelated stalled data handshake', async () => {
  const commands = [];
  let createIndex = 0;
  FakeWebSocket.reset((socket) => {
    if (socket.url.endsWith('/control')) {
      socket.open();
      socket.onSend = (text) => {
        const message = JSON.parse(text);
        commands.push(message);
        if (message.type === 'hello') socket.message(JSON.stringify(WELCOME));
        if (message.type === 'createSender') {
          createIndex += 1;
          socket.message(JSON.stringify({
            type: 'senderCreated', id: `sender_${createIndex}`, name: message.name,
            path: `/senders/sender_${createIndex}`,
            ticket: String(createIndex).padStart(32, 'a'),
          }));
        }
        if (message.type === 'closeSender') {
          socket.message(JSON.stringify({ type: 'senderClosed', id: message.senderId }));
        }
      };
      return;
    }
    if (socket.url.endsWith('/senders/sender_1')) socket.open(socket.protocols);
  });
  const bridge = client({ timeoutMs: 1_000 });
  await bridge.connect();
  const first = await bridge.createSender('First', {
    exportQueue: new ExportQueue(), maxBufferedBytes: 1, clock: { timeOrigin: 0 },
  });
  const stalled = bridge.createSender('Stalled', {
    exportQueue: new ExportQueue(), maxBufferedBytes: 1, clock: { timeOrigin: 0 },
  });
  await flushUntil(() => FakeWebSocket.instances.length === 3, 'stalled data socket construction');

  first.close();
  await flushUntil(
    () => commands.some((message) => message.type === 'closeSender'),
    'closeSender while the second data socket remains CONNECTING',
  );
  assert.equal(FakeWebSocket.instances[2].readyState, FakeWebSocket.CONNECTING);
  await first.closed;
  bridge.close();
  await assert.rejects(stalled, SyncLifecycleError);
});

test('sender close is synchronous and idempotent while closed waits for the matching FIFO acknowledgement', async () => {
  let acknowledgeClose;
  FakeWebSocket.reset((socket) => {
    if (!socket.url.endsWith('/control')) {
      socket.open(socket.protocols);
      return;
    }
    socket.open();
    socket.onSend = (text) => {
      const message = JSON.parse(text);
      if (message.type === 'hello') socket.message(JSON.stringify(WELCOME));
      if (message.type === 'createSender') socket.message(JSON.stringify({
        type: 'senderCreated', id: 'sender_1', name: message.name,
        path: '/senders/sender_1', ticket: 'a'.repeat(32),
      }));
      if (message.type === 'closeSender') {
        acknowledgeClose = () => socket.message(JSON.stringify({
          type: 'senderClosed', id: 'sender_1',
        }));
      }
    };
  });
  const exportQueue = new ExportQueue();
  const bridge = client();
  await bridge.connect();
  const sender = await bridge.createSender('Sender', { exportQueue, maxBufferedBytes: 1024, clock: { timeOrigin: 0 } });
  const data = FakeWebSocket.instances[1];

  assert.equal(sender.close(), undefined);
  assert.equal(sender.close(), undefined);
  assert.equal(exportQueue.closeCalls, 1);
  assert.equal(data.closeCalls, 1);
  let settled = false;
  sender.closed.finally(() => { settled = true; });
  await Promise.resolve();
  assert.equal(settled, false);
  acknowledgeClose();
  await sender.closed;
  assert.equal(settled, true);
  bridge.close();
});

test('sender backendLost close forwards loss locally and still completes one control/data teardown', async () => {
  const commands = [];
  scriptedControl({ onControl: (message) => commands.push(message) });
  const exportQueue = new ExportQueue();
  const bridge = client();
  await bridge.connect();
  const sender = await bridge.createSender('Sender', {
    exportQueue, maxBufferedBytes: 1024, clock: { timeOrigin: 0 },
  });
  const data = FakeWebSocket.instances[1];
  const options = { backendLost: true };

  assert.equal(sender.close(options), undefined);
  assert.equal(sender.close(), undefined);
  await sender.closed;

  assert.deepEqual(exportQueue.closeOptions, [options]);
  assert.equal(data.closeCalls, 1);
  assert.deepEqual(commands.map(({ type }) => type), [
    'hello', 'createSender', 'closeSender',
  ]);
  bridge.close();
});

test('data connection failure performs bounded sender cleanup before create rejects', async () => {
  const commands = [];
  FakeWebSocket.reset((socket) => {
    if (socket.url.endsWith('/control')) {
      socket.open();
      socket.onSend = (text) => {
        const message = JSON.parse(text);
        commands.push(message);
        if (message.type === 'hello') socket.message(JSON.stringify(WELCOME));
        if (message.type === 'createSender') socket.message(JSON.stringify({
          type: 'senderCreated', id: 'sender_1', name: message.name,
          path: '/senders/sender_1', ticket: 'a'.repeat(32),
        }));
        if (message.type === 'closeSender') {
          socket.message(JSON.stringify({ type: 'senderClosed', id: message.senderId }));
        }
      };
    } else {
      socket.fail();
    }
  });
  const bridge = client();
  await bridge.connect();
  await assert.rejects(bridge.createSender('Sender', {
    exportQueue: new ExportQueue(), maxBufferedBytes: 1024, clock: { timeOrigin: 0 },
  }));
  assert.deepEqual(commands.map(({ type }) => type), ['hello', 'createSender', 'closeSender']);
  bridge.close();
});

test('an unexpected data-socket close releases the native sender through the control FIFO', async () => {
  const commands = [];
  scriptedControl({ onControl: (message) => commands.push(message) });
  const bridge = client();
  await bridge.connect();
  const sender = await bridge.createSender('Sender', {
    exportQueue: new ExportQueue(), maxBufferedBytes: 1, clock: { timeOrigin: 0 },
  });

  FakeWebSocket.instances[1].remoteClose();
  await sender.closed;
  assert.deepEqual(commands.map(({ type }) => type), ['hello', 'createSender', 'closeSender']);
  assert.equal(sender.submit('texture', 0), false);
  bridge.close();
});

test('strict sender validation rejects malformed replies, names, out-of-order responses, and the 65th active sender', async () => {
  scriptedControl({ createResponses: [{
    type: 'senderCreated', id: 'bad id', name: 'Sender', path: '/senders/bad id', ticket: 'a'.repeat(32),
  }] });
  const malformed = client();
  await malformed.connect();
  await assert.rejects(malformed.createSender('Sender', {
    exportQueue: new ExportQueue(), maxBufferedBytes: 1, clock: { timeOrigin: 0 },
  }), SyncProtocolError);

  for (const name of ['', 'x'.repeat(65), 'bad\nname', '\ud800']) {
    const bridge = client();
    assert.throws(() => bridge.createSender(name, {
      exportQueue: new ExportQueue(), maxBufferedBytes: 1, clock: { timeOrigin: 0 },
    }));
  }

  scriptedControl();
  const capped = client();
  await capped.connect();
  const senders = [];
  for (let index = 0; index < 64; index += 1) {
    senders.push(await capped.createSender(`Sender ${index}`, {
      exportQueue: new ExportQueue(), maxBufferedBytes: 1, clock: { timeOrigin: 0 },
    }));
  }
  await assert.rejects(capped.createSender('Too many', {
    exportQueue: new ExportQueue(), maxBufferedBytes: 1, clock: { timeOrigin: 0 },
  }));
  capped.close();
  assert.equal(senders.every((sender) => sender.submit('texture', 0) === false), true);
});

test('64 stalled data opens consume the complete sender reservation bound', async () => {
  let senderIndex = 0;
  FakeWebSocket.reset((socket) => {
    if (!socket.url.endsWith('/control')) return;
    socket.open();
    socket.onSend = (text) => {
      const message = JSON.parse(text);
      if (message.type === 'hello') socket.message(JSON.stringify(WELCOME));
      if (message.type === 'createSender') {
        senderIndex += 1;
        socket.message(JSON.stringify({
          type: 'senderCreated', id: `sender_${senderIndex}`, name: message.name,
          path: `/senders/sender_${senderIndex}`,
          ticket: String(senderIndex).padStart(32, 'a'),
        }));
      }
    };
  });
  const bridge = client({ timeoutMs: 1_000 });
  await bridge.connect();
  const creations = Array.from({ length: 64 }, (_, index) => bridge.createSender(
    `Pending ${index}`,
    { exportQueue: new ExportQueue(), maxBufferedBytes: 1, clock: { timeOrigin: 0 } },
  ));
  await flushUntil(
    () => FakeWebSocket.instances.length === 65,
    'all 64 bounded data socket opens',
    500,
  );
  await assert.rejects(bridge.createSender('Sixty fifth', {
    exportQueue: new ExportQueue(), maxBufferedBytes: 1, clock: { timeOrigin: 0 },
  }), SyncCapabilityError);
  assert.equal(FakeWebSocket.instances.slice(1).every(
    (socket) => socket.readyState === FakeWebSocket.CONNECTING,
  ), true);

  bridge.close();
  const outcomes = await Promise.allSettled(creations);
  assert.equal(outcomes.every(
    (outcome) => outcome.status === 'rejected' && outcome.reason instanceof SyncLifecycleError,
  ), true);
  assert.equal(FakeWebSocket.instances.slice(1).every(
    (socket) => socket.closeCalls === 1 && socket.listenerCount === 0,
  ), true);
});

test('client close immediately tears down data and control sockets and is safe during queued work', async () => {
  scriptedControl();
  const bridge = client();
  await bridge.connect();
  const sender = await bridge.createSender('Sender', {
    exportQueue: new ExportQueue(), maxBufferedBytes: 1, clock: { timeOrigin: 0 },
  });
  bridge.close();
  assert.equal(FakeWebSocket.instances[0].closeCalls, 1);
  assert.equal(FakeWebSocket.instances[1].closeCalls, 1);
  assert.equal(sender.submit('texture', 0), false);
  await assert.rejects(sender.closed);
});

test('the browser entry point exposes the client, sink, and protocol surface together', async () => {
  const entry = await import('../../browser/index.js');
  assert.equal(entry.SyncBridgeClient, SyncBridgeClient);
  assert.equal(typeof entry.SyncFrameSink, 'function');
  assert.equal(typeof entry.encodeFrameV1, 'function');
  assert.equal(typeof entry.SYNC_ERROR_CODE.PROTOCOL, 'string');
  assert.equal(entry.SYNC_DEFAULT_ENDPOINT, SYNC_DEFAULT_ENDPOINT);
  assert.equal(entry.SyncPairingDeniedError, SyncPairingDeniedError);
  assert.equal(entry.SyncPairingBusyError, SyncPairingBusyError);
  assert.equal(entry.SyncPairingStoreError, SyncPairingStoreError);
  assert.equal(entry.SyncPairingDurabilityError, SyncPairingDurabilityError);
  assert.equal(entry.SyncPairingOriginLimitError, SyncPairingOriginLimitError);
});
