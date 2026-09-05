import { SyncFrameSink } from './frame-sink.js';

const PROTOCOL_VERSION = 1;
const MAX_HEALTH_BYTES = 65_536;
const MAX_CONTROL_BYTES = 16_384;
const MAX_PAIRING_BYTES = 1_024;
const MAX_HEALTH_CHUNKS = 1_024;
const MAX_PROVIDERS = 4;
const MAX_SENDERS = 64;
const MAX_PROVIDER_ID_BYTES = 32;
const MAX_SENDER_NAME_BYTES = 64;
const MAX_SENDER_ID_BYTES = 128;
const MAX_TICKET_BYTES = 128;
const SOCKET_CONNECTING = 0;
const SOCKET_OPEN = 1;
const SOCKET_CLOSING = 2;
const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder('utf-8', { fatal: true });

export const SYNC_DEFAULT_ENDPOINT = 'http://127.0.0.1:53979';

export const SYNC_ERROR_CODE = Object.freeze({
  UNAVAILABLE: 'SYNC_UNAVAILABLE',
  TIMEOUT: 'SYNC_TIMEOUT',
  AUTHENTICATION: 'SYNC_AUTHENTICATION',
  PROTOCOL: 'SYNC_PROTOCOL',
  CAPABILITY: 'SYNC_CAPABILITY',
  LIFECYCLE: 'SYNC_LIFECYCLE',
  CONFIGURATION: 'SYNC_CONFIGURATION',
  PERMISSION_REQUIRED: 'SYNC_PERMISSION_REQUIRED',
  PERMISSION_DENIED: 'SYNC_PERMISSION_DENIED',
  PAIRING_DENIED: 'SYNC_PAIRING_DENIED',
  PAIRING_BUSY: 'SYNC_PAIRING_BUSY',
  PAIRING_STORE: 'SYNC_PAIRING_STORE',
  PAIRING_DURABILITY: 'SYNC_PAIRING_DURABILITY',
  PAIRING_ORIGIN_LIMIT: 'SYNC_PAIRING_ORIGIN_LIMIT',
  SENDER_LOST: 'SYNC_SENDER_LOST',
});

export class SyncBridgeError extends Error {
  constructor(message, { code = SYNC_ERROR_CODE.LIFECYCLE, cause, daemonCode } = {}) {
    super(message, cause === undefined ? undefined : { cause });
    this.name = new.target.name;
    this.code = code;
    if (daemonCode !== undefined) this.daemonCode = daemonCode;
  }
}

export class SyncUnavailableError extends SyncBridgeError {
  constructor(message = 'Sync is unavailable', options = {}) {
    super(message, { ...options, code: SYNC_ERROR_CODE.UNAVAILABLE });
  }
}

export class SyncTimeoutError extends SyncBridgeError {
  constructor(message = 'Sync operation timed out', options = {}) {
    super(message, { ...options, code: SYNC_ERROR_CODE.TIMEOUT });
  }
}

export class SyncAuthenticationError extends SyncBridgeError {
  constructor(message = 'Sync authentication failed', options = {}) {
    super(message, { ...options, code: SYNC_ERROR_CODE.AUTHENTICATION });
  }
}

export class SyncProtocolError extends SyncBridgeError {
  constructor(message = 'Sync protocol error', options = {}) {
    super(message, { ...options, code: SYNC_ERROR_CODE.PROTOCOL });
  }
}

export class SyncCapabilityError extends SyncBridgeError {
  constructor(message = 'Sync capability is unavailable', options = {}) {
    super(message, { ...options, code: SYNC_ERROR_CODE.CAPABILITY });
  }
}

export class SyncLifecycleError extends SyncBridgeError {
  constructor(message = 'Sync client is closed', options = {}) {
    super(message, { ...options, code: SYNC_ERROR_CODE.LIFECYCLE });
  }
}

export class SyncSenderLostError extends SyncBridgeError {
  constructor(message = 'Sync sender connection was lost', {
    closeCode = null,
    closeReason = '',
    cause,
  } = {}) {
    super(message, { cause, code: SYNC_ERROR_CODE.SENDER_LOST });
    this.closeCode = Number.isInteger(closeCode) && closeCode >= 0 && closeCode <= 65_535
      ? closeCode
      : null;
    this.closeReason = typeof closeReason === 'string' &&
      isWellFormedUnicode(closeReason) && utf8Length(closeReason) <= 123
      ? closeReason
      : '';
  }
}

export class SyncConfigurationError extends SyncBridgeError {
  constructor(message = 'Invalid Sync configuration', options = {}) {
    super(message, { ...options, code: SYNC_ERROR_CODE.CONFIGURATION });
  }
}

export class SyncPermissionRequiredError extends SyncBridgeError {
  constructor(message = 'Loopback network permission requires a user action', options = {}) {
    super(message, { ...options, code: SYNC_ERROR_CODE.PERMISSION_REQUIRED });
  }
}

export class SyncPermissionDeniedError extends SyncBridgeError {
  constructor(message = 'Loopback network permission was denied', options = {}) {
    super(message, { ...options, code: SYNC_ERROR_CODE.PERMISSION_DENIED });
  }
}

export class SyncPairingDeniedError extends SyncBridgeError {
  constructor(message = 'Sync pairing was denied', options = {}) {
    super(message, { ...options, code: SYNC_ERROR_CODE.PAIRING_DENIED });
  }
}

export class SyncPairingBusyError extends SyncBridgeError {
  constructor(message = 'Sync pairing is already in progress', options = {}) {
    super(message, { ...options, code: SYNC_ERROR_CODE.PAIRING_BUSY });
  }
}

export class SyncPairingStoreError extends SyncBridgeError {
  constructor(message = 'Sync could not store the pairing token', options = {}) {
    super(message, { ...options, code: SYNC_ERROR_CODE.PAIRING_STORE });
  }
}

export class SyncPairingDurabilityError extends SyncBridgeError {
  constructor(message = 'Sync could not confirm pairing token durability', options = {}) {
    super(message, { ...options, code: SYNC_ERROR_CODE.PAIRING_DURABILITY });
  }
}

export class SyncPairingOriginLimitError extends SyncBridgeError {
  constructor(message = 'Sync pairing origin limit was reached', options = {}) {
    super(message, { ...options, code: SYNC_ERROR_CODE.PAIRING_ORIGIN_LIMIT });
  }
}

