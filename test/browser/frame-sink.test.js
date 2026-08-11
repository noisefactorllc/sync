import assert from 'node:assert/strict';
import test from 'node:test';

import { SyncFrameSink } from '../../browser/frame-sink.js';
import {
  ALPHA_MODE,
  COLOR_SPACE,
  decodeFrameHeaderV1,
  PIXEL_FORMAT,
} from '../../browser/protocol.js';

const DESCRIPTOR = Object.freeze({
  width: 2,
  height: 2,
  format: 'rgba8unorm',
  colorSpace: 'srgb',
  alphaMode: 'premultiplied',
  fps: 30,
});

const FRAME = Object.freeze({
  width: 2,
  height: 2,
  rowStride: 8,
  data: new Uint8Array([
    255, 0, 0, 255,
    0, 255, 0, 128,
    0, 0, 255, 64,
    255, 255, 255, 0,
  ]),
});

class MemoryExportQueue {
  constructor({ capacity = 2 } = {}) {
    this.capacity = capacity;
    this.configured = false;
    this.closed = false;
    this.descriptor = null;
    this.pending = [];
    this.history = [];
    this.polls = 0;
    this.closeCalls = 0;
    this.closeOptions = [];
    this.closeError = null;
    this.failNextEnqueue = false;
  }

  get available() {
    return this.configured && !this.closed && this.pending.length < this.capacity;
  }

  configure(descriptor) {
    this.configured = true;
    this.descriptor = descriptor;
  }

  enqueue(textureId, timestamp, onFrame, context) {
    if (this.failNextEnqueue) {
      this.failNextEnqueue = false;
      return false;
    }
    if (!this.available) return false;

    const entry = { textureId, timestamp, onFrame, context };
    this.pending.push(entry);
    this.history.push(entry);
    return true;
  }

  poll() {
    this.polls += 1;
  }

  complete(frame, index = 0) {
    const [entry] = this.pending.splice(index, 1);
    if (!entry) throw new Error('No pending export to complete');
    entry.onFrame(frame, entry.timestamp, entry.context);
  }

  close(options) {
    this.closeCalls += 1;
    this.closeOptions.push(options);
    this.closed = true;
    this.pending.length = 0;
    if (this.closeError) throw this.closeError;
  }
}

class MemorySocket {
  constructor() {
    this.readyState = 1;
    this.bufferedAmount = 0;
    this.sent = [];
    this.closeCalls = 0;
    this.closeError = null;
    this.sendError = null;
  }

  send(message) {
    if (this.sendError) throw this.sendError;
    this.sent.push(message);
  }

  close() {
    this.closeCalls += 1;
    this.readyState = 3;
    if (this.closeError) throw this.closeError;
  }
}

function createSink({ socket = new MemorySocket(), exportQueue = new MemoryExportQueue(), maxBufferedBytes = 1024, timeOrigin = 1723305600000 } = {}) {
  const sink = new SyncFrameSink({
    socket,
    exportQueue,
    maxBufferedBytes,
    clock: { timeOrigin },
  });
  return { sink, socket, exportQueue };
}

function configuredSink(options) {
  const result = createSink(options);
  result.sink.configure(DESCRIPTOR);
  return result;
}

function withOverride(value, property, override) {
  const result = Object.create(value);
  Object.defineProperty(result, property, { value: override });
  return result;
}

