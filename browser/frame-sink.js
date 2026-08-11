import {
  ALPHA_MODE,
  COLOR_SPACE,
  encodeFrameV1,
  PIXEL_FORMAT,
} from './protocol.js';

const SOCKET_OPEN = 1;
const MAX_UINT32 = 0xffffffff;
const HEADER_BYTES = 64;

const COLOR_SPACE_ENUM = Object.freeze({
  srgb: COLOR_SPACE.SRGB,
  'display-p3': COLOR_SPACE.DISPLAY_P3,
});

const ALPHA_MODE_ENUM = Object.freeze({
  opaque: ALPHA_MODE.OPAQUE,
  straight: ALPHA_MODE.STRAIGHT,
  premultiplied: ALPHA_MODE.PREMULTIPLIED,
});

function validateSocket(socket) {
  if (!socket ||
      typeof socket.readyState !== 'number' || !Number.isFinite(socket.readyState) ||
      typeof socket.bufferedAmount !== 'number' || !Number.isFinite(socket.bufferedAmount) ||
      typeof socket.send !== 'function' ||
      typeof socket.close !== 'function') {
    throw new TypeError('socket must expose numeric readyState/bufferedAmount and send/close methods');
  }
}

function validateExportQueue(exportQueue) {
  if (!exportQueue ||
      typeof exportQueue.available !== 'boolean' ||
      typeof exportQueue.configure !== 'function' ||
      typeof exportQueue.enqueue !== 'function' ||
      typeof exportQueue.poll !== 'function' ||
      typeof exportQueue.close !== 'function') {
    throw new TypeError('exportQueue must expose available and configure/enqueue/poll/close methods');
  }
}

function validateDescriptor(descriptor) {
  if (!descriptor || typeof descriptor !== 'object') {
    throw new TypeError('descriptor must be an object');
  }
  if (!Number.isSafeInteger(descriptor.width) || descriptor.width <= 0) {
    throw new RangeError('descriptor width must be a positive safe integer');
  }
  if (!Number.isSafeInteger(descriptor.height) || descriptor.height <= 0) {
    throw new RangeError('descriptor height must be a positive safe integer');
  }
  if (descriptor.format !== 'rgba8unorm') {
    throw new RangeError('descriptor format must be rgba8unorm');
  }
  if (!Object.hasOwn(COLOR_SPACE_ENUM, descriptor.colorSpace)) {
    throw new RangeError('descriptor colorSpace is unsupported');
  }
  if (!Object.hasOwn(ALPHA_MODE_ENUM, descriptor.alphaMode)) {
    throw new RangeError('descriptor alphaMode is unsupported');
  }
  if (typeof descriptor.fps !== 'number' || !Number.isFinite(descriptor.fps) || descriptor.fps <= 0) {
    throw new RangeError('descriptor fps must be positive and finite');
  }
}

function validFrame(frame, descriptor) {
  if (!frame || typeof frame !== 'object' || !descriptor) return false;
  if (frame.width !== descriptor.width || frame.height !== descriptor.height) return false;
  if (!Number.isSafeInteger(frame.width) || frame.width <= 0 || frame.width > MAX_UINT32) return false;
  if (!Number.isSafeInteger(frame.height) || frame.height <= 0 || frame.height > MAX_UINT32) return false;
  if (!Number.isSafeInteger(frame.rowStride) || frame.rowStride < frame.width * 4 || frame.rowStride > MAX_UINT32) return false;
  if (!(frame.data instanceof Uint8Array)) return false;

  const payloadBytes = frame.rowStride * frame.height;
  return Number.isSafeInteger(payloadBytes) &&
    payloadBytes <= MAX_UINT32 &&
    frame.data.byteLength === payloadBytes;
}