function utf8Length(value) {
  return textEncoder.encode(value).byteLength;
}

function isWellFormedUnicode(value) {
  for (let index = 0; index < value.length; index += 1) {
    const code = value.charCodeAt(index);
    if (code >= 0xd800 && code <= 0xdbff) {
      const next = value.charCodeAt(++index);
      if (!(next >= 0xdc00 && next <= 0xdfff)) return false;
    } else if (code >= 0xdc00 && code <= 0xdfff) {
      return false;
    }
  }
  return true;
}

function exactKeys(value, expected, description) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    throw new SyncProtocolError(`${description} must be an object`);
  }
  const actual = Object.keys(value).sort();
  const wanted = [...expected].sort();
  if (actual.length !== wanted.length || actual.some((key, index) => key !== wanted[index])) {
    throw new SyncProtocolError(`${description} has unexpected fields`);
  }
}

function boundedString(value, maximumBytes, description, pattern) {
  if (typeof value !== 'string' || value.length === 0 || !isWellFormedUnicode(value) ||
      utf8Length(value) > maximumBytes ||
      (pattern && !pattern.test(value))) {
    throw new SyncProtocolError(`${description} is invalid`);
  }
  return value;
}

function validateCapabilities(capabilities) {
  exactKeys(capabilities, ['send', 'receive', 'providers'], 'capabilities');
  if (typeof capabilities.send !== 'boolean' || typeof capabilities.receive !== 'boolean' ||
      !Array.isArray(capabilities.providers) || capabilities.providers.length > MAX_PROVIDERS) {
    throw new SyncProtocolError('capabilities are invalid');
  }

  const identities = new Set();
  let canSend = false;
  let canReceive = false;
  for (const provider of capabilities.providers) {
    exactKeys(provider, ['id', 'direction', 'available', 'selected'], 'provider capability');
    const id = boundedString(provider.id, MAX_PROVIDER_ID_BYTES, 'provider id');
    if (provider.direction !== 'send' && provider.direction !== 'receive') {
      throw new SyncProtocolError('provider direction is invalid');
    }
    if (typeof provider.available !== 'boolean' || typeof provider.selected !== 'boolean') {
      throw new SyncProtocolError('provider state is invalid');
    }
    const identity = `${provider.direction}:${id}`;
    if (identities.has(identity)) throw new SyncProtocolError('provider capability is duplicated');
    identities.add(identity);
    if (provider.available && provider.selected) {
      if (provider.direction === 'send') canSend = true;
      else canReceive = true;
    }
  }
  if (capabilities.send !== canSend || capabilities.receive !== canReceive) {
    throw new SyncProtocolError('aggregate capabilities do not match providers');
  }
  return capabilities;
}

function validateInstance(value) {
  return boundedString(value, 128, 'instance id', /^[A-Za-z0-9_-]+$/);
}

function validateVersion(value) {
  return boundedString(value, 64, 'product version', /^[\x20-\x7e]+$/);
}

function validateHealth(value) {
  exactKeys(value, [
    'product', 'status', 'version', 'protocolVersions', 'instanceId', 'capabilities',
  ], 'health response');
  if (value.product !== 'Sync' || value.status !== 'ok') {
    throw new SyncProtocolError('health identity is invalid');
  }
  validateVersion(value.version);
  validateInstance(value.instanceId);
  if (!Array.isArray(value.protocolVersions) || value.protocolVersions.length === 0 ||
      value.protocolVersions.length > 16) {
    throw new SyncProtocolError('protocol versions are invalid');
  }
  const versions = new Set();
  for (const version of value.protocolVersions) {
    if (!Number.isSafeInteger(version) || version <= 0 || version > 65_535 || versions.has(version)) {
      throw new SyncProtocolError('protocol versions are invalid');
    }
    versions.add(version);
  }
  if (!versions.has(PROTOCOL_VERSION)) {
    throw new SyncProtocolError('daemon does not support Sync protocol v1');
  }
  validateCapabilities(value.capabilities);
  return value;
}

function validateWelcome(value) {
  exactKeys(value, [
    'type', 'protocolVersion', 'version', 'instanceId', 'capabilities',
  ], 'welcome response');
  if (value.type !== 'welcome' || value.protocolVersion !== PROTOCOL_VERSION) {
    throw new SyncProtocolError('welcome protocol version is invalid');
  }
  validateVersion(value.version);
  validateInstance(value.instanceId);
  validateCapabilities(value.capabilities);
  return value;
}

function validatePaired(value) {
  exactKeys(value, ['type', 'protocolVersion', 'token'], 'paired response');
  if (value.type !== 'paired' || value.protocolVersion !== PROTOCOL_VERSION ||
      typeof value.token !== 'string' || !/^[a-f0-9]{64}$/.test(value.token)) {
    throw new SyncProtocolError('paired response is invalid');
  }
  return { protocolVersion: value.protocolVersion, token: value.token };
}

function validateSenderCreated(value, requestedName) {
  exactKeys(value, ['type', 'id', 'name', 'path', 'ticket'], 'senderCreated response');
  if (value.type !== 'senderCreated' || value.name !== requestedName) {
    throw new SyncProtocolError('senderCreated identity is invalid');
  }
  const id = boundedString(value.id, MAX_SENDER_ID_BYTES, 'sender id', /^[A-Za-z0-9_-]+$/);
  if (value.path !== `/senders/${id}`) {
    throw new SyncProtocolError('sender data path is invalid');
  }
  boundedString(value.ticket, MAX_TICKET_BYTES, 'sender ticket', /^[A-Za-z0-9._-]{32,128}$/);
  return value;
}

function validateSenderClosed(value, id) {
  exactKeys(value, ['type', 'id'], 'senderClosed response');
  if (value.type !== 'senderClosed' || value.id !== id) {
    throw new SyncProtocolError('senderClosed identity is invalid');
  }
  return value;
}

