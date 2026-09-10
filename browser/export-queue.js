// Callbacks borrow output bytes until they return. Callers own source resources.
export function validateDescriptor(descriptor) {
  if (!descriptor || typeof descriptor !== 'object') throw new TypeError('descriptor must be an object');
  for (const name of ['width', 'height']) {
    if (!Number.isSafeInteger(descriptor[name]) || descriptor[name] <= 0 || descriptor[name] > 4096) {
      throw new RangeError(`descriptor ${name} must be an integer from 1 to 4096`);
    }
  }
  const bytes = descriptor.width * descriptor.height * 4;
  if (!Number.isSafeInteger(bytes) || bytes > 64 * 1024 * 1024) throw new RangeError('frame size exceeds protocol limits');
  if (descriptor.format !== 'rgba8unorm') throw new RangeError('descriptor format must be rgba8unorm');
  if (!['srgb', 'display-p3'].includes(descriptor.colorSpace)) throw new RangeError('descriptor colorSpace is unsupported');
  if (!['opaque', 'straight', 'premultiplied'].includes(descriptor.alphaMode)) throw new RangeError('descriptor alphaMode is unsupported');
  if (!Number.isFinite(descriptor.fps) || descriptor.fps <= 0) throw new RangeError('descriptor fps must be positive and finite');
  return Object.freeze({ ...descriptor });
}

export function validateSlots(slots) {
  if (!Number.isSafeInteger(slots) || slots < 1 || slots > 8) throw new RangeError('slots must be an integer from 1 to 8');
  return slots;
}

export function frameFrom(descriptor, data) {
  return { ...descriptor, rowStride: descriptor.width * 4, data };
}

export function validateCallback(onFrame) {
  if (typeof onFrame !== 'function') throw new TypeError('onFrame must be a function');
}

export class ExportQueue {
  constructor() {
    this._descriptor = null;
    this._closed = false;
    this._busy = false;
    this._generation = 0;
  }

  get available() { return !this._closed && this._descriptor !== null && !this._busy; }

  configure(descriptor) {
    if (this._closed) return;
    this._descriptor = validateDescriptor(descriptor);
    this._generation += 1;
  }

  poll() {}

  close() {
    this._closed = true;
    this._descriptor = null;
    this._generation += 1;
  }
}
