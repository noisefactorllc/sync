const MAGIC = 0x434e5953;
const VERSION = 1;
const HEADER_BYTES = 64;
const TOP_DOWN_FLAG = 1;
const MAX_UINT32 = 0xffffffff;
const MAX_SAFE_BIGINT = BigInt(Number.MAX_SAFE_INTEGER);

export const PIXEL_FORMAT = Object.freeze({
  RGBA8_UNORM: 1,
});

export const COLOR_SPACE = Object.freeze({
  SRGB: 1,
  DISPLAY_P3: 2,
});

export const ALPHA_MODE = Object.freeze({
  OPAQUE: 1,
  STRAIGHT: 2,
  PREMULTIPLIED: 3,
});

function bytesFrom(value, name) {
  if (value instanceof ArrayBuffer) {
    return new Uint8Array(value);
  }
  if (ArrayBuffer.isView(value)) {
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  }
  throw new TypeError(`${name} must be an ArrayBuffer or typed array`);
}

const PIXEL_FORMAT_VALUES = new Set(Object.values(PIXEL_FORMAT));
const COLOR_SPACE_VALUES = new Set(Object.values(COLOR_SPACE));
const ALPHA_MODE_VALUES = new Set(Object.values(ALPHA_MODE));

function validateEnum(value, allowed, name) {
  if (!allowed.has(value)) {
    throw new RangeError(`Unsupported ${name}`);
  }
}

function validateUint32(value, name, { positive = false } = {}) {
  if (!Number.isSafeInteger(value) || value < (positive ? 1 : 0) || value > MAX_UINT32) {
    throw new RangeError(`${name} must be a ${positive ? 'positive ' : ''}uint32`);
  }
}

function validateSafeInteger(value, name) {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new RangeError(`${name} must be a non-negative safe integer`);
  }
}

function expectedPayloadBytes(width, height, rowStride) {
  const minimumStride = width * 4;
  if (rowStride < minimumStride) {
    throw new RangeError('row stride must be at least width * 4');
  }
  const payloadBytes = rowStride * height;
  if (!Number.isSafeInteger(payloadBytes) || payloadBytes > MAX_UINT32) {
    throw new RangeError('payload size exceeds protocol v1 limits');
  }
  return payloadBytes;
}

function validateFrameFields({ width, height, rowStride, pixelFormat, colorSpace, alphaMode }) {
  validateEnum(pixelFormat, PIXEL_FORMAT_VALUES, 'pixel format');
  validateEnum(colorSpace, COLOR_SPACE_VALUES, 'color space');
  validateEnum(alphaMode, ALPHA_MODE_VALUES, 'alpha mode');
  validateUint32(width, 'width', { positive: true });
  validateUint32(height, 'height', { positive: true });
  validateUint32(rowStride, 'row stride', { positive: true });
  return expectedPayloadBytes(width, height, rowStride);
}