function daemonError(value) {
  if (!value || typeof value !== 'object' || value.type !== 'error') return null;
  exactKeys(value, ['type', 'code', 'message'], 'error response');
  boundedString(value.code, 64, 'daemon error code', /^[a-z0-9_]+$/);
  boundedString(value.message, 256, 'daemon error message');
  const options = { daemonCode: value.code };
  if (value.code === 'authentication_failed') {
    return new SyncAuthenticationError(value.message, options);
  }
  if (value.code === 'publisher_unavailable' || value.code === 'sender_limit') {
    return new SyncCapabilityError(value.message, options);
  }
  if (value.code === 'duplicate_sender') {
    return new SyncCapabilityError(value.message, options);
  }
  if (value.code === 'bad_request' || value.code === 'out_of_order') {
    return new SyncProtocolError(value.message, options);
  }
  if (value.code === 'internal_error') {
    return new SyncUnavailableError(value.message, options);
  }
  return new SyncLifecycleError(value.message, options);
}

function pairingDaemonError(value) {
  if (!value || typeof value !== 'object' || value.type !== 'error') return null;
  exactKeys(value, ['type', 'code', 'message'], 'pairing error response');
  boundedString(value.code, 64, 'pairing error code', /^[a-z0-9_]+$/);
  boundedString(value.message, 256, 'pairing error message');
  const options = { daemonCode: value.code };
  if (value.code === 'pairing_denied') {
    return new SyncPairingDeniedError(value.message, options);
  }
  if (value.code === 'pairing_timeout') {
    return new SyncTimeoutError(value.message, options);
  }
  if (value.code === 'pairing_cooldown' || value.code === 'prompt_saturated' ||
      value.code === 'authority_saturated') {
    return new SyncPairingBusyError(value.message, options);
  }
  if (value.code === 'store_failure') {
    return new SyncPairingStoreError(value.message, options);
  }
  if (value.code === 'store_durability_uncertain') {
    return new SyncPairingDurabilityError(value.message, options);
  }
  if (value.code === 'origin_limit') {
    return new SyncPairingOriginLimitError(value.message, options);
  }
  if (value.code === 'prompt_failure' || value.code === 'internal_error') {
    return new SyncUnavailableError(value.message, options);
  }
  return new SyncProtocolError(value.message, options);
}

function normalizeEndpoint(endpoint) {
  if (typeof endpoint !== 'string' || endpoint.length > 256) {
    throw new SyncConfigurationError('endpoint must be an explicit loopback HTTP URL');
  }
  const match = /^http:\/\/([^/?#]+)\/?$/.exec(endpoint);
  if (!match) throw new SyncConfigurationError('endpoint must be an explicit loopback HTTP URL');

  const authority = match[1];
  const authorityMatch = authority.startsWith('[')
    ? /^\[([^\]]+)\]:(\d+)$/.exec(authority)
    : /^([^:]+):(\d+)$/.exec(authority);
  if (!authorityMatch) throw new SyncConfigurationError('endpoint must include a concrete port');
  const port = Number(authorityMatch[2]);
  if (!Number.isInteger(port) || port < 1 || port > 65_535) {
    throw new SyncConfigurationError('endpoint port is invalid');
  }

  let parsed;
  try {
    parsed = new URL(endpoint);
  } catch (cause) {
    throw new SyncConfigurationError('endpoint URL is invalid', { cause });
  }
  if (parsed.protocol !== 'http:' || parsed.username || parsed.password || parsed.search ||
      parsed.hash || parsed.pathname !== '/') {
    throw new SyncConfigurationError('endpoint must be a bare loopback HTTP origin');
  }

  const hostname = parsed.hostname;
  const bracketed = hostname.startsWith('[') && hostname.endsWith(']');
  const bareHostname = bracketed ? hostname.slice(1, -1) : hostname;
  const ipv4 = bareHostname.split('.');
  const isIpv4Loopback = ipv4.length === 4 &&
    ipv4.every((part) => /^\d{1,3}$/.test(part) && Number(part) <= 255) &&
    Number(ipv4[0]) === 127;
  const isIpv6Loopback = bareHostname.toLowerCase() === '::1';
  if (!isIpv4Loopback && !isIpv6Loopback) {
    throw new SyncConfigurationError('endpoint host must be an IPv4 or IPv6 loopback literal');
  }

  const host = isIpv6Loopback ? '[::1]' : ipv4.map(Number).join('.');
  const httpOrigin = `http://${host}:${port}`;
  const wsOrigin = `ws://${host}:${port}`;
  return Object.freeze({ httpOrigin, wsOrigin });
}

function validateToken(token) {
  if (token === undefined) return;
  if (typeof token !== 'string' || token.length === 0 || utf8Length(token) > 256 ||
      !/^[\x20-\x7e]+$/.test(token)) {
    throw new SyncConfigurationError('token must be 1-256 printable ASCII bytes');
  }
}

function validateTimeout(timeoutMs) {
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 60_000) {
    throw new SyncConfigurationError('timeoutMs must be an integer from 1 to 60000');
  }
}

function validatePairingTimeout(pairingTimeoutMs) {
  if (!Number.isSafeInteger(pairingTimeoutMs) || pairingTimeoutMs < 1 || pairingTimeoutMs > 120_000) {
    throw new SyncConfigurationError('pairingTimeoutMs must be an integer from 1 to 120000');
  }
}

function isUnsupportedPermissionError(error) {
  return error instanceof TypeError || error?.name === 'NotSupportedError';
}

function permissionState(value) {
  if (value === 'granted' || value === 'prompt' || value === 'denied') return value;
  throw new SyncUnavailableError('Loopback permission state is unavailable');
}

// Labels reach surfaces the user reads to make a decision: the pairing name in
// the daemon's native trust prompt, and the sender name in other applications'
// source pickers. The daemon rejects invisible and bidirectional formatting
// characters in both; reject them here too so callers see why rather than a
// generic bad_request.
const FORMATTING_CHARACTERS =
  /[\u200b-\u200f\u202a-\u202e\u2060-\u2064\u2066-\u2069\ufeff]/u;

function validateSenderName(name) {
  if (typeof name !== 'string' || name.length === 0 || !isWellFormedUnicode(name) ||
      utf8Length(name) > MAX_SENDER_NAME_BYTES ||
      /[\u0000-\u001f\u007f-\u009f]/u.test(name) ||
      FORMATTING_CHARACTERS.test(name)) {
    throw new SyncConfigurationError(
      'sender name must be 1-64 UTF-8 bytes without control or formatting characters',
    );
  }
}


function validatePairingName(name) {
  if (typeof name !== 'string' || name.length === 0 || !isWellFormedUnicode(name) ||
      utf8Length(name) > 64 || /[\u0000-\u001f\u007f-\u009f]/u.test(name) ||
      FORMATTING_CHARACTERS.test(name)) {
    throw new SyncConfigurationError(
      'pairing name must be 1-64 UTF-8 bytes without control or formatting characters',
    );
  }
}

