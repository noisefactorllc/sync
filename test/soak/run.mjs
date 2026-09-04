// Streams frames through a test-receiver syncd for SYNC_SOAK_SECONDS and writes
// one JSON sample per line to SYNC_SOAK_OUT (default: stdout).
//
//   SYNC_DAEMON_PATH=/path/to/syncd SYNC_SOAK_SECONDS=28800 \
//   SYNC_SOAK_OUT=protocol.jsonl node test/soak/run.mjs
//
// Progress goes to stderr only; stdout (or SYNC_SOAK_OUT) carries nothing but
// the JSONL a later analysis stage reads. SIGINT/SIGTERM ask the engine to
// stop after its current tick so the run still closes its session, tears the
// daemon down, and writes a final summary line before the process exits.
//
// The run is wrapped in try/finally: across an eight-hour run of ~480
// lifecycle cycles, a transient hiccup can throw out of soak.run(). Without
// the finally, that would orphan the syncd child and drop the flush — and an
// orphaned daemon is exactly what steals the camera extension's sink from
// the next run. On that path an `error` record lands in the JSONL before the
// summary, so a run that dies at hour six still leaves evidence of why, and
// the process exits non-zero.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { ProtocolSoak, summarise } from './engine.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const daemonPath = path.resolve(ROOT, process.env.SYNC_DAEMON_PATH || 'build/syncd');
const seconds = Number(process.env.SYNC_SOAK_SECONDS || 30);
const outPath = process.env.SYNC_SOAK_OUT || '';
const stream = outPath ? fs.createWriteStream(outPath, { flags: 'a' }) : process.stdout;
const samples = [];

const soak = new ProtocolSoak({
  daemonPath,
  origin: process.env.SYNC_SOAK_ORIGIN || 'https://soak.example',
  token: process.env.SYNC_SOAK_TOKEN || 'soak-token-123',
  width: Number(process.env.SYNC_SOAK_WIDTH || 1920),
  height: Number(process.env.SYNC_SOAK_HEIGHT || 1080),
  cycleMs: Number(process.env.SYNC_SOAK_CYCLE || 60) * 1000,
  leaksEveryMs: Number(process.env.SYNC_SOAK_LEAKS_EVERY || 900) * 1000,
  onSample(sample) {
    samples.push(sample);
    stream.write(`${JSON.stringify(sample)}\n`);
  },
});

const port = await soak.start();
process.stderr.write(`protocol soak pid=${soak.daemon.pid} port=${port}\n`);
const shutdown = () => { soak.stopped = true; };
process.once('SIGINT', shutdown);
process.once('SIGTERM', shutdown);
let runError = null;
try {
  await soak.run(seconds * 1000);
} catch (error) {
  runError = error;
  process.stderr.write(`protocol soak run failed: ${error?.stack || error}\n`);
} finally {
  const result = await soak.stop();
  const summary = samples.length >= 2 ? summarise(samples, { warmupFraction: 0.25 }) : null;
  if (runError) {
    stream.write(`${JSON.stringify({ plane: 'protocol', type: 'error',
      message: runError.message ?? String(runError),
      stack: String(runError?.stack ?? runError) })}\n`);
  }
  stream.write(`${JSON.stringify({ plane: 'protocol', type: 'summary', ...result,
    growthKb: summary ? summary.growthKb : null, peakKb: summary ? summary.peak : null })}\n`);
  if (result.reaped === false) {
    process.stderr.write(`protocol soak: daemon pid=${soak.daemon.pid} would not die ` +
      `after SIGTERM+SIGKILL; giving up rather than hanging\n`);
  }
  process.stderr.write(`sent=${result.sentFrames} dropped=${result.dropped} ` +
    `reconnects=${result.reconnects} growth=${summary ? summary.growthKb : 'n/a'}KiB ` +
    `exit=${result.exitCode}\n`);
  if (outPath) await new Promise((resolve) => stream.end(resolve));
  if (result.exitCode !== 0 || runError || result.reaped === false) process.exitCode = 1;
}
