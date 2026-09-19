import { spawn } from 'node:child_process';
import { existsSync, unlinkSync, readFileSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { upgrade } from '../soak/lib/ws.mjs';
import { residentKbAsync, footprintKbAsync } from '../soak/lib/process-metrics.mjs';

const ROOT = fileURLToPath(new URL('../../', import.meta.url));
const SERVER_BIN = path.resolve(ROOT, 'build/sync_audio_test_server');
const PRODUCER_BIN = '/Users/alex/platform/scaffold/apps/sync-soak/bin/paced-producer.mjs';

function parseArgs() {
  const args = process.argv.slice(2);
  let baselineDuration = 120; // default 2 min for baselines
  let combinedDuration = 1800; // default 30 min for combined load
  let withReverse = false;
  let outPath = null;
  for (let i = 0; i < args.length; ++i) {
    if (args[i] === '--baseline-duration' && i + 1 < args.length) {
      baselineDuration = parseInt(args[++i], 10);
    } else if (args[i] === '--combined-duration' && i + 1 < args.length) {
      combinedDuration = parseInt(args[++i], 10);
    } else if (args[i] === '--with-reverse') {
      withReverse = true;
    } else if (args[i] === '--out' && i + 1 < args.length) {
      outPath = args[++i];
    }
  }
  return { baselineDuration, combinedDuration, withReverse, outPath };
}

function percentile(sorted, p) {
  if (sorted.length === 0) return 0;
  const index = (p / 100) * (sorted.length - 1);
  const lower = Math.floor(index);
  const upper = Math.ceil(index);
  const weight = index - lower;
  return sorted[lower] * (1 - weight) + sorted[upper] * weight;
}

async function startServer() {
  return new Promise((resolve, reject) => {
    const proc = spawn(SERVER_BIN, [
      '--port', '0',
      '--test-origin', 'http://127.0.0.1:8000',
      '--test-token', 'soak-token',
      '--test-receiver'
    ], { stdio: ['ignore', 'pipe', 'inherit'] });

    let ready = false;
    let buffer = '';
    proc.stdout.setEncoding('utf8');
    proc.stdout.on('data', chunk => {
      buffer += chunk;
      const lines = buffer.split('\n');
      for (const line of lines) {
        if (!line.trim()) continue;
        try {
          const msg = JSON.parse(line.trim());
          if (msg.type === 'ready') {
            ready = true;
            resolve({ proc, port: msg.port, pid: proc.pid });
            return;
          }
        } catch {
          // not ready json
        }
      }
    });

    proc.on('error', reject);
    proc.on('exit', code => {
      if (!ready) reject(new Error(`Server exited early with code ${code}`));
    });
  });
}

async function runArm({ armName, durationSeconds, runAudio, runVideo }) {
  console.log(`\n========================================`);
  console.log(`Starting Arm: ${armName} (duration: ${durationSeconds}s / ${(durationSeconds / 60).toFixed(1)} min)`);
  console.log(`Audio: ${runAudio ? 'ON (32-ch, 48 kHz float32)' : 'OFF'}`);
  console.log(`Video: ${runVideo ? 'ON (1080p60 uncompressed RGBA8)' : 'OFF'}`);
  console.log(`========================================`);

  const { proc: serverProc, port, pid } = await startServer();
  const telemetrySeries = [];
  let samplingActive = true;

  const audioMetrics = {
    totalReads: 0,
    emptyReads: 0,
    normalReads: 0,
    totalFrames: 0,
    serverReportedDrops: 0,
    cursorDiscontinuities: 0,
    rttMs: [],
    slowReads10ms: 0,
    slowReads20ms: 0,
  };

  const videoMetrics = {
    framesScheduled: 0,
    framesSent: 0,
    framesSkipped: 0,
    accepted: 0,
    dropped: 0,
    completeSeconds: 0,
    lowRateSeconds: 0,
    deliveredFps: 0,
    secondBuckets: {},
  };

  let audioActive = false;
  let audioPromise = Promise.resolve();

  if (runAudio) {
    audioActive = true;
    const audioControl = await upgrade(port, '/control', { origin: 'http://127.0.0.1:8000' });
    audioControl.sendText(JSON.stringify({ type: 'hello', token: 'soak-token', protocolVersions: [1] }));
    await audioControl.nextJson();
    audioControl.sendText(JSON.stringify({ type: 'openAudioSource', sourceId: 'audio_32' }));
    await audioControl.nextJson();

    audioPromise = (async () => {
      let nextExpectedFrame = null;
      while (audioActive) {
        const t0 = performance.now();
        audioControl.sendText(JSON.stringify({ type: 'readAudioSource', sourceId: 'audio_32' }));
        const resp = await audioControl.next();
        const rtt = performance.now() - t0;
        audioMetrics.rttMs.push(rtt);
        audioMetrics.totalReads++;
        if (rtt >= 10) audioMetrics.slowReads10ms++;
        if (rtt >= 20) audioMetrics.slowReads20ms++;

        const buf = resp.payload;
        const frameCount = buf.readUInt32LE(12);
        const firstFrame = Number(buf.readBigUInt64LE(16));
        const dropped = Number(buf.readBigUInt64LE(24));
        if (dropped > audioMetrics.serverReportedDrops) {
          audioMetrics.serverReportedDrops = dropped;
        }

        if (frameCount === 0) {
          audioMetrics.emptyReads++;
          await new Promise(r => setTimeout(r, 2));
        } else {
          audioMetrics.normalReads++;
          audioMetrics.totalFrames += frameCount;
          if (nextExpectedFrame !== null && firstFrame !== nextExpectedFrame) {
            audioMetrics.cursorDiscontinuities++;
          }
          nextExpectedFrame = firstFrame + frameCount;
        }
      }
      try {
        audioControl.sendText(JSON.stringify({ type: 'closeAudioSource', sourceId: 'audio_32' }));
        audioControl.close();
      } catch {}
    })();
  }

  const producerOutPath = `/tmp/producer-qual-${Date.now()}-${Math.random().toString(36).slice(2)}.jsonl`;
  let videoPromise = Promise.resolve();

  if (runVideo) {
    videoPromise = new Promise((resolve, reject) => {
      const producer = spawn('node', [PRODUCER_BIN], {
        env: {
          ...process.env,
          PRODUCER_PORT: String(port),
          PRODUCER_ORIGIN: 'http://127.0.0.1:8000',
          PRODUCER_TOKEN: 'soak-token',
          PRODUCER_OUT: producerOutPath,
          PRODUCER_SECONDS: String(durationSeconds),
          PRODUCER_FPS: '60',
          PRODUCER_ALLOW_NICE: '1',
        },
        stdio: 'inherit',
      });
      producer.on('error', reject);
      producer.on('exit', code => {
        if (code !== 0) reject(new Error(`Producer exited with code ${code}`));
        else resolve();
      });
    });
  } else {
    videoPromise = new Promise(r => setTimeout(r, durationSeconds * 1000));
  }

  // Sample telemetry every 5 seconds
  const startTime = Date.now();
  const telemetrySampler = (async () => {
    while (samplingActive) {
      try {
        const elapsedSec = Math.floor((Date.now() - startTime) / 1000);
        const rss = await residentKbAsync(pid);
        const footprint = await footprintKbAsync(pid);
        const sample = {
          elapsedSeconds: elapsedSec,
          rssKb: rss,
          footprintKb: footprint,
          audioFrames: audioMetrics.totalFrames,
          audioDrops: audioMetrics.serverReportedDrops,
          audioGaps: audioMetrics.cursorDiscontinuities,
          audioSlowReads10ms: audioMetrics.slowReads10ms,
        };
        telemetrySeries.push(sample);
        if (elapsedSec > 0 && elapsedSec % 30 === 0) {
          console.log(`[Progress ${armName} @ ${elapsedSec}s/${durationSeconds}s]: RSS=${rss} KiB, Footprint=${footprint} KiB, AudioFrames=${audioMetrics.totalFrames}, AudioDrops=${audioMetrics.serverReportedDrops}, AudioSlowReads10ms=${audioMetrics.slowReads10ms}`);
        }
      } catch {}
      await new Promise(r => setTimeout(r, 5000));
    }
  })();

  await videoPromise;

  if (runAudio) {
    audioActive = false;
    await audioPromise;
  }

  samplingActive = false;
  await telemetrySampler;

  if (runVideo && existsSync(producerOutPath)) {
    const content = readFileSync(producerOutPath, 'utf8');
    const lines = content.trim().split('\n');
    let firstSubmitEpochMs = null;
    let summaryRecord = null;
    for (const line of lines) {
      if (!line.trim()) continue;
      try {
        const record = JSON.parse(line);
        if (record.type === 'summary') {
          summaryRecord = record;
        } else if (record.type === 'frame') {
          if (!record.skipped) {
            videoMetrics.framesSent++;
            if (firstSubmitEpochMs === null) firstSubmitEpochMs = record.submitEpochMs;
            const elapsedSec = Math.floor((record.submitEpochMs - firstSubmitEpochMs) / 1000);
            videoMetrics.secondBuckets[elapsedSec] = (videoMetrics.secondBuckets[elapsedSec] || 0) + 1;
          } else {
            videoMetrics.framesSkipped++;
          }
        }
      } catch {}
    }
    if (summaryRecord) {
      videoMetrics.framesSent = summaryRecord.framesSent;
      videoMetrics.framesSkipped = summaryRecord.framesSkipped;
      videoMetrics.framesScheduled = summaryRecord.framesScheduled;
      videoMetrics.accepted = summaryRecord.accepted;
      videoMetrics.dropped = summaryRecord.dropped;
    } else {
      videoMetrics.framesScheduled = videoMetrics.framesSent + videoMetrics.framesSkipped;
    }
    videoMetrics.deliveredFps = videoMetrics.framesSent / durationSeconds;

    const buckets = Object.values(videoMetrics.secondBuckets);
    const stableBuckets = buckets.length > 2 ? buckets.slice(1, -1) : buckets;
    for (const count of stableBuckets) {
      if (count >= 59) videoMetrics.completeSeconds++;
      if (count < 50) videoMetrics.lowRateSeconds++;
    }

    try { unlinkSync(producerOutPath); } catch {}
  }

  serverProc.kill('SIGTERM');
  await new Promise(r => setTimeout(r, 200));
  try { serverProc.kill('SIGKILL'); } catch {}

  audioMetrics.rttMs.sort((a, b) => a - b);
  const audioSummary = runAudio ? {
    totalReads: audioMetrics.totalReads,
    emptyReads: audioMetrics.emptyReads,
    normalReads: audioMetrics.normalReads,
    totalFrames: audioMetrics.totalFrames,
    nominalFrames: durationSeconds * 48000,
    continuityRatio: Number((audioMetrics.totalFrames / (durationSeconds * 48000)).toFixed(4)),
    sampleDropCount: audioMetrics.serverReportedDrops,
    cursorDiscontinuities: audioMetrics.cursorDiscontinuities,
    slowReads10ms: audioMetrics.slowReads10ms,
    slowReads20ms: audioMetrics.slowReads20ms,
    rttPercentilesMs: {
      p50: Number(percentile(audioMetrics.rttMs, 50).toFixed(3)),
      p90: Number(percentile(audioMetrics.rttMs, 90).toFixed(3)),
      p95: Number(percentile(audioMetrics.rttMs, 95).toFixed(3)),
      p99: Number(percentile(audioMetrics.rttMs, 99).toFixed(3)),
      max: Number(percentile(audioMetrics.rttMs, 100).toFixed(3)),
    },
  } : null;

  const initialRss = telemetrySeries.length > 0 ? telemetrySeries[0].rssKb : 0;
  const finalRss = telemetrySeries.length > 0 ? telemetrySeries[telemetrySeries.length - 1].rssKb : 0;
  const initialFootprint = telemetrySeries.length > 0 ? telemetrySeries[0].footprintKb : 0;
  const finalFootprint = telemetrySeries.length > 0 ? telemetrySeries[telemetrySeries.length - 1].footprintKb : 0;
  const peakRss = telemetrySeries.reduce((max, s) => Math.max(max, s.rssKb || 0), 0);
  const peakFootprint = telemetrySeries.reduce((max, s) => Math.max(max, s.footprintKb || 0), 0);

  // Compute memory slope (KiB/min) over stable region (after 1st minute)
  let footprintSlopeKbPerMin = 0;
  if (telemetrySeries.length > 12) {
    const stable = telemetrySeries.slice(6); // after ~30s
    const first = stable[0];
    const last = stable[stable.length - 1];
    const minutes = (last.elapsedSeconds - first.elapsedSeconds) / 60;
    if (minutes > 0) {
      footprintSlopeKbPerMin = Number(((last.footprintKb - first.footprintKb) / minutes).toFixed(2));
    }
  }

  const armResult = {
    arm: armName,
    durationSeconds,
    audio: audioSummary,
    video: runVideo ? {
      framesScheduled: videoMetrics.framesScheduled,
      framesSent: videoMetrics.framesSent,
      framesSkipped: videoMetrics.framesSkipped,
      accepted: videoMetrics.accepted,
      dropped: videoMetrics.dropped,
      deliveredFps: Number(videoMetrics.deliveredFps.toFixed(3)),
      completeSeconds: videoMetrics.completeSeconds,
      lowRateSeconds: videoMetrics.lowRateSeconds,
    } : null,
    memory: {
      initialRssKb: initialRss,
      finalRssKb: finalRss,
      peakRssKb: peakRss,
      deltaRssKb: finalRss - initialRss,
      initialFootprintKb: initialFootprint,
      finalFootprintKb: finalFootprint,
      peakFootprintKb: peakFootprint,
      deltaFootprintKb: finalFootprint - initialFootprint,
      footprintSlopeKbPerMin,
    },
    telemetrySamples: telemetrySeries,
  };

  console.log(`\nArm Summary: ${armName}`);
  if (runAudio) {
    console.log(`  Audio: frames=${audioSummary.totalFrames} (nominal=${audioSummary.nominalFrames}, drops=${audioSummary.sampleDropCount}, gaps=${audioSummary.cursorDiscontinuities}, p50=${audioSummary.rttPercentilesMs.p50}ms, p95=${audioSummary.rttPercentilesMs.p95}ms, max=${audioSummary.rttPercentilesMs.max}ms)`);
  }
  if (runVideo) {
    console.log(`  Video: sent=${armResult.video.framesSent}, skipped=${armResult.video.framesSkipped}, accepted=${armResult.video.accepted}, fps=${armResult.video.deliveredFps}, completeSec=${armResult.video.completeSeconds}, lowRateSec=${armResult.video.lowRateSeconds}`);
  }
  console.log(`  Memory: RSS ${initialRss} -> ${finalRss} KiB (Peak ${peakRss}), Footprint ${initialFootprint} -> ${finalFootprint} KiB (Peak ${peakFootprint}, Slope ${footprintSlopeKbPerMin} KiB/min)`);

  return armResult;
}

async function main() {
  const { baselineDuration, combinedDuration, withReverse, outPath } = parseArgs();
  console.log(`Running Combined Audio + Video Load Qualification`);
  console.log(`Baseline duration: ${baselineDuration}s, Combined duration: ${combinedDuration}s, withReverse: ${withReverse}`);

  const arms = [
    { armName: 'arm_baseline_audio', runAudio: true, runVideo: false, durationSeconds: baselineDuration },
    { armName: 'arm_baseline_video', runAudio: false, runVideo: true, durationSeconds: baselineDuration },
    { armName: 'arm_combined_load', runAudio: true, runVideo: true, durationSeconds: combinedDuration },
  ];

  if (withReverse) {
    arms.push(
      { armName: 'arm_post_baseline_video', runAudio: false, runVideo: true, durationSeconds: Math.min(baselineDuration, 60) },
      { armName: 'arm_post_baseline_audio', runAudio: true, runVideo: false, durationSeconds: Math.min(baselineDuration, 60) },
    );
  }

  const results = [];
  for (const arm of arms) {
    const res = await runArm(arm);
    results.push(res);
    await new Promise(r => setTimeout(r, 3000));
  }

  const output = {
    schema: 'sync-combined-audio-video-qualification-v1',
    suite: 'Sync 1080p60 Combined Audio (32-ch) + Video Load Qualification',
    timestampUtc: new Date().toISOString(),
    host: {
      platform: process.platform,
      arch: process.arch,
      nodeVersion: process.version,
    },
    baselineDurationSeconds: baselineDuration,
    combinedDurationSeconds: combinedDuration,
    arms: results,
  };

  if (outPath) {
    writeFileSync(outPath, JSON.stringify(output, null, 2));
    console.log(`\nResults written to ${outPath}`);
  }

  console.log('\n========================================');
  console.log('QUALIFICATION EXECUTION COMPLETE');
  console.log('========================================');
}

main().catch(err => {
  console.error('Qualification failed:', err);
  process.exit(1);
});