function validateSinkOptions({
  exportQueue,
  maxBufferedBytes,
  maxBufferedFrames,
  clock = globalThis.performance,
} = {}) {
  if (!exportQueue || typeof exportQueue.available !== 'boolean' ||
      !['configure', 'enqueue', 'poll', 'close'].every((method) => typeof exportQueue[method] === 'function')) {
    throw new SyncConfigurationError('exportQueue is invalid');
  }
  const hasByteBudget = maxBufferedBytes !== undefined;
  const hasFrameBudget = maxBufferedFrames !== undefined;
  if (hasByteBudget === hasFrameBudget) {
    throw new SyncConfigurationError(
      'exactly one buffered-byte or buffered-frame limit is required',
    );
  }
  if (hasByteBudget &&
      (!Number.isSafeInteger(maxBufferedBytes) || maxBufferedBytes <= 0)) {
    throw new SyncConfigurationError('maxBufferedBytes must be a positive safe integer');
  }
  if (hasFrameBudget &&
      (!Number.isSafeInteger(maxBufferedFrames) || maxBufferedFrames <= 0)) {
    throw new SyncConfigurationError('maxBufferedFrames must be a positive safe integer');
  }
  if (!clock || typeof clock.timeOrigin !== 'number' || !Number.isFinite(clock.timeOrigin) || clock.timeOrigin < 0) {
    throw new SyncConfigurationError('clock.timeOrigin must be finite and non-negative');
  }
  return { exportQueue, maxBufferedBytes, maxBufferedFrames, clock };
}

function runWithTimeout(operation, timeoutMs, description, onTimeout, signal) {
  let timer;
  let aborted;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => {
      reject(new SyncTimeoutError(`${description} timed out`));
      try { onTimeout?.(); } catch {}
    }, timeoutMs);
  });
  const candidates = [operation, timeout];
  if (signal) {
    candidates.push(new Promise((_, reject) => {
      aborted = () => reject(new SyncLifecycleError(`${description} was canceled`));
      if (signal.aborted) aborted();
      else signal.addEventListener('abort', aborted, { once: true });
    }));
  }
  return Promise.race(candidates).finally(() => {
    clearTimeout(timer);
    if (aborted) signal.removeEventListener('abort', aborted);
  });
}

function monotonicNow() {
  return typeof globalThis.performance?.now === 'function'
    ? globalThis.performance.now()
    : Date.now();
}

async function readBoundedJson(response, maximumBytes) {
  const contentType = response.headers?.get?.('content-type');
  if (typeof contentType !== 'string' || !/^application\/json(?:\s*;|$)/i.test(contentType)) {
    throw new SyncProtocolError('health response is not JSON');
  }
  const declared = response.headers?.get?.('content-length');
  if (declared !== null && declared !== undefined) {
    if (!/^\d+$/.test(declared) || Number(declared) > maximumBytes) {
      throw new SyncProtocolError('health response exceeds the size limit');
    }
  }
  const reader = response.body?.getReader?.();
  if (!reader) throw new SyncProtocolError('health response body is not streamable');
  const chunks = [];
  let total = 0;
  let chunkCount = 0;
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      if (!(value instanceof Uint8Array)) throw new SyncProtocolError('health response bytes are invalid');
      chunkCount += 1;
      if (chunkCount > MAX_HEALTH_CHUNKS) {
        await reader.cancel();
        throw new SyncProtocolError('health response has too many chunks');
      }
      total += value.byteLength;
      if (total > maximumBytes) {
        await reader.cancel();
        throw new SyncProtocolError('health response exceeds the size limit');
      }
      chunks.push(value);
    }
  } finally {
    reader.releaseLock?.();
  }
  const bytes = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, offset);
    offset += chunk.byteLength;
  }
  let text;
  try {
    text = textDecoder.decode(bytes);
  } catch (cause) {
    throw new SyncProtocolError('health response is not valid UTF-8', { cause });
  }
  try {
    return JSON.parse(text);
  } catch (cause) {
    throw new SyncProtocolError('health response is not valid JSON', { cause });
  }
}

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  promise.catch(() => {});
  return { promise, resolve, reject };
}

class SyncSenderSink {
  constructor(client, session, id, frameSink, dataSocket) {
    this.id = id;
    this._client = client;
    this._session = session;
    this._frameSink = frameSink;
    this._closed = false;
    this._completion = deferred();
    this.closed = this._completion.promise;
    this._dataSocket = dataSocket;
    this._finalStats = null;
    this._dataEnded = (event) => this._remoteEnd(event);
    dataSocket.addEventListener('error', this._dataEnded);
    dataSocket.addEventListener('close', this._dataEnded);
  }

  get stats() {
    return this._frameSink === null ? this._finalStats : this._frameSink.stats;
  }

  configure(descriptor) {
    if (this._closed) throw new SyncLifecycleError('sender is closed');
    return this._frameSink.configure(descriptor);
  }

  submit(texture, timestamp) {
    if (this._closed) return false;
    return this._frameSink.submit(texture, timestamp);
  }

  close(options) {
    if (this._closed) return;
    this._closed = true;
    let localError;
    try {
      this._closeLocal(options);
    } catch (error) {
      localError = error;
    }
    this._client._requestSenderClose(this).then(
      (value) => this._completion.resolve(value),
      (error) => this._completion.reject(error),
    );
    if (localError) throw localError;
  }

  _abort(error) {
    if (!this._closed) {
      this._closed = true;
      try { this._closeLocal(); } catch {}
    }
    this._completion.reject(error);
  }

  _closeLocal(options) {
    // Drop the transport and sink once closed. A host that keeps this object
    // around (last sender, error cause, UI state) must not keep the export
    // queue's GPU staging and two sockets alive with it.
    const dataSocket = this._dataSocket;
    const frameSink = this._frameSink;
    this._dataSocket = null;
    this._frameSink = null;
    // Keep the sink's own counter object: a host that read `stats` before
    // close still holds the live object, and a late completion the sink
    // counts after close is still visible.
    this._finalStats = frameSink.stats;
    dataSocket.removeEventListener('error', this._dataEnded);
    dataSocket.removeEventListener('close', this._dataEnded);
    frameSink.close(options);
  }

