import assert from 'node:assert/strict';
import test from 'node:test';
import { streamFrames } from './stream.mjs';

test('immediately completed frame writes yield to a queued stop', async (t) => {
  let stopped = false;
  let writes = 0;
  const stop = setImmediate(() => { stopped = true; });
  t.after(() => clearImmediate(stop));
  await streamFrames({
    shouldStop: () => stopped || writes >= 1000,
    writeFrame: async () => { writes += 1; },
  });
  assert.equal(stopped, true, 'streaming must let the queued stop run');
  assert.equal(writes, 64, 'one bounded burst runs before yielding');
});

test('a paced stream waits between writes instead of flooding a slow test target', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  let writes = 0;
  const streaming = streamFrames({ fps: 20,
    shouldStop: () => writes >= 2,
    writeFrame: async () => { writes += 1; },
  });
  await Promise.resolve();
  assert.equal(writes, 1);
  t.mock.timers.tick(49);
  await Promise.resolve();
  assert.equal(writes, 1, 'a second frame cannot be sent before its interval');
  t.mock.timers.tick(1);
  await Promise.resolve();
  assert.equal(writes, 2);
  await Promise.resolve();
  t.mock.timers.tick(50);
  await streaming;
});

test('a stop interrupts a long pacing interval', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  let stopped = false;
  let writes = 0;
  let completed = false;
  const streaming = streamFrames({ fps: 0.01,
    shouldStop: () => stopped,
    writeFrame: async () => { writes += 1; },
  }).then(() => { completed = true; });
  await Promise.resolve();
  stopped = true;
  t.mock.timers.tick(100);
  await Promise.resolve();
  await Promise.resolve();
  assert.equal(completed, true, 'stopping must not wait out a 100-second frame interval');
  await streaming;
  assert.equal(writes, 1);
});

test('a fractional frame interval rounds up to preserve the rate ceiling', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  // MockTimers preserves fractions, whereas Node's real timers truncate them.
  const schedule = globalThis.setTimeout;
  t.mock.method(globalThis, 'setTimeout', (callback, milliseconds) =>
    schedule(callback, Math.trunc(milliseconds)));
  let writes = 0;
  const streaming = streamFrames({ fps: 60,
    shouldStop: () => writes >= 2,
    writeFrame: async () => { writes += 1; },
  });
  await Promise.resolve();
  t.mock.timers.tick(16);
  await Promise.resolve();
  assert.equal(writes, 1, '16ms would exceed 60 FPS');
  t.mock.timers.tick(1);
  await streaming;
  assert.equal(writes, 2);
});

test('paced waits service lifecycle work between frames', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  let writes = 0;
  let serviced = false;
  const streaming = streamFrames({ fps: 0.01,
    shouldStop: () => serviced,
    writeFrame: async () => { writes += 1; },
    onWait: async () => { serviced = true; },
  });
  await Promise.resolve();
  t.mock.timers.tick(100);
  await Promise.resolve();
  await Promise.resolve();
  assert.equal(serviced, true, 'lifecycle work must not wait 100 seconds for another frame');
  await streaming;
  assert.equal(writes, 1);
});