export class SyncFrameSink {
  constructor({
    socket,
    exportQueue,
    maxBufferedBytes,
    maxBufferedFrames,
    clock = globalThis.performance,
  } = {}) {
    validateSocket(socket);
    validateExportQueue(exportQueue);
    const hasByteBudget = maxBufferedBytes !== undefined;
    const hasFrameBudget = maxBufferedFrames !== undefined;
    if (hasByteBudget === hasFrameBudget) {
      throw new RangeError('exactly one buffered-byte or buffered-frame limit is required');
    }
    if (hasByteBudget &&
        (!Number.isSafeInteger(maxBufferedBytes) || maxBufferedBytes <= 0)) {
      throw new RangeError('maxBufferedBytes must be a positive safe integer');
    }
    if (hasFrameBudget &&
        (!Number.isSafeInteger(maxBufferedFrames) || maxBufferedFrames <= 0)) {
      throw new RangeError('maxBufferedFrames must be a positive safe integer');
    }
    if (!clock || typeof clock.timeOrigin !== 'number' ||
        !Number.isFinite(clock.timeOrigin) || clock.timeOrigin < 0) {
      throw new RangeError('clock.timeOrigin must be a finite non-negative number');
    }

    this._socket = socket;
    this._exportQueue = exportQueue;
    this._maxBufferedBytes = maxBufferedBytes ?? null;
    this._maxBufferedFrames = maxBufferedFrames ?? null;
    this._timeOrigin = clock.timeOrigin;
    this._descriptor = null;
    this._encodedFrameBytes = null;
    this._sequence = 0;
    this._closed = false;
    this._onFrame = (frame, timestamp, sequence) => {
      this._complete(frame, timestamp, sequence);
    };
    this.stats = {
      accepted: 0,
      droppedBusy: 0,
      droppedBackpressure: 0,
      sent: 0,
      failed: 0,
    };
  }

  configure(descriptor) {
    validateDescriptor(descriptor);
    this._descriptor = descriptor;
    const payloadBytes = descriptor.width * descriptor.height * 4;
    this._encodedFrameBytes = Number.isSafeInteger(payloadBytes)
      ? HEADER_BYTES + payloadBytes
      : Number.MAX_SAFE_INTEGER;
    this._exportQueue.configure(descriptor);
  }

  submit(textureId, timestamp) {
    const sequence = ++this._sequence;
    try {
      this._exportQueue.poll();
    } catch {
      this.stats.failed += 1;
      return false;
    }

    if (this._closed || this._socket.readyState !== SOCKET_OPEN) {
      this.stats.failed += 1;
      return false;
    }
    if (this._encodedFrameBytes !== null &&
        this._wouldExceedBufferedBudget(this._encodedFrameBytes)) {
      this.stats.droppedBackpressure += 1;
      return false;
    }
    if (!this._exportQueue.available) {
      this.stats.droppedBusy += 1;
      return false;
    }

    try {
      if (this._exportQueue.enqueue(textureId, timestamp, this._onFrame, sequence) !== true) {
        this.stats.failed += 1;
        return false;
      }
    } catch {
      this.stats.failed += 1;
      return false;
    }

    this.stats.accepted += 1;
    return true;
  }

  close(options) {
    if (this._closed) return;
    this._closed = true;
    let firstError;
    try {
      this._exportQueue.close(options);
    } catch (error) {
      firstError = error;
    }
    try {
      this._socket.close();
    } catch (error) {
      if (!firstError) firstError = error;
    }
    if (firstError) throw firstError;
  }

  _complete(frame, timestamp, sequence) {
    try {
      if (!validFrame(frame, this._descriptor)) {
        this.stats.failed += 1;
        return;
      }
      if (this._closed || this._socket.readyState !== SOCKET_OPEN) {
        this.stats.failed += 1;
        return;
      }
      const encodedFrameBytes = HEADER_BYTES + frame.data.byteLength;
      if (this._wouldExceedBufferedBudget(encodedFrameBytes)) {
        this.stats.droppedBackpressure += 1;
        return;
      }

      const message = encodeFrameV1({
        width: frame.width,
        height: frame.height,
        rowStride: frame.rowStride,
        sequence,
        presentationTimeUs: Math.round((this._timeOrigin + timestamp) * 1000),
        pixelFormat: PIXEL_FORMAT.RGBA8_UNORM,
        colorSpace: COLOR_SPACE_ENUM[this._descriptor.colorSpace],
        alphaMode: ALPHA_MODE_ENUM[this._descriptor.alphaMode],
      }, frame.data);

      if (this._closed || this._socket.readyState !== SOCKET_OPEN) {
        this.stats.failed += 1;
        return;
      }
      if (this._wouldExceedBufferedBudget(message.byteLength)) {
        this.stats.droppedBackpressure += 1;
        return;
      }

      this._socket.send(message);
      this.stats.sent += 1;
    } catch {
      this.stats.failed += 1;
    }
  }

  _wouldExceedBufferedBudget(additionalBytes) {
    const frameBudget = this._encodedFrameBytes === null
      ? 0
      : this._encodedFrameBytes * this._maxBufferedFrames;
    const limit = this._maxBufferedBytes ?? (
      Number.isSafeInteger(frameBudget) ? frameBudget : Number.MAX_SAFE_INTEGER
    );
    return additionalBytes > limit ||
      this._socket.bufferedAmount > limit - additionalBytes;
  }
}