  _remoteEnd(event) {
    if (this._closed) return;
    this._closed = true;
    const error = new SyncSenderLostError(undefined, {
      closeCode: event?.code,
      closeReason: event?.reason,
    });
    // A transport failure leaves the renderer backend alive. Destroy its
    // export resources normally; only renderer-driven close may abandon them.
    try { this._closeLocal(); } catch {}
    this._completion.reject(error);
    try {
      this._client._requestSenderClose(this).catch(() => {});
    } catch {}
  }
}

export class SyncBridgeClient {
  constructor({
    endpoint = SYNC_DEFAULT_ENDPOINT,
    token,
    fetch: fetchImplementation = globalThis.fetch,
    WebSocket: WebSocketImplementation = globalThis.WebSocket,
    permissions = globalThis.navigator?.permissions,
    timeoutMs = 3_000,
    pairingTimeoutMs = 35_000,
    maxHealthBytes = MAX_HEALTH_BYTES,
    maxControlMessageBytes = MAX_CONTROL_BYTES,
  } = {}) {
    this._endpoint = normalizeEndpoint(endpoint);
    validateToken(token);
    validateTimeout(timeoutMs);
    validatePairingTimeout(pairingTimeoutMs);
    if (!Number.isSafeInteger(maxHealthBytes) || maxHealthBytes < 1 || maxHealthBytes > MAX_HEALTH_BYTES) {
      throw new SyncConfigurationError(`maxHealthBytes must be from 1 to ${MAX_HEALTH_BYTES}`);
    }
    if (!Number.isSafeInteger(maxControlMessageBytes) || maxControlMessageBytes < 1 ||
        maxControlMessageBytes > MAX_CONTROL_BYTES) {
      throw new SyncConfigurationError(`maxControlMessageBytes must be from 1 to ${MAX_CONTROL_BYTES}`);
    }
    this._token = token;
    this._fetch = fetchImplementation;
    this._WebSocket = WebSocketImplementation;
    this._permissions = permissions;
    this._timeoutMs = timeoutMs;
    this._pairingTimeoutMs = pairingTimeoutMs;
    this._maxHealthBytes = maxHealthBytes;
    this._maxControlMessageBytes = maxControlMessageBytes;
    this._controlSocket = null;
    this._controlSession = null;
    this._nextControlGeneration = 1;
    this._controlHandlers = null;
    this._controlPending = null;
    this._connectPromise = null;
    this._pairingAttempt = null;
    this._passiveAttempts = new Set();
    this._welcome = null;
    this._controlQueue = Promise.resolve();
    this._pendingOpens = new Set();
    this._senders = new Set();
    this._senderReservations = 0;
    this._closed = false;
  }

  get connected() {
    return this._welcome !== null && !this._closed;
  }

  get welcome() {
    return this._welcome;
  }

  async probe() {
    let attempt;
    try {
      attempt = this._beginPassiveAttempt();
      await this._enforcePassivePermission(attempt);
      if (this._closed) throw new SyncLifecycleError();
      const health = await this._requestHealth(attempt.controller);
      return { available: true, health };
    } catch (error) {
      return this._unavailable(error);
    } finally {
      if (attempt) this._finishPassiveAttempt(attempt);
    }
  }

  pair(name) {
    validatePairingName(name);
    if (this._closed) return Promise.reject(new SyncLifecycleError());
    if (this._pairingAttempt) {
      return Promise.reject(new SyncPairingBusyError());
    }

    const cancellation = deferred();
    const attempt = {
      controller: new AbortController(),
      canceled: false,
      cancelSocket: null,
      cancel: null,
      deadline: monotonicNow() + this._pairingTimeoutMs,
    };
    attempt.cancel = (error) => {
      if (attempt.canceled) return;
      attempt.canceled = true;
      attempt.controller.abort();
      attempt.cancelSocket?.(error);
      cancellation.reject(error);
    };
    this._pairingAttempt = attempt;

    const operation = this._performPair(name, attempt);
    operation.catch(() => {});
    const tracked = Promise.race([operation, cancellation.promise]).finally(() => {
      attempt.controller.abort();
      if (this._pairingAttempt === attempt) this._pairingAttempt = null;
    });
    return tracked;
  }

  connect() {
    if (this._closed) return Promise.reject(new SyncLifecycleError());
    if (this._welcome) return Promise.resolve(this._welcome);
    if (this._connectPromise) return this._connectPromise;
    if (this._token === undefined) {
      return Promise.reject(new SyncConfigurationError('token is required to connect'));
    }
    if (typeof this._WebSocket !== 'function') {
      return Promise.reject(new SyncUnavailableError('WebSocket is unavailable'));
    }

    const attempt = this._connect();
    const tracked = attempt.catch((error) => {
      if (this._connectPromise === tracked) this._connectPromise = null;
      throw error;
    });
    this._connectPromise = tracked;
    return tracked;
  }

  createSender(name, options) {
    validateSenderName(name);
    const sinkOptions = validateSinkOptions(options);
    if (this._closed) return Promise.reject(new SyncLifecycleError());
    if (this._senders.size + this._senderReservations >= MAX_SENDERS) {
      return Promise.reject(new SyncCapabilityError(`Sync protocol v1 permits at most ${MAX_SENDERS} senders`));
    }
    this._senderReservations += 1;
    return (async () => {
      if (!this._welcome) await this.connect();
      const session = this._controlSession;
      this._assertSession(session);
      if (!session.welcome.capabilities.send) {
        throw new SyncCapabilityError('daemon has no selected send provider');
      }

      const created = await this._scheduleControl(
        session,
        () => this._exchange(
          { type: 'createSender', name },
          (message) => validateSenderCreated(message, name),
          session,
        ),
      );
      let dataSocket;
      const dataProtocol = `sync.sender.${created.ticket}`;
      try {
        dataSocket = await this._openSocket(
          `${this._endpoint.wsOrigin}${created.path}`,
          dataProtocol,
          'sender data connection',
          session,
        );
        if (dataSocket.protocol !== dataProtocol) {
          throw new SyncProtocolError('sender data subprotocol was not negotiated');
        }
        this._assertSession(session);
        const frameSink = new SyncFrameSink({ socket: dataSocket, ...sinkOptions });
        const sender = new SyncSenderSink(this, session, created.id, frameSink, dataSocket);
        this._senders.add(sender);
        return sender;
      } catch (error) {
        try { dataSocket?.close(); } catch {}
        if (this._isSessionActive(session)) {
          try {
            await this._scheduleControl(
              session,
              () => this._exchange(
                { type: 'closeSender', senderId: created.id },
                (message) => validateSenderClosed(message, created.id),
                session,
              ),
            );
          } catch (cleanupError) {
            if (error && typeof error === 'object' && error.cause === undefined) {
              try { error.cause = cleanupError; } catch {}
            }
          }
        }
        throw error;
      }
    })().finally(() => {
      this._senderReservations -= 1;
    });
  }

