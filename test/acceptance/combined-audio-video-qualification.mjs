// Combined 32-channel audio + 1080p60 video load collector with a strict
// verdict (research plan 2026-09-29 Revision 10, Priority 2; finding
// SYNC-CR-004). The collector separates its ledgers and never derives a
// qualification number from a submission count, a polling rate or a
// summary hash:
//   - submissions: every scheduled producer slot, from the paced
//     producer's own per-slot JSONL records (sent and skipped alike), with
//     complete second buckets including zero-submission seconds;
//   - queue acceptance: the daemon's accepted/dropped counters as read by
//     the producer - telemetry about the ingest queue, explicitly not
//     delivery;
//   - audio polling: read counts and round-trip times - a polling rate,
//     explicitly not delivery;
//   - audio sample integrity: every (channel, frame) sample of the
//     synthetic audio_32_tones fixture checked against its deterministic
//     value, plus cursor continuity and a wall-clock production ledger;
//   - final pixels: absent on this path. The test server's TestPublisher
//     accepts frames into a queue; nothing decodes final rendered pixels
//     here, so the delivery criteria are reported as blocked, never
//     guessed, and the overall verdict is NOT QUALIFIED.
// The verdict logic lives in ./lib/qualification-verdict.mjs and is
// exercised by negative controls in
// combined-audio-video-qualification.test.mjs (injected duplicate,
// omitted identity, torn frame, swapped channel, sample gap, zero second,
// missed wall-clock production): every control fails the verdict.
//
// Schema v2: v1's deliveredFps (a submission count presented as delivery)
// and its first/last-trimmed complete-second statistics are removed;
// buckets now cover every complete second of the window and the partial
// tail is reported instead of dropped.
import { spawn } from 'node:child_process';
import { existsSync, unlinkSync, readFileSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { upgrade } from '../soak/lib/ws.mjs';
import { residentKbAsync, footprintKbAsync } from '../soak/lib/process-metrics.mjs';
import {
  AUDIO_SOURCES, buildSlotLedger, combinedVerdict, evaluateAudio, evaluateVideo,
  parseProducerRecords, verifyTonesPacket,
} from './lib/qualification-verdict.mjs';

const ROOT = fileURLToPath(new URL('../../', import.meta.url));
const SERVER_BIN = path.resolve(ROOT, 'build/sync_audio_test_server');
const PRODUCER_BIN = path.resolve(ROOT, '../scaffold/apps/sync-soak/bin/paced-producer.mjs');
// The deterministic per-channel tone fixture: every (channel, frame) sample
// has one expected value, so channel identity is checked from sample
// values, not from a cursor. Tagged synthetic by the verdict.
const AUDIO_SOURCE = 'audio_32_tones';

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

function printVerdict(armName, verdict) {
  console.log(`\nVerdict [${armName}]: ${verdict.verdict}`);
  for (const name of verdict.failed) {
    console.log(`  FAILED criterion: ${name}`);
  }
  for (const name of verdict.blockedCriteria) {
    console.log(`  blocked criterion: ${name}`);
  }
  for (const name of verdict.pending) {
    console.log(`  pending criterion: ${name}`);
  }
  for (const blocker of verdict.blockers) {
    console.log(`  blocker: ${blocker}`);
  }
  if (verdict.failed.length === 0 && verdict.blockers.length === 0) {
    console.log('  (no failed criteria and no blockers recorded)');
  }
}

async function runArm({ armName, durationSeconds, runAudio, runVideo }) {
  console.log(`\n========================================`);
  console.log(`Starting Arm: ${armName} (duration: ${durationSeconds}s / ${(durationSeconds / 60).toFixed(1)} min)`);
  console.log(`Audio: ${runAudio ? `ON (${AUDIO_SOURCE}, 32-ch, 48 kHz float32)` : 'OFF'}`);
  console.log(`Video: ${runVideo ? 'ON (1080p60 uncompressed RGBA8)' : 'OFF'}`);
  console.log(`========================================`);

  const { proc: serverProc, port, pid } = await startServer();
  const telemetrySeries = [];
  let samplingActive = true;

  const audioMetrics = {
    totalReads: 0,
    emptyReads: 0,
    normalReads: 0,
    strayText: 0,
    rttMs: [],
    slowReads10ms: 0,
    slowReads20ms: 0,
  };
  // Every readAudioSource reply, verified against the fixture: the audio
  // sample ledger.
  const audioPackets = [];
  let firstDataMs = null;
  let lastDataMs = null;

  let audioActive = false;
  let audioPromise = Promise.resolve();

  if (runAudio) {
    audioActive = true;
    const audioControl = await upgrade(port, '/control', { origin: 'http://127.0.0.1:8000' });
    audioControl.sendText(JSON.stringify({ type: 'hello', token: 'soak-token', protocolVersions: [1] }));
    await audioControl.nextJson();
    audioControl.sendText(JSON.stringify({ type: 'openAudioSource', sourceId: AUDIO_SOURCE }));
    await audioControl.nextJson();

    audioPromise = (async () => {
      while (audioActive) {
        const t0 = performance.now();
        audioControl.sendText(JSON.stringify({ type: 'readAudioSource', sourceId: AUDIO_SOURCE }));
        const resp = await audioControl.next();
        const rtt = performance.now() - t0;
        audioMetrics.rttMs.push(rtt);
        audioMetrics.totalReads++;
        if (rtt >= 10) audioMetrics.slowReads10ms++;
        if (rtt >= 20) audioMetrics.slowReads20ms++;

        if (resp.opcode !== 0x2) {
          // The control socket only answers this request; anything else is
          // counted, not interpreted as audio.
          audioMetrics.strayText++;
          continue;
        }

        const packet = verifyTonesPacket(resp.payload);
        audioPackets.push(packet);
        if (packet.frameCount === 0) {
          audioMetrics.emptyReads++;
          await new Promise(r => setTimeout(r, 2));
        } else {
          audioMetrics.normalReads++;
          const now = performance.now();
          if (firstDataMs === null) firstDataMs = now;
          lastDataMs = now;
        }
      }
      try {
        audioControl.sendText(JSON.stringify({ type: 'closeAudioSource', sourceId: AUDIO_SOURCE }));
        audioControl.close();
      } catch {}
    })();
  }

  const producerOutPath = path.join(os.tmpdir(), `producer-qual-${Date.now()}-${Math.random().toString(36).slice(2)}.jsonl`);
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
          // The producer imports its protocol client and frame header from
          // the sync checkout it runs against; name this one so the run is
          // reproducible anywhere this checkout exists.
          PRODUCER_SYNC_DIR: ROOT,
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
  const audioFramesSoFar = () => {
    let total = 0;
    for (const packet of audioPackets) total += packet.frameCount;
    return total;
  };
  const audioDropsSoFar = () => audioPackets.reduce((max, p) => Math.max(max, p.droppedFrames), 0);
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
          audioFrames: audioFramesSoFar(),
          audioDrops: audioDropsSoFar(),
          audioSlowReads10ms: audioMetrics.slowReads10ms,
        };
        telemetrySeries.push(sample);
        if (elapsedSec > 0 && elapsedSec % 30 === 0) {
          console.log(`[Progress ${armName} @ ${elapsedSec}s/${durationSeconds}s]: RSS=${rss} KiB, Footprint=${footprint} KiB, AudioFrames=${sample.audioFrames}, AudioDrops=${sample.audioDrops}, AudioSlowReads10ms=${sample.audioSlowReads10ms}`);
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

  let slotLedger = null;
  let queueAcceptance = null;
  let producerErrors = [];
  if (runVideo && existsSync(producerOutPath)) {
    const content = readFileSync(producerOutPath, 'utf8');
    const records = parseProducerRecords(content.split('\n'));
    producerErrors = records.errors;
    if (records.start) {
      try {
        slotLedger = buildSlotLedger(records);
      } catch {
        slotLedger = null;
      }
    }
    if (records.summary) {
      // The daemon's ingest-queue counters as the producer read them:
      // telemetry about queue acceptance, not delivery.
      queueAcceptance = {
        accepted: records.summary.accepted,
        dropped: records.summary.dropped,
        rejected: null,
        note: 'daemon ingest queue counters (getStats); queue acceptance is not delivery',
        perSecondSamples: records.statsSamples.length,
      };
    }
    try { unlinkSync(producerOutPath); } catch {}
  }

  serverProc.kill('SIGTERM');
  await new Promise(r => setTimeout(r, 200));
  try { serverProc.kill('SIGKILL'); } catch {}

  audioMetrics.rttMs.sort((a, b) => a - b);
  const totalAudioFrames = audioPackets.reduce((sum, packet) => sum + packet.frameCount, 0);
  const productionWindowMs = firstDataMs !== null ? lastDataMs - firstDataMs : null;
  const audioSummary = runAudio ? {
    fixture: AUDIO_SOURCE,
    sourceKind: AUDIO_SOURCES[AUDIO_SOURCE].kind,
    totalReads: audioMetrics.totalReads,
    emptyReads: audioMetrics.emptyReads,
    normalReads: audioMetrics.normalReads,
    strayTextReads: audioMetrics.strayText,
    totalFrames: totalAudioFrames,
    productionWindowMs: productionWindowMs === null ? null : Number(productionWindowMs.toFixed(1)),
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

  const videoSummary = runVideo ? {
    fixture: 'paced-producer 1920x1080 RGBA8 markers',
    scheduledSlots: slotLedger ? slotLedger.scheduledSlots : 0,
    submittedSlots: slotLedger ? slotLedger.submittedSlots : 0,
    skippedSlots: slotLedger ? slotLedger.skippedSlots : 0,
    // Submission telemetry only; the offered rate is not delivery.
    submittedPerSecond: slotLedger ? slotLedger.submittedPerSecond : 0,
    offeredFraction: slotLedger ? slotLedger.offeredFraction : 0,
  } : null;

  const videoVerdict = evaluateVideo({
    slotLedger,
    finalPixels: null, // no final-pixel decode exists on this path
    queueAcceptance: queueAcceptance ? `${queueAcceptance.accepted} accepted / ${queueAcceptance.dropped} dropped` : 'no queue stats',
    ranVideo: runVideo,
  });
  if (producerErrors.length > 0 && runVideo) {
    videoVerdict.criteria.unshift({
      name: 'producer_record_errors', status: 'fail',
      detail: `${producerErrors.length} unparseable or error record(s) in the producer ledger`,
      measured: { errors: producerErrors.slice(0, 5) },
    });
  }
  const audioVerdict = evaluateAudio({
    packets: runAudio ? audioPackets : null,
    ranAudio: runAudio,
    production: productionWindowMs === null ? null : { windowMs: productionWindowMs, totalFrames: totalAudioFrames },
  });
  const verdict = combinedVerdict({ video: videoVerdict, audio: audioVerdict });

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
    ledgers: {
      submissions: slotLedger,
      queueAcceptance,
      audioPolling: runAudio ? {
        note: 'read counts and round-trip times; a polling rate is not delivery',
        totalReads: audioMetrics.totalReads,
        emptyReads: audioMetrics.emptyReads,
        slowReads10ms: audioMetrics.slowReads10ms,
        slowReads20ms: audioMetrics.slowReads20ms,
      } : null,
      audioPackets: runAudio ? {
        note: 'every read verified against the deterministic fixture values',
        packetCount: audioPackets.length,
        checkedSamples: audioPackets.reduce((sum, packet) => sum + packet.checkedSamples, 0),
      } : null,
      finalPixels: null,
    },
    audio: audioSummary,
    video: videoSummary,
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
    verdict,
  };

  console.log(`\nArm Summary: ${armName}`);
  if (runAudio) {
    console.log(`  Audio: fixture=${audioSummary.fixture} (${audioSummary.sourceKind}), frames=${audioSummary.totalFrames}, productionWindow=${audioSummary.productionWindowMs}ms, drops=${audioDropsSoFar()}, p50=${audioSummary.rttPercentilesMs.p50}ms, p95=${audioSummary.rttPercentilesMs.p95}ms, max=${audioSummary.rttPercentilesMs.max}ms`);
  }
  if (runVideo && slotLedger) {
    console.log(`  Video: scheduled=${slotLedger.scheduledSlots}, submitted=${slotLedger.submittedSlots}, skipped=${slotLedger.skippedSlots} (offered ${slotLedger.submittedPerSecond}/s; submissions are not delivery)`);
    console.log(`  Seconds: complete=${slotLedger.completeSeconds}, zeroSubmission=${slotLedger.zeroDeliverySeconds}, below50=${slotLedger.secondsBelow50}, tailSlots=${slotLedger.tailSlotCount}`);
  }
  if (runVideo && queueAcceptance) {
    console.log(`  Queue acceptance (not delivery): accepted=${queueAcceptance.accepted}, dropped=${queueAcceptance.dropped}`);
  }
  console.log(`  Memory: RSS ${initialRss} -> ${finalRss} KiB (Peak ${peakRss}), Footprint ${initialFootprint} -> ${finalFootprint} KiB (Peak ${peakFootprint}, Slope ${footprintSlopeKbPerMin} KiB/min)`);
  printVerdict(armName, verdict);

  return armResult;
}

async function main() {
  const { baselineDuration, combinedDuration, withReverse, outPath } = parseArgs();
  console.log(`Running Combined Audio + Video Load Collector`);
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

  const combinedArm = results.find(r => r.arm === 'arm_combined_load');
  const output = {
    schema: 'sync-combined-audio-video-qualification-v2',
    suite: 'Sync 1080p60 Combined Audio (32-ch) + Video Load Collector',
    timestampUtc: new Date().toISOString(),
    host: {
      platform: process.platform,
      arch: process.arch,
      nodeVersion: process.version,
    },
    baselineDurationSeconds: baselineDuration,
    combinedDurationSeconds: combinedDuration,
    overallVerdict: combinedArm ? combinedArm.verdict : null,
    arms: results,
  };

  if (outPath) {
    writeFileSync(outPath, JSON.stringify(output, null, 2));
    console.log(`\nResults written to ${outPath}`);
  }

  console.log('\n========================================');
  console.log('COLLECTOR EXECUTION COMPLETE');
  if (output.overallVerdict) {
    console.log(`Overall verdict (arm_combined_load): ${output.overallVerdict.verdict}`);
    for (const blocker of output.overallVerdict.blockers) console.log(`  blocker: ${blocker}`);
    for (const name of output.overallVerdict.failed) console.log(`  FAILED criterion: ${name}`);
  }
  console.log('========================================');
}

main().catch(err => {
  console.error('Collector failed:', err);
  process.exit(1);
});
