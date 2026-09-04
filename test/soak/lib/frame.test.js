import assert from 'node:assert/strict';
import test from 'node:test';
import { createFrameBuffer, stampFrame, readFrameHeader, FRAME_HEADER_BYTES }
  from './frame.mjs';

test('a created frame carries the Sync magic and the requested geometry', () => {
  const frame = createFrameBuffer({ width: 320, height: 200 });
  assert.equal(frame.length, FRAME_HEADER_BYTES + 320 * 200 * 4);
  const header = readFrameHeader(frame);
  assert.equal(header.magic, 0x434e5953);
  assert.equal(header.version, 1);
  assert.equal(header.headerBytes, FRAME_HEADER_BYTES);
  assert.equal(header.width, 320);
  assert.equal(header.height, 200);
  assert.equal(header.strideBytes, 320 * 4);
  assert.equal(header.payloadBytes, 320 * 200 * 4);
});

test('stamping writes the sequence and timestamp the receiver reads back', () => {
  const frame = createFrameBuffer({ width: 8, height: 8 });
  stampFrame(frame, 4242, 1_700_000_000_000_000n);
  const header = readFrameHeader(frame);
  assert.equal(header.sequence, 4242n);
  assert.equal(header.timestampUs, 1_700_000_000_000_000n);
});

test('stamping varies a payload byte so a receiver checksum moves per frame', () => {
  const frame = createFrameBuffer({ width: 8, height: 8 });
  stampFrame(frame, 1, 0n);
  const first = frame[FRAME_HEADER_BYTES];
  stampFrame(frame, 2, 0n);
  assert.notEqual(frame[FRAME_HEADER_BYTES], first);
});