  close() {
    if (this._closed) return;
    this._closed = true;
    const error = new SyncLifecycleError('Sync client was closed');
    this._pairingAttempt?.cancel(error);
    for (const attempt of this._passiveAttempts) attempt.controller.abort();
    this._cancelPendingOpens(error);
    for (const sender of [...this._senders]) sender._abort(error);
    this._senders.clear();
    this._terminateControl(error, true, this._controlSession);
  }

  _unavailable(error) {
    const normalized = error instanceof SyncBridgeError
      ? error
      : new SyncUnavailableError('Sync is unavailable', { cause: error });
    return { available: false, code: normalized.code, message: normalized.message };
  }

  async _requestHealth(controller, timeoutMs = this._timeoutMs) {
    if (typeof this._fetch !== 'function') {
      throw new SyncUnavailableError('Fetch is unavailable');
    }
    return runWithTimeout((async () => {
      let response;
      // Call the implementation without a receiver: a browser's global fetch
      // rejects with "Illegal invocation" when invoked as a method of anything
      // but its global, and the SDK's default is exactly that global function.
      const fetchImplementation = this._fetch;
      try {
        response = await fetchImplementation(`${this._endpoint.httpOrigin}/health`, {
          method: 'GET',
          credentials: 'omit',
          cache: 'no-store',
          redirect: 'error',
          targetAddressSpace: 'loopback',
          headers: { Accept: 'application/json' },
          signal: controller.signal,
        });
      } catch (cause) {
        throw new SyncUnavailableError('Sync daemon did not answer', { cause });
      }
      if (!response || response.status !== 200 || response.ok !== true) {
        throw new SyncUnavailableError(`Sync health request failed with HTTP ${response?.status ?? 'unknown'}`);
      }
      return validateHealth(await readBoundedJson(response, this._maxHealthBytes));
    })(), timeoutMs, 'Sync health request', () => controller.abort(), controller.signal);
  }

  async _performPair(name, attempt) {
    try {
      await this._requestHealth(attempt.controller, this._remainingPairingTime(attempt));
    } catch (error) {
      if (attempt.canceled || this._closed) throw new SyncLifecycleError();
      if (error instanceof SyncTimeoutError) throw error;
      const permissionTimeoutMs = this._remainingPairingTime(attempt);
      const permissionQuery = this._queryLoopbackPermission(
        attempt.controller.signal,
        () => this._remainingPairingTime(attempt),
      );
      const state = await runWithTimeout(
        permissionQuery,
        permissionTimeoutMs,
        'Sync pairing permission query',
        () => attempt.controller.abort(),
        attempt.controller.signal,
      );
      if (attempt.canceled || this._closed) throw new SyncLifecycleError();
      this._remainingPairingTime(attempt);
      if (state === 'prompt') throw new SyncPermissionRequiredError();
      if (state === 'denied') throw new SyncPermissionDeniedError();
      throw error instanceof SyncBridgeError
        ? error
        : new SyncUnavailableError('Sync daemon did not answer', { cause: error });
    }
    if (attempt.canceled || this._closed) throw new SyncLifecycleError();
    this._remainingPairingTime(attempt);
    if (typeof this._WebSocket !== 'function') {
      throw new SyncUnavailableError('WebSocket is unavailable');
    }
    return this._exchangePair(name, attempt);
  }

  _remainingPairingTime(attempt) {
    const remaining = Math.ceil(attempt.deadline - monotonicNow());
    if (remaining < 1) throw new SyncTimeoutError('Sync pairing timed out');
    return remaining;
  }

  _exchangePair(name, attempt) {
    let timeoutMs;
    try {
      timeoutMs = this._remainingPairingTime(attempt);
    } catch (error) {
      return Promise.reject(error);
    }
    let socket;
    try {
      socket = new this._WebSocket(`${this._endpoint.wsOrigin}/pair`);
    } catch (cause) {
      if (attempt.canceled || this._closed) {
        return Promise.reject(new SyncLifecycleError());
      }
      try {
        this._remainingPairingTime(attempt);
      } catch (error) {
        return Promise.reject(error);
      }
      return Promise.reject(new SyncUnavailableError('pairing connection could not be created', { cause }));
    }

    return new Promise((resolve, reject) => {
      let settled = false;
      let response;
      let responseError;
      let timer;

      const cleanup = () => {
        clearTimeout(timer);
        socket.removeEventListener('open', opened);
        socket.removeEventListener('message', message);
        socket.removeEventListener('error', failed);
        socket.removeEventListener('close', closed);
        if (attempt.cancelSocket === cancel) attempt.cancelSocket = null;
      };
      const settle = (complete, closeSocket = false) => {
        if (settled) return;
        settled = true;
        cleanup();
        if (closeSocket &&
            (socket.readyState === SOCKET_CONNECTING || socket.readyState === SOCKET_OPEN)) {
          try { socket.close(); } catch {}
        }
        complete();
      };
      const cancel = (error) => settle(() => reject(error), true);
      const rejectInactiveOrExpired = () => {
        try {
          if (attempt.canceled || this._closed) throw new SyncLifecycleError();
          this._remainingPairingTime(attempt);
          return false;
        } catch (error) {
          cancel(error);
          return true;
        }
      };
      const protocolFailure = (messageText, cause) => {
        if (rejectInactiveOrExpired()) return;
        cancel(new SyncProtocolError(
          messageText, cause === undefined ? {} : { cause },
        ));
      };
      const opened = () => {
        if (rejectInactiveOrExpired()) return;
        try {
          socket.send(JSON.stringify({
            type: 'pair', protocolVersions: [PROTOCOL_VERSION], name,
          }));
        } catch (cause) {
          if (rejectInactiveOrExpired()) return;
          cancel(new SyncUnavailableError('pairing request could not be sent', { cause }));
        }
      };
      const message = (event) => {
        if (rejectInactiveOrExpired()) return;
        if (response !== undefined || responseError !== undefined) {
          protocolFailure('pairing connection sent more than one response');
          return;
        }
        const data = event.data;
        if (typeof data !== 'string' || data.length > MAX_PAIRING_BYTES ||
            utf8Length(data) > MAX_PAIRING_BYTES) {
          protocolFailure('pairing response is not bounded UTF-8 text');
          return;
        }
        let value;
        try {
          value = JSON.parse(data);
        } catch (cause) {
          protocolFailure('pairing response is not valid JSON', cause);
          return;
        }
        try {
          const nextError = pairingDaemonError(value) ?? undefined;
          const nextResponse = nextError === undefined ? validatePaired(value) : undefined;
          if (rejectInactiveOrExpired()) return;
          responseError = nextError;
          response = nextResponse;
        } catch (error) {
          if (rejectInactiveOrExpired()) return;
          cancel(error instanceof SyncBridgeError
            ? error
            : new SyncProtocolError('pairing response is invalid', { cause: error }));
        }
      };
      const failed = () => {
        if (rejectInactiveOrExpired()) return;
        cancel(new SyncUnavailableError('pairing connection failed'));
      };
      const closed = () => {
        if (rejectInactiveOrExpired()) return;
        if (responseError !== undefined) settle(() => reject(responseError));
        else if (response !== undefined) settle(() => resolve(response));
        else settle(() => reject(new SyncUnavailableError('pairing connection closed before a response')));
      };

      attempt.cancelSocket = cancel;
      socket.addEventListener('open', opened);
      socket.addEventListener('message', message);
      socket.addEventListener('error', failed);
      socket.addEventListener('close', closed);
      timer = setTimeout(() => {
        cancel(new SyncTimeoutError('pairing response timed out'));
      }, timeoutMs);
    });
  }

