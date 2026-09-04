// Sync v1 frame: a 64-byte header followed by tightly packed RGBA8.
// Offsets match the daemon's protocol reader; see native/src/protocol.cpp.
export const FRAME_HEADER_BYTES = 64;
const MAGIC = 0x434e5953; // "SYNC" little-endian

export function createFrameBuffer({ width, height }) {
  if (!Number.isInteger(width) || width <= 0) throw new TypeError('width must be a positive integer');
  if (!Number.isInteger(height) || height <= 0) throw new TypeError('height must be a positive integer');
  const payloadBytes = width * height * 4;
  const frame = Buffer.alloc(FRAME_HEADER_BYTES + payloadBytes);
  frame.writeUInt32LE(MAGIC, 0);
  frame.writeUInt16LE(1, 4);                 // protocol version
  frame.writeUInt16LE(FRAME_HEADER_BYTES, 6);
  frame.writeUInt32LE(1, 8);                 // flags
  frame.writeUInt16LE(1, 12);                // colour space
  frame.writeUInt16LE(1, 14);                // transfer
  frame.writeUInt16LE(3, 16);                // pixel format: RGBA8
  frame.writeUInt32LE(width, 20);
  frame.writeUInt32LE(height, 24);
  frame.writeUInt32LE(width * 4, 28);
  frame.writeUInt32LE(payloadBytes, 32);
  return frame;
}

export function stampFrame(frame, sequence, timestampUs) {
  frame.writeBigUInt64LE(BigInt(sequence), 36);
  frame.writeBigUInt64LE(BigInt(timestampUs), 44);
  // A varying payload keeps a receiver's checksum honest across frames.
  frame[FRAME_HEADER_BYTES] = Number(BigInt(sequence) & 0xffn);
  return frame;
}

export function readFrameHeader(buffer) {
  return {
    magic: buffer.readUInt32LE(0),
    version: buffer.readUInt16LE(4),
    headerBytes: buffer.readUInt16LE(6),
    width: buffer.readUInt32LE(20),
    height: buffer.readUInt32LE(24),
    strideBytes: buffer.readUInt32LE(28),
    payloadBytes: buffer.readUInt32LE(32),
    sequence: buffer.readBigUInt64LE(36),
    timestampUs: buffer.readBigUInt64LE(44),
  };
}
