import { SYNC_ERROR_CODE } from './client.js';
import { SYNC_SDK_VERSION } from './version.js';

const CODES = new Set(Object.values(SYNC_ERROR_CODE));
const LOCAL_COUNTERS = ['accepted', 'sent', 'droppedBusy', 'droppedBackpressure', 'failed'];
const NATIVE_COUNTERS = ['accepted', 'dropped', 'rejected', 'failed', 'lastSequence', 'lastPresentationTimeUs'];
const count = (value) => Number.isSafeInteger(value) && value >= 0 ? value : null;
const errorCode = (error) => CODES.has(error?.code) ? error.code : null;
const counters = (source, keys) => source ? Object.fromEntries(keys.map(key => [key, count(source[key])])) : null;

// Export only defined diagnostic fields. Never copy an error message or cause.
export async function createDiagnosticSnapshot({ client, sender, descriptor, error } = {}) {
  const welcome = client?.welcome;
  const version = welcome?.version;
  const providers = Array.isArray(welcome?.capabilities?.providers)
    ? welcome.capabilities.providers.slice(0, 4).filter(provider =>
      /^[a-z][a-z0-9-]{0,31}$/.test(provider?.id) &&
      ['send', 'receive'].includes(provider.direction) &&
      typeof provider.available === 'boolean' && typeof provider.selected === 'boolean')
      .map(({ id, direction, available, selected }) => ({ id, direction, available, selected }))
    : [];
  const snapshot = {
    schemaVersion: 1,
    sdkVersion: SYNC_SDK_VERSION,
    daemonVersion: typeof version === 'string' && version.length <= 64 &&
      /^\d+\.\d+\.\d+(?:[-+][a-zA-Z0-9.-]+)?$/.test(version) ? version : null,
    protocolVersion: welcome?.protocolVersion === 1 ? 1 : null,
    connected: client?.connected === true,
    providers,
    frame: descriptor ? {
      width: count(descriptor.width), height: count(descriptor.height),
      format: descriptor.format === 'rgba8unorm' ? descriptor.format : null,
      colorSpace: ['srgb', 'display-p3'].includes(descriptor.colorSpace) ? descriptor.colorSpace : null,
      alphaMode: ['opaque', 'straight', 'premultiplied'].includes(descriptor.alphaMode) ? descriptor.alphaMode : null,
      fps: Number.isFinite(descriptor.fps) && descriptor.fps > 0 ? descriptor.fps : null,
    } : null,
    local: counters(sender?.stats, LOCAL_COUNTERS),
    native: null,
    nativeError: null,
    error: errorCode(error),
  };
  if (typeof sender?.getStats === 'function') {
    try {
      const native = await sender.getStats();
      snapshot.native = counters(native, NATIVE_COUNTERS);
      if (snapshot.native) {
        snapshot.native.checksum = typeof native.checksum === 'string' && /^[0-9a-f]{16}$/.test(native.checksum)
          ? native.checksum : null;
      }
    } catch (cause) {
      snapshot.nativeError = errorCode(cause) ?? SYNC_ERROR_CODE.UNAVAILABLE;
    }
  }
  return snapshot;
}