  async _connect() {
    const attempt = this._beginPassiveAttempt();
    try {
      await this._enforcePassivePermission(attempt);
      if (this._closed) throw new SyncLifecycleError();
    } finally {
      this._finishPassiveAttempt(attempt);
    }
    const socket = await this._openSocket(
      `${this._endpoint.wsOrigin}/control`, undefined, 'control connection',
    );
    if (this._closed) {
      socket.close();
      throw new SyncLifecycleError();
    }
    const session = {
      generation: this._nextControlGeneration++,
      socket,
      welcome: null,
      alive: true,
      terminationError: null,
    };
    this._controlSocket = socket;
    this._controlSession = session;
    this._installControlHandlers(socket, session);
    try {
      const welcome = await this._exchange({
        type: 'hello', token: this._token, protocolVersions: [PROTOCOL_VERSION],
      }, validateWelcome, session);
      this._assertSession(session);
      session.welcome = welcome;
      this._welcome = welcome;
      return welcome;
    } catch (error) {
      this._terminateControl(error, true, session);
      throw error;
    }
  }

  _scheduleControl(session, operation) {
    const result = this._controlQueue.then(() => {
      this._assertSession(session);
      return operation();
    });
    this._controlQueue = result.catch(() => {});
    return result;
  }

  _requestSenderClose(sender) {
    this._senders.delete(sender);
    return this._scheduleControl(
      sender._session,
      () => this._exchange(
        { type: 'closeSender', senderId: sender.id },
        (message) => validateSenderClosed(message, sender.id),
        sender._session,
      ),
    );
  }

  async _openSocket(url, protocol, description, session = null) {
    let socket;
    try {
      socket = protocol === undefined
        ? new this._WebSocket(url)
        : new this._WebSocket(url, protocol);
    } catch (cause) {
      throw new SyncUnavailableError(`${description} could not be created`, { cause });
    }
    return new Promise((resolve, reject) => {
      let timer;
      let settled = false;
      const opening = { socket, session, cancel: null };
      const cleanup = () => {
        clearTimeout(timer);
        socket.removeEventListener('open', opened);
        socket.removeEventListener('error', failed);
        socket.removeEventListener('close', closed);
        this._pendingOpens.delete(opening);
      };
      const settle = (complete) => {
        if (settled) return;
        settled = true;
        cleanup();
        complete();
      };
      const cancel = (error) => {
        settle(() => {
          try {
            if (socket.readyState === SOCKET_CONNECTING || socket.readyState === SOCKET_OPEN) {
              socket.close();
            }
          } catch {}
          reject(error);
        });
      };
      const opened = () => {
        settle(() => resolve(socket));
      };
      const failed = () => {
        cancel(new SyncUnavailableError(`${description} failed`));
      };
      const closed = () => {
        cancel(new SyncUnavailableError(`${description} closed before opening`));
      };
      opening.cancel = cancel;
      this._pendingOpens.add(opening);
      socket.addEventListener('open', opened);
      socket.addEventListener('error', failed);
      socket.addEventListener('close', closed);
      timer = setTimeout(() => {
        cancel(new SyncTimeoutError(`${description} timed out`));
      }, this._timeoutMs);
    }).catch((error) => {
      if (socket.readyState === SOCKET_CONNECTING || socket.readyState === SOCKET_OPEN) {
        try { socket.close(); } catch {}
      }
      throw error;
    });
  }

  _installControlHandlers(socket, session) {
    const message = (event) => this._receiveControl(event.data, session);
    const error = () => this._terminateControl(
      new SyncUnavailableError('control connection failed'), true, session,
    );
    const close = () => {
      if (!this._closed && this._controlSession === session) {
        this._terminateControl(new SyncLifecycleError('control connection closed'), false, session);
      }
    };
    socket.addEventListener('message', message);
    socket.addEventListener('error', error);
    socket.addEventListener('close', close);
    this._controlHandlers = { socket, session, message, error, close };
  }

  _receiveControl(data, session) {
    if (this._controlSession !== session || !session.alive) return;
    const pending = this._controlPending;
    if (!pending) {
      this._terminateControl(new SyncProtocolError('unexpected control message'), true, session);
      return;
    }
    if (typeof data !== 'string' || data.length > this._maxControlMessageBytes ||
        utf8Length(data) > this._maxControlMessageBytes) {
      const error = new SyncProtocolError('control message is not bounded UTF-8 text');
      this._clearControlPending();
      pending.reject(error);
      this._terminateControl(error, true, session);
      return;
    }
    let value;
    try {
      value = JSON.parse(data);
    } catch (cause) {
      const error = new SyncProtocolError('control message is not valid JSON', { cause });
      this._clearControlPending();
      pending.reject(error);
      this._terminateControl(error, true, session);
      return;
    }
    this._clearControlPending();
    pending.resolve(value);
  }