test('constructor validates transport dependencies, pressure limit, and clock', () => {
  const socket = new MemorySocket();
  const exportQueue = new MemoryExportQueue();
  const valid = { socket, exportQueue, maxBufferedBytes: 1, clock: { timeOrigin: 0 } };

  for (const value of [null, {}, withOverride(socket, 'readyState', '1'), withOverride(socket, 'bufferedAmount', '0')]) {
    assert.throws(() => new SyncFrameSink({ ...valid, socket: value }), TypeError);
  }
  for (const method of ['send', 'close']) {
    assert.throws(() => new SyncFrameSink({ ...valid, socket: withOverride(socket, method, null) }), TypeError);
  }
  for (const value of [null, {}, withOverride(exportQueue, 'available', 1)]) {
    assert.throws(() => new SyncFrameSink({ ...valid, exportQueue: value }), TypeError);
  }
  for (const method of ['configure', 'enqueue', 'poll', 'close']) {
    assert.throws(() => new SyncFrameSink({ ...valid, exportQueue: withOverride(exportQueue, method, null) }), TypeError);
  }
  for (const maxBufferedBytes of [0, -1, 1.5, Number.MAX_SAFE_INTEGER + 1]) {
    assert.throws(() => new SyncFrameSink({ ...valid, maxBufferedBytes }), RangeError);
  }
  for (const maxBufferedFrames of [0, -1, 1.5, Number.MAX_SAFE_INTEGER + 1]) {
    assert.throws(() => new SyncFrameSink({
      socket,
      exportQueue,
      maxBufferedFrames,
      clock: { timeOrigin: 0 },
    }), RangeError);
  }
  assert.throws(() => new SyncFrameSink({
    ...valid,
    maxBufferedFrames: 1,
  }), RangeError);
  assert.doesNotThrow(() => new SyncFrameSink({
    socket,
    exportQueue,
    maxBufferedFrames: 1,
    clock: { timeOrigin: 0 },
  }));
  for (const timeOrigin of [-1, Infinity, Number.NaN, '0']) {
    assert.throws(() => new SyncFrameSink({ ...valid, clock: { timeOrigin } }), RangeError);
  }
  assert.doesNotThrow(() => new SyncFrameSink(valid));
});

test('one-frame pressure budget follows live resize in both directions', () => {
  const socket = new MemorySocket();
  const exportQueue = new MemoryExportQueue();
  const sink = new SyncFrameSink({
    socket,
    exportQueue,
    maxBufferedFrames: 1,
    clock: { timeOrigin: 0 },
  });
  const configure = (width, height) => sink.configure({
    ...DESCRIPTOR,
    width,
    height,
  });
  const complete = (width, height) => exportQueue.complete({
    width,
    height,
    rowStride: width * 4,
    data: new Uint8Array(width * height * 4),
  });

  configure(2, 2);
  configure(4, 4);
  assert.equal(sink.submit('larger', 1), true);
  complete(4, 4);
  assert.equal(socket.sent.length, 1);

  configure(2, 2);
  socket.bufferedAmount = 1;
  assert.equal(sink.submit('smaller', 2), false);
  assert.equal(exportQueue.pending.length, 0);
  assert.equal(sink.stats.droppedBackpressure, 1);
});

test('configure validates one v1 descriptor and forwards the identical object', () => {
  const { sink, exportQueue } = createSink();

  for (const descriptor of [
    null,
    { ...DESCRIPTOR, width: 0 },
    { ...DESCRIPTOR, width: 1.5 },
    { ...DESCRIPTOR, height: Number.MAX_SAFE_INTEGER + 1 },
    { ...DESCRIPTOR, format: 'rgba16float' },
    { ...DESCRIPTOR, colorSpace: 'rec2020' },
    { ...DESCRIPTOR, colorSpace: 'toString' },
    { ...DESCRIPTOR, alphaMode: 'associated' },
    { ...DESCRIPTOR, alphaMode: 'constructor' },
    { ...DESCRIPTOR, fps: 0 },
    { ...DESCRIPTOR, fps: Infinity },
  ]) {
    assert.throws(() => sink.configure(descriptor));
  }

  assert.equal(sink.configure(DESCRIPTOR), undefined);
  assert.equal(exportQueue.descriptor, DESCRIPTOR);
});

test('submit is synchronous, polls first, and drops before readback under socket pressure', () => {
  const { sink, socket, exportQueue } = configuredSink({ maxBufferedBytes: 10 });
  socket.bufferedAmount = 11;

  const result = sink.submit('texture', 12.5);

  assert.equal(result, false);
  assert.equal(result instanceof Promise, false);
  assert.equal(exportQueue.polls, 1);
  assert.equal(exportQueue.pending.length, 0);
  assert.deepEqual(sink.stats, {
    accepted: 0,
    droppedBusy: 0,
    droppedBackpressure: 1,
    sent: 0,
    failed: 0,
  });
});

test('submit reserves the full encoded frame budget before starting readback', () => {
  const { sink, socket, exportQueue } = configuredSink({ maxBufferedBytes: 80 });
  socket.bufferedAmount = 1;

  assert.equal(sink.submit('texture', 12.5), false);
  assert.equal(exportQueue.polls, 1);
  assert.equal(exportQueue.pending.length, 0);
  assert.equal(sink.stats.droppedBackpressure, 1);
});