// Encodes one v1 frame. With no `into`, returns a fresh ArrayBuffer holding
// exactly the frame. With `into` (an ArrayBuffer of at least 64 + payload
// bytes), writes the frame at offset 0 and returns a Uint8Array view of it, so
// a caller sending sixty frames a second can keep one staging buffer for the
// life of a stream instead of allocating and zero-filling megabytes per frame.
// A WebSocket copies the bytes out of a view synchronously in send(), so the
// staging buffer is free to reuse the moment send() returns.
export function encodeFrameV1(metadata, rgbaBytes, into = undefined) {
  const {
    width,
    height,
    rowStride,
    sequence,
    presentationTimeUs,
    pixelFormat,
    colorSpace,
    alphaMode,
  } = metadata ?? {};
  const payload = bytesFrom(rgbaBytes, 'rgbaBytes');
  const payloadBytes = validateFrameFields({
    width,
    height,
    rowStride,
    pixelFormat,
    colorSpace,
    alphaMode,
  });
  validateSafeInteger(sequence, 'sequence');
  validateSafeInteger(presentationTimeUs, 'presentation time');
  if (payload.byteLength !== payloadBytes) {
    throw new RangeError('payload length must equal row stride * height');
  }

  const frameBytes = HEADER_BYTES + payloadBytes;
  let frame;
  if (into === undefined) {
    frame = new ArrayBuffer(frameBytes);
  } else if (!(into instanceof ArrayBuffer)) {
    throw new TypeError('into must be an ArrayBuffer');
  } else if (into.byteLength < frameBytes) {
    throw new RangeError('into is smaller than the encoded frame');
  } else {
    frame = into;
  }
  const view = new DataView(frame, 0, HEADER_BYTES);
  view.setUint32(0, MAGIC, true);
  view.setUint16(4, VERSION, true);
  view.setUint16(6, HEADER_BYTES, true);
  view.setUint32(8, TOP_DOWN_FLAG, true);
  view.setUint16(12, pixelFormat, true);
  view.setUint16(14, colorSpace, true);
  view.setUint16(16, alphaMode, true);
  view.setUint16(18, 0, true);
  view.setUint32(20, width, true);
  view.setUint32(24, height, true);
  view.setUint32(28, rowStride, true);
  view.setUint32(32, payloadBytes, true);
  view.setBigUint64(36, BigInt(sequence), true);
  view.setBigUint64(44, BigInt(presentationTimeUs), true);
  // Reserved tail: a reused buffer must not carry stale bytes here.
  view.setUint32(52, 0, true);
  view.setUint32(56, 0, true);
  view.setUint32(60, 0, true);
  new Uint8Array(frame, HEADER_BYTES, payloadBytes).set(payload);
  return into === undefined ? frame : new Uint8Array(frame, 0, frameBytes);
}

export function decodeFrameHeaderV1(bytes) {
  const frame = bytesFrom(bytes, 'bytes');
  if (frame.byteLength < HEADER_BYTES) {
    throw new RangeError('Frame is shorter than the v1 header');
  }
  const view = new DataView(frame.buffer, frame.byteOffset, frame.byteLength);
  if (view.getUint32(0, true) !== MAGIC) {
    throw new RangeError('Invalid frame magic');
  }
  if (view.getUint16(4, true) !== VERSION) {
    throw new RangeError('Unsupported protocol version');
  }
  if (view.getUint16(6, true) !== HEADER_BYTES) {
    throw new RangeError('Unsupported header size');
  }
  const flags = view.getUint32(8, true);
  if ((flags & TOP_DOWN_FLAG) === 0) {
    throw new RangeError('Top-down row-order flag is required');
  }
  if ((flags & ~TOP_DOWN_FLAG) !== 0) {
    throw new RangeError('Unsupported frame flags');
  }

  const pixelFormat = view.getUint16(12, true);
  const colorSpace = view.getUint16(14, true);
  const alphaMode = view.getUint16(16, true);
  const reserved = view.getUint16(18, true);
  if (reserved !== 0) {
    throw new RangeError('Reserved header fields must be zero');
  }
  for (let offset = 52; offset < HEADER_BYTES; offset += 1) {
    if (frame[offset] !== 0) {
      throw new RangeError('Reserved header fields must be zero');
    }
  }

  const width = view.getUint32(20, true);
  const height = view.getUint32(24, true);
  const rowStride = view.getUint32(28, true);
  const payloadBytes = view.getUint32(32, true);
  const expectedBytes = validateFrameFields({
    width,
    height,
    rowStride,
    pixelFormat,
    colorSpace,
    alphaMode,
  });
  if (payloadBytes !== expectedBytes) {
    throw new RangeError('Payload length does not match dimensions and row stride');
  }
  if (frame.byteLength !== HEADER_BYTES && frame.byteLength !== HEADER_BYTES + payloadBytes) {
    throw new RangeError('Frame payload length does not match header');
  }

  const sequence = view.getBigUint64(36, true);
  const presentationTimeUs = view.getBigUint64(44, true);
  if (sequence > MAX_SAFE_BIGINT || presentationTimeUs > MAX_SAFE_BIGINT) {
    throw new RangeError('64-bit frame values exceed the safe integer range');
  }

  return {
    version: VERSION,
    headerBytes: HEADER_BYTES,
    flags,
    pixelFormat,
    colorSpace,
    alphaMode,
    width,
    height,
    rowStride,
    payloadBytes,
    sequence: Number(sequence),
    presentationTimeUs: Number(presentationTimeUs),
  };
}