  async _exchange(command, validate, session) {
    this._assertSession(session);
    if (this._controlPending) throw new SyncProtocolError('more than one control request is in flight');
    const body = JSON.stringify(command);
    if (utf8Length(body) > this._maxControlMessageBytes) {
      throw new SyncConfigurationError('control request exceeds the size limit');
    }
    const response = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        if (this._controlPending?.timer !== timer) return;
        const error = new SyncTimeoutError('control response timed out');
        this._clearControlPending();
        reject(error);
        this._terminateControl(error, true, session);
      }, this._timeoutMs);
      this._controlPending = { resolve, reject, timer, session };
    });
    try {
      session.socket.send(body);
    } catch (cause) {
      const error = new SyncUnavailableError('control request could not be sent', { cause });
      const pending = this._controlPending;
      this._clearControlPending();
      pending?.reject(error);
      this._terminateControl(error, true, session);
    }

    const value = await response;
    let remoteError;
    try {
      remoteError = daemonError(value);
    } catch (error) {
      const normalized = error instanceof SyncBridgeError
        ? error
        : new SyncProtocolError('invalid daemon error response', { cause: error });
      this._terminateControl(normalized, true, session);
      throw normalized;
    }
    if (remoteError) throw remoteError;
    try {
      return validate(value);
    } catch (error) {
      const normalized = error instanceof SyncBridgeError
        ? error
        : new SyncProtocolError('invalid control response', { cause: error });
      this._terminateControl(normalized, true, session);
      throw normalized;
    }
  }

  _clearControlPending() {
    if (!this._controlPending) return;
    clearTimeout(this._controlPending.timer);
    this._controlPending = null;
  }

  _terminateControl(error, closeSocket, session) {
    if (session && this._controlSession !== session) {
      this._cancelPendingOpens(error, session);
      if (closeSocket && session.socket?.readyState < SOCKET_CLOSING) {
        try { session.socket.close(); } catch {}
      }
      return;
    }
    const activeSession = session ?? this._controlSession;
    if (activeSession) {
      activeSession.alive = false;
      activeSession.terminationError = error;
    }
    const pending = this._controlPending;
    if (!pending || !activeSession || pending.session === activeSession) {
      this._clearControlPending();
      pending?.reject(error);
    }
    const handlers = this._controlHandlers;
    if (handlers && (!activeSession || handlers.session === activeSession)) {
      handlers.socket.removeEventListener('message', handlers.message);
      handlers.socket.removeEventListener('error', handlers.error);
      handlers.socket.removeEventListener('close', handlers.close);
      this._controlHandlers = null;
    }
    const socket = activeSession?.socket ?? this._controlSocket;
    this._controlSocket = null;
    this._controlSession = null;
    this._welcome = null;
    this._connectPromise = null;
    this._cancelPendingOpens(error, activeSession);
    if (this._senders.size > 0) {
      for (const sender of [...this._senders]) {
        if (!activeSession || sender._session === activeSession) {
          sender._abort(error);
          this._senders.delete(sender);
        }
      }
    }
    if (closeSocket && socket && socket.readyState < SOCKET_CLOSING) {
      try { socket.close(); } catch {}
    }
  }

  _isSessionActive(session) {
    return !this._closed && session !== null && session.alive &&
      this._controlSession === session && session.socket.readyState === SOCKET_OPEN;
  }

  _assertSession(session) {
    if (this._closed) throw new SyncLifecycleError();
    if (!this._isSessionActive(session)) {
      throw session?.terminationError ?? new SyncLifecycleError('control session is no longer active');
    }
  }

  _cancelPendingOpens(error, session) {
    for (const opening of [...this._pendingOpens]) {
      if (session === undefined || opening.session === session) opening.cancel(error);
    }
  }

  _beginPassiveAttempt() {
    if (this._closed) throw new SyncLifecycleError();
    const attempt = {
      controller: new AbortController(),
      deadline: monotonicNow() + this._timeoutMs,
    };
    this._passiveAttempts.add(attempt);
    return attempt;
  }

  _finishPassiveAttempt(attempt) {
    this._passiveAttempts.delete(attempt);
    attempt.controller.abort();
  }

  _remainingPassiveTime(attempt) {
    const remaining = Math.ceil(attempt.deadline - monotonicNow());
    if (remaining < 1) throw new SyncTimeoutError('Sync loopback permission query timed out');
    return remaining;
  }

  async _enforcePassivePermission(attempt) {
    const permissionTimeoutMs = this._remainingPassiveTime(attempt);
    const permissionQuery = this._queryLoopbackPermission(
      attempt.controller.signal,
      () => this._remainingPassiveTime(attempt),
    );
    const state = await runWithTimeout(
      permissionQuery,
      permissionTimeoutMs,
      'Sync loopback permission query',
      () => attempt.controller.abort(),
      attempt.controller.signal,
    );
    this._remainingPassiveTime(attempt);
    if (state === 'prompt') throw new SyncPermissionRequiredError();
    if (state === 'denied') throw new SyncPermissionDeniedError();
  }

  async _queryLoopbackPermission(signal, assertDeadline) {
    const assertActive = () => {
      if (signal?.aborted) throw new SyncLifecycleError('Loopback permission query was canceled');
      assertDeadline?.();
    };
    assertActive();
    if (!this._permissions || typeof this._permissions.query !== 'function') return null;
    let result;
    try {
      result = await this._permissions.query({ name: 'loopback-network' });
    } catch (cause) {
      assertActive();
      if (!isUnsupportedPermissionError(cause)) {
        throw new SyncUnavailableError('Loopback permission query failed', { cause });
      }
      try {
        result = await this._permissions.query({ name: 'local-network-access' });
      } catch (legacyCause) {
        assertActive();
        if (isUnsupportedPermissionError(legacyCause)) return null;
        throw new SyncUnavailableError('Loopback permission query failed', { cause: legacyCause });
      }
    }
    assertActive();
    return permissionState(result?.state);
  }
}
