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
    this.sentBuffers = [];
    this.closeCalls = 0;
    this.closeError = null;
    this.sendError = null;
  }

  send(message) {
    if (this.sendError) throw this.sendError;
    // A real WebSocket copies the bytes out synchronously, so the caller is
    // free to reuse its buffer. Snapshot here for the same reason, and keep
    // the backing buffer's identity so tests can see reuse.
    if (message instanceof ArrayBuffer) {
      this.sent.push(message.slice(0));
      this.sentBuffers.push(message);
    } else {
      this.sent.push(message.buffer.slice(message.byteOffset, message.byteOffset + message.byteLength));
      this.sentBuffers.push(message.buffer);
    }
  }

  close() {
    this.closeCalls += 1;
    this.readyState = 3;
    if (this.closeError) throw this.closeError;
  }
}

class TaskBufferedSocket extends MemorySocket {
  constructor() {
    super();
    this.peakBufferedAmount = 0;
  }

  send(message) {
    super.send(message);
    this.bufferedAmount += message.byteLength;
    this.peakBufferedAmount = Math.max(this.peakBufferedAmount, this.bufferedAmount);
  }

  drain() {
    this.bufferedAmount = 0;
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

test('poll completion admits every next render task within a one-frame socket budget', { timeout: 1000 }, () => {
  const socket = new TaskBufferedSocket();
  const exportQueue = new MemoryExportQueue({ capacity: 1 });
  exportQueue.poll = function () {
    this.polls += 1;
    if (this.pending.length) this.complete(FRAME);
  };
  const sink = new SyncFrameSink({ socket, exportQueue, maxBufferedFrames: 1, clock: { timeOrigin: 0 } });
  sink.configure(DESCRIPTOR);
  const accepted = [];

  for (let tick = 1; tick <= 12; tick += 1) {
    socket.drain(); // Browser transport drains between render tasks, never inside send().
    accepted.push(sink.submit(`texture-${tick}`, tick));
  }

  assert.deepEqual(accepted, Array(12).fill(true));
  assert.deepEqual(socket.sent.map((message) => decodeFrameHeaderV1(message).sequence), [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]);
  assert.equal(socket.peakBufferedAmount, 80);
  assert.equal(exportQueue.pending.length, 1);
  assert.deepEqual(sink.stats, { accepted: 12, droppedBusy: 0, droppedBackpressure: 0, sent: 11, failed: 0 });
});

test('pressure inherited from an earlier task rejects admission even if poll drains it', { timeout: 1000 }, () => {
  const { sink, socket, exportQueue } = configuredSink({ maxBufferedBytes: 80 });
  socket.bufferedAmount = 1;
  exportQueue.poll = () => { socket.bufferedAmount = 0; };

  assert.equal(sink.submit('pressured', 1), false);
  assert.equal(exportQueue.pending.length, 0);
  assert.equal(sink.stats.droppedBackpressure, 1);
  assert.equal(sink.submit('next-task', 2), true);
});

test('multiple poll completions and an immediate enqueue completion cannot exceed one encoded frame', { timeout: 1000 }, () => {
  const socket = new TaskBufferedSocket();
  const exportQueue = new MemoryExportQueue();
  const sink = new SyncFrameSink({ socket, exportQueue, maxBufferedFrames: 1, clock: { timeOrigin: 0 } });
  sink.configure(DESCRIPTOR);
  assert.equal(sink.submit('first', 1), true);
  assert.equal(sink.submit('second', 2), true);
  exportQueue.poll = function () {
    while (this.pending.length) this.complete(FRAME);
  };
  const enqueue = exportQueue.enqueue.bind(exportQueue);
  exportQueue.enqueue = function (...args) {
    const accepted = enqueue(...args);
    if (accepted) this.complete(FRAME);
    return accepted;
  };

  assert.equal(sink.submit('third', 3), true);
  assert.equal(socket.peakBufferedAmount, 80);
  assert.deepEqual(socket.sent.map((message) => decodeFrameHeaderV1(message).sequence), [1]);
  assert.deepEqual(sink.stats, { accepted: 3, droppedBusy: 0, droppedBackpressure: 2, sent: 1, failed: 0 });
});

test('padded-stride completions still obey the configured socket byte limit', { timeout: 1000 }, () => {
  const socket = new TaskBufferedSocket();
  const exportQueue = new MemoryExportQueue();
  const sink = new SyncFrameSink({ socket, exportQueue, maxBufferedFrames: 1, clock: { timeOrigin: 0 } });
  sink.configure(DESCRIPTOR);
  assert.equal(sink.submit('padded', 1), true);
  exportQueue.complete({ ...FRAME, rowStride: 12, data: new Uint8Array(24) });

  assert.equal(socket.sent.length, 0);
  assert.equal(sink.stats.droppedBackpressure, 1);
  assert.equal(sink.submit('tight', 2), true);
  exportQueue.complete(FRAME);
  assert.equal(socket.peakBufferedAmount, 80);
});

test('asynchronous completion copies borrowed export bytes before the callback returns', { timeout: 1000 }, async () => {
  const { sink, socket, exportQueue } = configuredSink();
  const borrowed = new Uint8Array(FRAME.data);
  assert.equal(sink.submit('async', 1), true);

  await new Promise((resolve) => {
    queueMicrotask(() => {
      exportQueue.complete({ ...FRAME, data: borrowed });
      borrowed.fill(0);
      resolve();
    });
  });

  assert.equal(socket.sent.length, 1);
  assert.deepEqual([...new Uint8Array(socket.sent[0], 64)], [...FRAME.data]);
  assert.equal(sink.stats.sent, 1);
});

test('reconfigure during poll invalidates admission and preserves global sequence order', { timeout: 1000 }, () => {
  const { sink, socket, exportQueue } = configuredSink();
  exportQueue.poll = () => sink.configure({ ...DESCRIPTOR, colorSpace: 'display-p3' });

  assert.equal(sink.submit('old-configuration', 1), false);
  assert.equal(exportQueue.history.length, 0);
  assert.equal(sink.stats.failed, 1);
  exportQueue.poll = () => {};
  assert.equal(sink.submit('new-configuration', 2), true);
  exportQueue.complete(FRAME);
  assert.equal(decodeFrameHeaderV1(socket.sent[0]).sequence, 2);
  assert.equal(decodeFrameHeaderV1(socket.sent[0]).colorSpace, COLOR_SPACE.DISPLAY_P3);
});

test('close during poll fails admission without enqueueing on the closed queue', { timeout: 1000 }, () => {
  const { sink, exportQueue } = configuredSink();
  exportQueue.poll = () => sink.close();
  exportQueue.enqueue = () => { throw new Error('closed queue must not be touched'); };

  assert.equal(sink.submit('closed-during-poll', 1), false);
  assert.equal(exportQueue.history.length, 0);
  assert.equal(sink.stats.failed, 1);
  assert.equal(sink.stats.droppedBusy, 0);
});

test('socket must remain open across the admission decision and poll', { timeout: 1000 }, () => {
  const { sink, socket, exportQueue } = configuredSink();
  socket.readyState = 0;
  exportQueue.poll = () => { socket.readyState = 1; };

  assert.equal(sink.submit('opening-during-poll', 1), false);
  assert.equal(exportQueue.history.length, 0);
  assert.equal(sink.stats.failed, 1);
  exportQueue.poll = () => { socket.readyState = 3; };
  assert.equal(sink.submit('closing-during-poll', 2), false);
  assert.equal(exportQueue.history.length, 0);
  assert.equal(sink.stats.failed, 2);
});

test('callbacks from a previous configuration cannot use a new descriptor with identical dimensions', { timeout: 1000 }, () => {
  const { sink, socket, exportQueue } = configuredSink();
  assert.equal(sink.submit('old', 1), true);
  sink.configure({ ...DESCRIPTOR, colorSpace: 'display-p3' });
  assert.equal(sink.submit('current', 2), true);

  exportQueue.complete(FRAME);
  assert.equal(socket.sent.length, 0);
  assert.equal(sink.stats.failed, 1);
  exportQueue.complete(FRAME);
  assert.deepEqual(socket.sent.map((message) => decodeFrameHeaderV1(message).sequence), [2]);
  assert.equal(decodeFrameHeaderV1(socket.sent[0]).colorSpace, COLOR_SPACE.DISPLAY_P3);
});

test('duplicate and reordered completions never send a non-increasing sequence', { timeout: 1000 }, () => {
  const { sink, socket, exportQueue } = configuredSink();
  assert.equal(sink.submit('older', 1), true);
  assert.equal(sink.submit('newer', 2), true);
  const duplicate = exportQueue.history[1];
  exportQueue.complete(FRAME, 1);
  exportQueue.complete(FRAME);
  duplicate.onFrame(FRAME, duplicate.timestamp, duplicate.context);
  assert.equal(sink.submit('next', 3), true);
  exportQueue.complete(FRAME);

  assert.deepEqual(socket.sent.map((message) => decodeFrameHeaderV1(message).sequence), [2, 3]);
  assert.equal(sink.stats.failed, 2);
});

test('a failed send consumes its completion and cannot be replayed later', { timeout: 1000 }, () => {
  const { sink, socket, exportQueue } = configuredSink();
  assert.equal(sink.submit('throws', 1), true);
  const duplicate = exportQueue.history[0];
  socket.sendError = new Error('send failed');
  exportQueue.complete(FRAME);
  socket.sendError = null;
  duplicate.onFrame(FRAME, duplicate.timestamp, duplicate.context);
  assert.equal(sink.submit('recovers', 2), true);
  exportQueue.complete(FRAME);

  assert.deepEqual(socket.sent.map((message) => decodeFrameHeaderV1(message).sequence), [2]);
  assert.equal(sink.stats.failed, 2);
});

test('failed configure invalidates pending exports until a successful configuration', { timeout: 1000 }, () => {
  const { sink, socket, exportQueue } = configuredSink();
  assert.equal(sink.submit('old', 1), true);
  const configure = exportQueue.configure.bind(exportQueue);
  const error = new Error('configure failed');
  exportQueue.configure = () => { throw error; };

  assert.throws(() => sink.configure({ ...DESCRIPTOR }), (caught) => caught === error);
  assert.equal(sink.stats.failed, 1);
  exportQueue.complete(FRAME);
  assert.equal(sink.submit('unconfigured', 2), false);
  assert.equal(socket.sent.length, 0);
  assert.equal(sink.stats.failed, 3);
  exportQueue.configure = configure;
  sink.configure(DESCRIPTOR);
  assert.equal(sink.submit('recovered', 3), true);
  exportQueue.complete(FRAME);
  assert.deepEqual(socket.sent.map((message) => decodeFrameHeaderV1(message).sequence), [3]);
});

test('an initially available queue retains pre-configuration submit compatibility', { timeout: 1000 }, () => {
  const { sink, exportQueue } = createSink();
  exportQueue.configure(DESCRIPTOR);

  assert.equal(sink.submit('available-before-sink-configure', 1), true);
  assert.equal(exportQueue.pending.length, 1);
  assert.equal(sink.stats.accepted, 1);
});

test('configure after close leaves both closed dependencies alone', { timeout: 1000 }, () => {
  const { sink, exportQueue } = configuredSink();
  sink.close();
  exportQueue.configure = () => { throw new Error('closed queue must not be touched'); };

  assert.doesNotThrow(() => sink.configure(DESCRIPTOR));
  assert.equal(exportQueue.closeCalls, 1);
});

test('enqueue reconfiguration invalidates a successful return and its later callback', { timeout: 1000 }, () => {
  const { sink, socket, exportQueue } = configuredSink();
  const enqueue = exportQueue.enqueue.bind(exportQueue);
  exportQueue.enqueue = (...args) => {
    const accepted = enqueue(...args);
    sink.configure({ ...DESCRIPTOR });
    return accepted;
  };

  assert.equal(sink.submit('reconfigured-during-enqueue', 1), false);
  assert.equal(sink.stats.accepted, 0);
  assert.equal(sink.stats.failed, 1);
  exportQueue.complete(FRAME);
  assert.equal(socket.sent.length, 0);
  assert.equal(sink.stats.failed, 2);
});

test('throwing poll or enqueue fails only that submit and allows later progress', { timeout: 1000 }, () => {
  for (const method of ['poll', 'enqueue']) {
    const { sink, socket, exportQueue } = configuredSink();
    const original = exportQueue[method].bind(exportQueue);
    exportQueue[method] = () => { throw new Error(`${method} failed`); };

    assert.equal(sink.submit('throws', 1), false);
    assert.deepEqual(sink.stats, { accepted: 0, droppedBusy: 0, droppedBackpressure: 0, sent: 0, failed: 1 });
    exportQueue[method] = original;
    assert.equal(sink.submit('recovered', 2), true);
    exportQueue.complete(FRAME);
    assert.deepEqual(socket.sent.map((message) => decodeFrameHeaderV1(message).sequence), [2]);
  }
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
  // A closed sink leaves its closed queue alone.
  assert.equal(exportQueue.polls, 0);
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

test('frames are encoded into one staging buffer that is reused across frames', () => {
  const { sink, socket, exportQueue } = configuredSink();

  assert.equal(sink.submit('a', 1), true);
  exportQueue.complete(FRAME);
  assert.equal(sink.submit('b', 2), true);
  exportQueue.complete({ ...FRAME, data: new Uint8Array(FRAME.data).reverse() });

  assert.equal(socket.sent.length, 2);
  assert.ok(ArrayBuffer.isView(socket.sentBuffers[0]) === false);
  assert.equal(socket.sentBuffers[0], socket.sentBuffers[1]);
  assert.deepEqual([...new Uint8Array(socket.sent[0], 64)], [...FRAME.data]);
  assert.deepEqual([...new Uint8Array(socket.sent[1], 64)], [...FRAME.data].reverse());
  assert.equal(decodeFrameHeaderV1(socket.sent[0]).sequence, 1);
  assert.equal(decodeFrameHeaderV1(socket.sent[1]).sequence, 2);
});

test('the staging buffer grows to a padded stride and is replaced on reconfigure', () => {
  const { sink, socket, exportQueue } = configuredSink();
  assert.equal(sink.submit('a', 1), true);
  exportQueue.complete(FRAME);
  const first = socket.sentBuffers[0];

  // Same descriptor, wider stride: the payload is larger than width*height*4.
  const padded = new Uint8Array(24);
  padded.set(FRAME.data.subarray(0, 8), 0);
  padded.set(FRAME.data.subarray(8, 16), 12);
  assert.equal(sink.submit('b', 2), true);
  exportQueue.complete({ ...FRAME, rowStride: 12, data: padded });
  assert.notEqual(socket.sentBuffers[1], first);
  assert.equal(decodeFrameHeaderV1(socket.sent[1]).rowStride, 12);
  assert.deepEqual([...new Uint8Array(socket.sent[1], 64)], [...padded]);

  // The larger buffer now serves the smaller frame too.
  assert.equal(sink.submit('c', 3), true);
  exportQueue.complete(FRAME);
  assert.equal(socket.sentBuffers[2], socket.sentBuffers[1]);
  assert.equal(socket.sent[2].byteLength, 80);

  sink.configure({ ...DESCRIPTOR, width: 3 });
  assert.equal(sink.submit('d', 4), true);
  exportQueue.complete({ width: 3, height: 2, rowStride: 12, data: padded });
  assert.notEqual(socket.sentBuffers[3], socket.sentBuffers[2]);
});

test('a closed sink touches neither the export queue nor the socket', () => {
  const { sink, socket, exportQueue } = configuredSink();
  assert.equal(sink.submit('a', 1), true);
  sink.close();
  const polls = exportQueue.polls;
  assert.equal(sink.submit('b', 2), false);
  assert.equal(exportQueue.polls, polls);
  assert.equal(socket.sent.length, 0);
  // A late completion from the queue after close is dropped, not sent.
  assert.doesNotThrow(() => sink._onFrame(FRAME, 1, 1));
  assert.equal(socket.sent.length, 0);
  assert.equal(sink.stats.failed, 2);
});