test('submit drops busy when the bounded export ring has no slot', () => {
  const exportQueue = new MemoryExportQueue({ capacity: 1 });
  const { sink } = configuredSink({ exportQueue });

  assert.equal(sink.submit('first', 1), true);
  assert.equal(sink.submit('second', 2), false);
  assert.equal(exportQueue.polls, 2);
  assert.deepEqual(sink.stats, {
    accepted: 1,
    droppedBusy: 1,
    droppedBackpressure: 0,
    sent: 0,
    failed: 0,
  });
});

test('enqueue failure is contained and counted as failed', () => {
  const { sink, exportQueue } = configuredSink();
  exportQueue.failNextEnqueue = true;

  assert.equal(sink.submit('texture', 1), false);
  assert.deepEqual(sink.stats, {
    accepted: 0,
    droppedBusy: 0,
    droppedBackpressure: 0,
    sent: 0,
    failed: 1,
  });
});

test('post-readback pressure drops a frame immediately without sending it', () => {
  const { sink, socket, exportQueue } = configuredSink({ maxBufferedBytes: 80 });
  assert.equal(sink.submit('texture', 10), true);
  socket.bufferedAmount = 1;

  assert.doesNotThrow(() => exportQueue.complete(FRAME));
  assert.equal(socket.sent.length, 0);
  assert.deepEqual(sink.stats, {
    accepted: 1,
    droppedBusy: 0,
    droppedBackpressure: 1,
    sent: 0,
    failed: 0,
  });
});

test('accepted completion sends the exact v1 header and payload', () => {
  const descriptor = {
    ...DESCRIPTOR,
    colorSpace: 'display-p3',
    alphaMode: 'straight',
  };
  const { sink, socket, exportQueue } = createSink();
  sink.configure(descriptor);

  assert.equal(sink.submit('texture', 123.456), true);
  assert.doesNotThrow(() => exportQueue.complete(FRAME));

  assert.equal(socket.sent.length, 1);
  assert.deepEqual(decodeFrameHeaderV1(socket.sent[0]), {
    version: 1,
    headerBytes: 64,
    flags: 1,
    pixelFormat: PIXEL_FORMAT.RGBA8_UNORM,
    colorSpace: COLOR_SPACE.DISPLAY_P3,
    alphaMode: ALPHA_MODE.STRAIGHT,
    width: 2,
    height: 2,
    rowStride: 8,
    payloadBytes: 16,
    sequence: 1,
    presentationTimeUs: 1723305600123456,
  });
  assert.deepEqual([...new Uint8Array(socket.sent[0], 64)], [...FRAME.data]);
  assert.deepEqual(sink.stats, {
    accepted: 1,
    droppedBusy: 0,
    droppedBackpressure: 0,
    sent: 1,
    failed: 0,
  });
});

test('two dropped attempts produce a visible sequence gap and reuse one completion callback', () => {
  const { sink, socket, exportQueue } = configuredSink({ maxBufferedBytes: 80 });

  assert.equal(sink.submit('first', 1), true);
  exportQueue.complete(FRAME);
  socket.bufferedAmount = 1;
  assert.equal(sink.submit('drop-one', 2), false);
  assert.equal(sink.submit('drop-two', 3), false);
  socket.bufferedAmount = 0;
  assert.equal(sink.submit('fourth', 4), true);
  exportQueue.complete(FRAME);

  assert.deepEqual(socket.sent.map((message) => decodeFrameHeaderV1(message).sequence), [1, 4]);
  assert.equal(exportQueue.history[0].onFrame, exportQueue.history[1].onFrame);
  assert.deepEqual(exportQueue.history.map(({ context }) => context), [1, 4]);
});

