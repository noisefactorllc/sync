import { setImmediate as yieldToEventLoop } from 'node:timers/promises';

export async function streamFrames({ shouldStop, writeFrame, fps = 0, onWait }) {
  let turns = 0;
  while (!shouldStop()) {
    await writeFrame();
    turns += 1;
    if (fps > 0) {
      // A ceiling, not a catch-up schedule: a slow frame must not cause a
      // burst of queued work on an already overloaded target.
      // Small waits keep shouldStop responsive even at very low frame rates
      // and avoid overflowing Node's maximum timer delay. Round up because
      // Node truncates fractional delays (16.67ms would become 16ms).
      for (let remaining = Math.ceil(1000 / fps); remaining > 0 && !shouldStop();) {
        const delay = Math.min(remaining, 100);
        await new Promise((resolve) => setTimeout(resolve, delay));
        remaining -= delay;
        if (onWait && !shouldStop()) await onWait();
      }
    } else if (turns % 64 === 0) {
      await yieldToEventLoop();
    }
  }
}
