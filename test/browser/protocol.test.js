import assert from 'node:assert/strict';
import test from 'node:test';

import {
  ALPHA_MODE,
  COLOR_SPACE,
  decodeFrameHeaderV1,
  encodeFrameV1,
  PIXEL_FORMAT,
} from '../../browser/protocol.js';

const metadata = Object.freeze({
  width: 2,
  height: 2,
  rowStride: 8,
  sequence: 4294967301,
  presentationTimeUs: 1723305600123456,
  pixelFormat: PIXEL_FORMAT.RGBA8_UNORM,
  colorSpace: COLOR_SPACE.SRGB,
  alphaMode: ALPHA_MODE.PREMULTIPLIED,
});

const payload = new Uint8Array([
  255, 0, 0, 255,
  0, 255, 0, 128,
  0, 0, 255, 64,
  255, 255, 255, 0,
]);

test('encodes the v1 golden frame at the specified little-endian offsets', () => {
  const frame = encodeFrameV1(metadata, payload);
  const view = new DataView(frame);

  assert.equal(frame.byteLength, 80);
  assert.equal(view.getUint32(0, true), 0x434e5953);
  assert.equal(view.getUint16(4, true), 1);
  assert.equal(view.getUint16(6, true), 64);
  assert.equal(view.getUint32(8, true), 1);
  assert.equal(view.getUint16(12, true), 1);
  assert.equal(view.getUint16(14, true), 1);
  assert.equal(view.getUint16(16, true), 3);
  assert.equal(view.getUint16(18, true), 0);
  assert.equal(view.getUint32(20, true), 2);
  assert.equal(view.getUint32(24, true), 2);
  assert.equal(view.getUint32(28, true), 8);
  assert.equal(view.getUint32(32, true), 16);
  assert.equal(view.getBigUint64(36, true), 4294967301n);
  assert.equal(view.getBigUint64(44, true), 1723305600123456n);
  assert.deepEqual(
    [...new Uint8Array(frame, 52, 12)],
    [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
  );
  assert.deepEqual([...new Uint8Array(frame, 64)], [...payload]);
});

test('round-trips the v1 golden frame metadata and payload contract', () => {
  const frame = encodeFrameV1(metadata, payload);

  assert.deepEqual(decodeFrameHeaderV1(frame), {
    version: 1,
    headerBytes: 64,
    flags: 1,
    pixelFormat: PIXEL_FORMAT.RGBA8_UNORM,
    colorSpace: COLOR_SPACE.SRGB,
    alphaMode: ALPHA_MODE.PREMULTIPLIED,
    width: 2,
    height: 2,
    rowStride: 8,
    payloadBytes: 16,
    sequence: 4294967301,
    presentationTimeUs: 1723305600123456,
  });
});

test('rejects unsupported metadata and incorrect payload size before framing', () => {
  assert.throws(() => encodeFrameV1({ ...metadata, pixelFormat: 99 }, payload), /pixel format/i);
  assert.throws(() => encodeFrameV1({ ...metadata, colorSpace: 99 }, payload), /color space/i);
  assert.throws(() => encodeFrameV1({ ...metadata, alphaMode: 99 }, payload), /alpha mode/i);
  assert.throws(() => encodeFrameV1({ ...metadata, width: 0 }, payload), /width/i);
  assert.throws(() => encodeFrameV1({ ...metadata, height: 1.5 }, payload), /height/i);
  assert.throws(() => encodeFrameV1({ ...metadata, rowStride: 7 }, payload), /row stride/i);
  assert.throws(() => encodeFrameV1({ ...metadata, sequence: Number.MAX_SAFE_INTEGER + 1 }, payload), /sequence/i);
  assert.throws(() => encodeFrameV1({ ...metadata, presentationTimeUs: Number.MAX_SAFE_INTEGER + 1 }, payload), /presentation time/i);
  assert.throws(() => encodeFrameV1(metadata, payload.subarray(0, 15)), /payload/i);
});

test('rejects malformed v1 headers before accepting a frame', () => {
  const valid = new Uint8Array(encodeFrameV1(metadata, payload));
  const decode = (mutate) => {
    const bytes = valid.slice();
    mutate(new DataView(bytes.buffer), bytes);
    return () => decodeFrameHeaderV1(bytes);
  };

  assert.throws(decode((view) => view.setUint32(0, 0, true)), /magic/i);
  assert.throws(decode((view) => view.setUint16(4, 2, true)), /version/i);
  assert.throws(decode((view) => view.setUint16(6, 63, true)), /header/i);
  assert.throws(decode((view) => view.setUint32(8, 0, true)), /top-down/i);
  assert.throws(decode((view) => view.setUint16(12, 9, true)), /pixel format/i);
  assert.throws(decode((view) => view.setUint16(14, 9, true)), /color space/i);
  assert.throws(decode((view) => view.setUint16(16, 9, true)), /alpha mode/i);
  assert.throws(decode((view) => view.setUint16(18, 1, true)), /reserved/i);
  assert.throws(decode((view) => view.setUint32(20, 0, true)), /width/i);
  assert.throws(decode((view) => view.setUint32(28, 7, true)), /row stride/i);
  assert.throws(decode((view, bytes) => bytes[52] = 1), /reserved/i);
  assert.throws(decode((view) => view.setBigUint64(36, BigInt(Number.MAX_SAFE_INTEGER) + 1n, true)), /safe integer/i);
  assert.throws(decode((view) => view.setUint32(32, 15, true)), /payload/i);
});

test('accepts a header alone but rejects a supplied frame with length mismatch', () => {
  const frame = new Uint8Array(encodeFrameV1(metadata, payload));
  assert.equal(decodeFrameHeaderV1(frame.subarray(0, 64)).payloadBytes, 16);
  assert.throws(() => decodeFrameHeaderV1(frame.subarray(0, 79)), /payload/i);
  assert.throws(() => decodeFrameHeaderV1(new Uint8Array(64)), /magic/i);
});