test('invalid completed frames are contained, counted failed, and never sent', async (t) => {
  const invalidFrames = [
    ['non-object frame', null],
    ['mismatched width', { ...FRAME, width: 3 }],
    ['mismatched height', { ...FRAME, height: 3 }],
    ['short row stride', { ...FRAME, rowStride: 7, data: new Uint8Array(14) }],
    ['non-Uint8Array payload', { ...FRAME, data: new Uint8ClampedArray(FRAME.data) }],
    ['incorrect payload length', { ...FRAME, data: FRAME.data.subarray(0, 15) }],
    ['v1 payload overflow', { ...FRAME, rowStride: 0xffffffff, data: FRAME.data }],
  ];

  for (const [name, frame] of invalidFrames) {
    await t.test(name, () => {
      const { sink, socket, exportQueue } = configuredSink();
      assert.equal(sink.submit('texture', 1), true);
      assert.doesNotThrow(() => exportQueue.complete(frame));
      assert.equal(socket.sent.length, 0);
      assert.equal(sink.stats.failed, 1);
    });
  }
});

test('closed, non-open, and throwing sockets fail without escaping', () => {
  {
    const { sink, socket, exportQueue } = configuredSink();
    socket.readyState = 0;
    assert.equal(sink.submit('texture', 1), false);
    assert.equal(exportQueue.pending.length, 0);
    assert.equal(sink.stats.failed, 1);
  }

  {
    const { sink, socket, exportQueue } = configuredSink();
    assert.equal(sink.submit('texture', 1), true);
    socket.readyState = 3;
    assert.doesNotThrow(() => exportQueue.complete(FRAME));
    assert.equal(socket.sent.length, 0);
    assert.equal(sink.stats.failed, 1);
  }

  {
    const { sink, socket, exportQueue } = configuredSink();
    socket.sendError = new Error('send failed');
    assert.equal(sink.submit('texture', 1), true);
    assert.doesNotThrow(() => exportQueue.complete(FRAME));
    assert.equal(socket.sent.length, 0);
    assert.equal(sink.stats.failed, 1);
  }
});

test('close is idempotent, closes both dependencies once, and rejects later submits', () => {
  const { sink, socket, exportQueue } = configuredSink();
  const stats = sink.stats;

  assert.equal(sink.close(), undefined);
  assert.equal(sink.close(), undefined);
  assert.equal(exportQueue.closeCalls, 1);
  assert.equal(socket.closeCalls, 1);
  assert.equal(sink.stats, stats);
  assert.equal(sink.submit('later', 1), false);
  assert.equal(exportQueue.polls, 1);
  assert.equal(sink.stats.failed, 1);
});

test('backendLost close forwards the exact loss descriptor while still closing the data socket once', () => {
  const { sink, socket, exportQueue } = configuredSink();
  const options = { backendLost: true };

  assert.equal(sink.close(options), undefined);
  assert.equal(sink.close(), undefined);

  assert.deepEqual(exportQueue.closeOptions, [options]);
  assert.equal(socket.closeCalls, 1);
});

test('queue close failure still closes the socket exactly once', () => {
  const { sink, socket, exportQueue } = configuredSink();
  const queueError = new Error('queue close failed');
  exportQueue.closeError = queueError;

  assert.throws(() => sink.close(), (error) => error === queueError);
  assert.equal(exportQueue.closeCalls, 1);
  assert.equal(socket.closeCalls, 1);
  assert.doesNotThrow(() => sink.close());
  assert.equal(exportQueue.closeCalls, 1);
  assert.equal(socket.closeCalls, 1);
});

test('socket close failure occurs after the queue closes and is not retried', () => {
  const { sink, socket, exportQueue } = configuredSink();
  const socketError = new Error('socket close failed');
  socket.closeError = socketError;

  assert.throws(() => sink.close(), (error) => error === socketError);
  assert.equal(exportQueue.closeCalls, 1);
  assert.equal(socket.closeCalls, 1);
  assert.doesNotThrow(() => sink.close());
  assert.equal(exportQueue.closeCalls, 1);
  assert.equal(socket.closeCalls, 1);
});

test('both close failures preserve queue-first error ordering after both attempts', () => {
  const { sink, socket, exportQueue } = configuredSink();
  const queueError = new Error('queue close failed first');
  exportQueue.closeError = queueError;
  socket.closeError = new Error('socket close failed second');

  assert.throws(() => sink.close(), (error) => error === queueError);
  assert.equal(exportQueue.closeCalls, 1);
  assert.equal(socket.closeCalls, 1);
  assert.doesNotThrow(() => sink.close());
  assert.equal(exportQueue.closeCalls, 1);
  assert.equal(socket.closeCalls, 1);
});
