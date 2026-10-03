// Negative controls for the strict combined-collector verdict (research
// plan 2026-09-29 Revision 10, Priority 2; finding SYNC-CR-004). Every
// injected defect must fail the verdict: a duplicate identity, an omitted
// identity, a torn final frame, an out-of-order identity, a zero-delivery
// second, a swapped audio channel, a cursor gap, server-reported drops,
// missed wall-clock production, a trailing slot-record omission, a
// wrong-format packet mixed with valid ones, startup/tail audio
// underproduction, and out-of-range final-pixel identities that must not
// inflate the delivery numerator. A clean synthetic ledger produces no
// failed
// criterion, and the standing blockers (synthetic audio fixture,
// unadopted jitter epsilon, no final-pixel ledger on the collector path)
// keep the overall verdict NOT QUALIFIED - which is the honest state of
// this evidence, never a reason to soften a criterion.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  AUDIO_SOURCES, buildSlotLedger, combinedVerdict, evaluateAudio, evaluateVideo,
  parseAudioPacket, parseProducerRecords, verifyTonesPacket,
} from './lib/qualification-verdict.mjs';

const FPS = 60;
const PERIOD_NS = 16_666_666n; // periodNsFor(60): round(1e9 / 60)
const T0_NS = 1_000_000_000n;
const SLOTS = 600; // 10 s at 60 Hz

// Deadline_k = t0 + k*period, the producer's own schedule rule, recomputed
// here independently of the module under test.
const secondOfSeq = (seq) => Number((T0_NS + PERIOD_NS * BigInt(seq) - T0_NS) / 1_000_000_000n);

function slotLine(seq, skipped = false) {
  const deadlineNs = T0_NS + PERIOD_NS * BigInt(seq);
  return JSON.stringify({
    type: skipped ? 'skip' : 'frame',
    seq,
    deadlineNs: deadlineNs.toString(),
    submitNs: deadlineNs.toString(),
    submitEpochMs: 1_000 + Math.floor(seq * 16.667),
    writableLengthBefore: 0,
    blockedNs: '0',
    ...(skipped ? { skipped: true, reason: 'backpressure' } : { skipped: false, writeDoneNs: (deadlineNs + 100_000n).toString() }),
  });
}

function producerLines({ skips = new Set(), summarySeconds = 10 } = {}) {
  const lines = [JSON.stringify({
    type: 'start', epochMs: 1000, hrtimeNs: T0_NS.toString(), anchorMethod: 'ms-edge',
    width: 1920, height: 1080, fps: FPS, periodNs: PERIOD_NS.toString(),
    t0Ns: T0_NS.toString(), senderId: 'sender-test', name: 'Paced producer',
    pixelFormatField: 3, alphaModeField: 1, colorSpaceField: 1, frameBytes: 8_294_464,
    nice: 0, node: process.version, pid: null, port: null, origin: null, seconds: summarySeconds,
    source: null, pacing: {}, semantics: {},
  })];
  const slots = [];
  for (let seq = 0; seq < SLOTS; seq += 1) {
    lines.push(slotLine(seq, skips.has(seq)));
    slots.push({ seq, skipped: skips.has(seq) });
  }
  let sent = 0;
  let skipped = 0;
  for (const slot of slots) slot.skipped ? skipped += 1 : sent += 1;
  lines.push(JSON.stringify({
    type: 'summary', framesSent: sent, framesSkipped: skipped, framesScheduled: SLOTS,
    accepted: sent, dropped: 0, scheduleErrorUs: { p50: 1, p95: 2, p99: 3, max: 4 },
    writeDurationUs: { p50: 1, p95: 2, p99: 3, max: 4 }, blockedUs: { p50: 1, p95: 2, p99: 3, max: 4 },
    firstFrame: null, skipsMaskedByBlock: 0, prepaintDeferred: 0,
    seconds: summarySeconds, stopReason: 'seconds', error: null, cpu: null, resources: null,
  }));
  return { lines, slots };
}

function ledgerFromLines(lines) {
  const records = parseProducerRecords(lines);
  assert.equal(records.errors.length, 0, `parse errors: ${records.errors.join('; ')}`);
  return buildSlotLedger(records);
}

function cleanFinalPixels({ ok = true } = {}) {
  const entries = [];
  for (let seq = 0; seq < SLOTS; seq += 1) {
    entries.push({ sequence: seq, ok, arrivalMs: seq * (1000 / FPS) });
  }
  return entries;
}

function makeTonesPacket({ firstFrame = 0, frames = 120, channels = 32, rate = 48000, dropped = 0, mutate = null } = {}) {
  const buffer = Buffer.alloc(32 + channels * frames * 4);
  // The wire magic is the big-endian bytes "NAUD", like the daemon writes
  // and the browser client's DataView.getUint32(0) reads.
  buffer.writeUInt32BE(0x4e415544, 0);
  buffer.writeUInt16LE(1, 4);
  buffer.writeUInt16LE(channels, 6);
  buffer.writeUInt32LE(rate, 8);
  buffer.writeUInt32LE(frames, 12);
  buffer.writeBigUInt64LE(BigInt(firstFrame), 16);
  buffer.writeBigUInt64LE(BigInt(dropped), 24);
  for (let frame = 0; frame < frames; frame += 1) {
    for (let channel = 0; channel < channels; channel += 1) {
      const index = 32 + (frame * channels + channel) * 4;
      const expected = AUDIO_SOURCES.audio_32_tones.expected(channel, firstFrame + frame);
      buffer.writeFloatLE(mutate ? mutate(channel, firstFrame + frame, expected) : expected, index);
    }
  }
  return buffer;
}

const failedNames = (result) => result.criteria.filter((item) => item.status === 'fail').map((item) => item.name);
const verdictFor = (video, audio) => combinedVerdict({ video, audio });

test('verifyTonesPacket accepts the deterministic fixture and rejects corrupted samples', () => {
  const clean = verifyTonesPacket(makeTonesPacket({ firstFrame: 48_000, frames: 120 }));
  assert.equal(clean.mismatches, 0);
  assert.equal(clean.nonfinite, 0);
  assert.equal(clean.channelCount, 32);
  assert.equal(clean.sampleRate, 48000);
  assert.equal(clean.firstFrame, 48_000);
  assert.equal(clean.checkedSamples, 32 * 120);
  assert.ok(clean.worstAbsError < 1e-5, `worst error ${clean.worstAbsError}`);

  // Swapped channels carry each other's tone: nearly every frame of both
  // channels mismatches its expected value (a few samples near a shared
  // zero crossing fall inside the tolerance).
  const swapped = verifyTonesPacket(makeTonesPacket({
    frames: 60,
    mutate: (channel, frame, expected) =>
      (channel === 2 ? AUDIO_SOURCES.audio_32_tones.expected(3, frame)
        : channel === 3 ? AUDIO_SOURCES.audio_32_tones.expected(2, frame)
        : expected),
  }));
  assert.ok(swapped.mismatches >= 100, `mismatches ${swapped.mismatches}`);
  assert.equal(swapped.nonfinite, 0);

  const nonfinite = verifyTonesPacket(makeTonesPacket({
    frames: 10,
    mutate: (channel, frame, expected) => (channel === 7 && frame === 3 ? NaN : expected),
  }));
  assert.ok(nonfinite.nonfinite >= 1);
  // The NaN sample is not also counted as a mismatch.
  assert.equal(nonfinite.mismatches, 0);

  const wrongChannels = verifyTonesPacket(makeTonesPacket({ frames: 10, channels: 24 }));
  assert.equal(wrongChannels.fixtureMismatch, true);
  assert.equal(wrongChannels.checkedSamples, 0);
});

test('parseAudioPacket rejects malformed packets', () => {
  assert.throws(() => parseAudioPacket(Buffer.alloc(31)), /shorter than its header/);
  const badMagic = makeTonesPacket({ frames: 1 });
  badMagic.writeUInt32BE(0xdeadbeef, 0);
  assert.throws(() => parseAudioPacket(badMagic), /unsupported audio packet/);
  const badLength = makeTonesPacket({ frames: 2 });
  badLength.writeUInt32LE(3, 12); // claims 3 frames, holds 2
  assert.throws(() => parseAudioPacket(badLength), /invalid audio packet length/);
});

test('the submission ledger keeps every complete second, including zero-submission seconds', () => {
  const { lines } = producerLines();
  const ledger = ledgerFromLines(lines);
  assert.equal(ledger.scheduledSlots, SLOTS);
  assert.equal(ledger.submittedSlots, SLOTS);
  assert.equal(ledger.skippedSlots, 0);
  assert.equal(ledger.completeSeconds, 10);
  assert.equal(ledger.secondBuckets.length, 10);
  assert.equal(ledger.secondBuckets.reduce((sum, bucket) => sum + bucket.submitted, 0), SLOTS);
  assert.equal(ledger.zeroDeliverySeconds, 0);
  assert.equal(ledger.secondsBelow50, 0);
  assert.equal(ledger.duplicateSeqRecords, 0);
  assert.equal(ledger.missingSeqRecords, 0);
  assert.equal(ledger.offeredFraction, 1);
  assert.equal(ledger.submittedPerSecond, 60);
});

test('a fully skipped second is counted, not hidden as an absent bucket', () => {
  const secondToKill = 4;
  const skips = new Set();
  for (let seq = 0; seq < SLOTS; seq += 1) if (secondOfSeq(seq) === secondToKill) skips.add(seq);
  assert.ok(skips.size > 0);
  const { lines } = producerLines({ skips });
  const ledger = ledgerFromLines(lines);
  assert.equal(ledger.secondBuckets.length, 10);
  assert.equal(ledger.zeroDeliverySeconds, 1);
  assert.equal(ledger.secondsBelow50, 1);
  assert.equal(ledger.skippedSlots, skips.size);
  assert.equal(ledger.submittedSlots, SLOTS - skips.size);
  assert.equal(ledger.secondBuckets[secondToKill].submitted, 0);
});

test('a partially covered final second is reported as a tail instead of dropped', () => {
  const { lines } = producerLines({ summarySeconds: 9.983 });
  const ledger = ledgerFromLines(lines);
  assert.equal(ledger.completeSeconds, 9);
  const expectedTail = Array.from({ length: SLOTS }, (_, seq) => seq).filter((seq) => secondOfSeq(seq) >= 9).length;
  assert.ok(expectedTail > 0);
  assert.equal(ledger.tailSlotCount, expectedTail);
  assert.equal(ledger.secondBuckets.length, 9);
});

test('a torn producer ledger fails slot ledger integrity', () => {
  const { lines } = producerLines();
  const dropped = lines.slice();
  // Remove one frame record (line 1 is the start record).
  dropped.splice(1, 1);
  const torn = parseProducerRecords(dropped);
  const tornLedger = buildSlotLedger(torn);
  assert.equal(tornLedger.missingSeqRecords, 1);
  const verdict = evaluateVideo({ slotLedger: tornLedger, finalPixels: cleanFinalPixels(), ranVideo: true });
  assert.ok(failedNames(verdict).includes('slot_ledger_integrity'));

  const duplicated = lines.slice();
  duplicated.splice(2, 0, lines[1]);
  const dupLedger = buildSlotLedger(parseProducerRecords(duplicated));
  assert.equal(dupLedger.duplicateSeqRecords, 1);
  const dupVerdict = evaluateVideo({ slotLedger: dupLedger, finalPixels: cleanFinalPixels(), ranVideo: true });
  assert.ok(failedNames(dupVerdict).includes('slot_ledger_integrity'));
});

test('negative control: trailing omitted slot records fail integrity and cannot shrink the denominator', () => {
  // Drop the last 60 frame records while the summary still schedules 600
  // slots: the ledger must count them as missing against framesScheduled
  // and must keep the delivery denominator at 600.
  const { lines } = producerLines();
  const trailing = lines.slice(0, 1 + SLOTS - 60);
  trailing.push(lines[lines.length - 1]);
  const ledger = buildSlotLedger(parseProducerRecords(trailing));
  assert.equal(ledger.scheduledSlots, SLOTS);
  assert.equal(ledger.scheduledBySummary, true);
  assert.equal(ledger.receivedSlotRecords, SLOTS - 60);
  assert.equal(ledger.missingSeqRecords, 60);
  // A receiver that only ever saw the 540 submitted slots still has its
  // delivery fraction judged against all 600 scheduled slots.
  const pixels = cleanFinalPixels().slice(0, SLOTS - 60);
  const video = evaluateVideo({ slotLedger: ledger, finalPixels: pixels, ranVideo: true });
  const names = failedNames(video);
  assert.ok(names.includes('slot_ledger_integrity'));
  assert.ok(names.includes('delivery_fraction'));
  const fraction = video.criteria.find((item) => item.name === 'delivery_fraction');
  assert.equal(fraction.measured.denominator, SLOTS);
  assert.ok(fraction.measured.fraction < 0.99);
  assert.equal(verdictFor(video, null).qualified, false);
});

test('negative control: slot records beyond the scheduled range fail integrity', () => {
  const { lines } = producerLines();
  const extra = lines.slice();
  // Insert a record for a sequence the producer never scheduled (before
  // the summary line).
  extra.splice(extra.length - 1, 0, slotLine(SLOTS + 5));
  const ledger = buildSlotLedger(parseProducerRecords(extra));
  assert.equal(ledger.unscheduledSeqRecords, 1);
  const video = evaluateVideo({ slotLedger: ledger, finalPixels: cleanFinalPixels(), ranVideo: true });
  assert.ok(failedNames(video).includes('slot_ledger_integrity'));
  assert.equal(verdictFor(video, null).qualified, false);
});

test('a clean synthetic video ledger passes every criterion and still cannot qualify', () => {
  const { lines } = producerLines();
  const ledger = ledgerFromLines(lines);
  const video = evaluateVideo({ slotLedger: ledger, finalPixels: cleanFinalPixels(), ranVideo: true });
  assert.deepEqual(failedNames(video), []);
  assert.deepEqual(video.criteria.filter((item) => item.status === 'blocked').map((item) => item.name), []);
  const fraction = video.criteria.find((item) => item.name === 'delivery_fraction');
  assert.equal(fraction.status, 'pass');
  assert.equal(fraction.measured.fraction, 1);
  const jitter = video.criteria.find((item) => item.name === 'jitter_within_epsilon');
  assert.equal(jitter.status, 'pending');
  assert.equal(jitter.measured.epsilon, null);
  assert.ok(jitter.measured.intervals >= SLOTS - 1);
  assert.deepEqual(video.blockers, ['jitter epsilon not adopted: closure verdicts stay invalid until the versioned decision exists (SYNC-CR-005)']);
});

test('negative control: an injected duplicate identity fails the verdict', () => {
  const { lines } = producerLines();
  const ledger = ledgerFromLines(lines);
  const pixels = cleanFinalPixels();
  pixels.splice(100, 0, { ...pixels[99] });
  const video = evaluateVideo({ slotLedger: ledger, finalPixels: pixels, ranVideo: true });
  assert.ok(failedNames(video).includes('duplicate_identities'));
  const verdict = verdictFor(video, null);
  assert.equal(verdict.qualified, false);
});

test('negative control: omitted identities fail the delivery fraction against the full denominator', () => {
  const { lines } = producerLines();
  const ledger = ledgerFromLines(lines);
  const pixels = cleanFinalPixels();
  const omitted = pixels.splice(300, 10);
  assert.equal(omitted.length, 10);
  const video = evaluateVideo({ slotLedger: ledger, finalPixels: pixels, ranVideo: true });
  const names = failedNames(video);
  assert.ok(names.includes('delivery_fraction'));
  const fraction = video.criteria.find((item) => item.name === 'delivery_fraction');
  assert.equal(fraction.measured.uniqueDelivered, 590);
  assert.equal(fraction.measured.denominator, 600);
  assert.ok(fraction.measured.fraction < 0.99);
  assert.equal(verdictFor(video, null).qualified, false);
});

test('negative control: out-of-range identities cannot inflate the delivery numerator', () => {
  const { lines } = producerLines();
  const ledger = ledgerFromLines(lines);
  const pixels = cleanFinalPixels();
  // Ten scheduled identities are never delivered; ten unique identities
  // beyond the scheduled range (SLOTS..SLOTS+9, each with its own
  // arrival) are appended in their place. A numerator that counts every
  // unique sequence scores this ledger 600/600 and passes; the verdict
  // must reject the unscheduled identities and keep the numerator at
  // the in-range deliveries only.
  pixels.splice(300, 10);
  for (let offset = 0; offset < 10; offset += 1) {
    pixels.push({ sequence: SLOTS + offset, ok: true, arrivalMs: 5000 + offset * (1000 / FPS) });
  }
  const video = evaluateVideo({ slotLedger: ledger, finalPixels: pixels, ranVideo: true });
  const names = failedNames(video);
  assert.ok(names.includes('delivery_fraction'));
  const fraction = video.criteria.find((item) => item.name === 'delivery_fraction');
  assert.equal(fraction.measured.uniqueDelivered, 590);
  assert.equal(fraction.measured.denominator, SLOTS);
  assert.equal(fraction.measured.unscheduledIdentities, 10);
  assert.ok(fraction.measured.fraction < 0.99);
  assert.equal(verdictFor(video, null).qualified, false);
});

test('negative control: a final-pixel ledger of only out-of-range identities delivers nothing', () => {
  const { lines } = producerLines();
  const ledger = ledgerFromLines(lines);
  const pixels = [];
  for (let seq = 0; seq < SLOTS; seq += 1) {
    pixels.push({ sequence: SLOTS + seq, ok: true, arrivalMs: seq * (1000 / FPS) });
  }
  const video = evaluateVideo({ slotLedger: ledger, finalPixels: pixels, ranVideo: true });
  const names = failedNames(video);
  assert.ok(names.includes('delivery_fraction'));
  assert.ok(names.includes('zero_delivery_seconds'));
  const fraction = video.criteria.find((item) => item.name === 'delivery_fraction');
  assert.equal(fraction.measured.uniqueDelivered, 0);
  assert.equal(fraction.measured.denominator, SLOTS);
  assert.equal(fraction.measured.unscheduledIdentities, SLOTS);
  const zero = video.criteria.find((item) => item.name === 'zero_delivery_seconds');
  assert.equal(zero.measured.zeroDeliverySeconds, ledger.completeSeconds);
  assert.equal(verdictFor(video, null).qualified, false);
});

test('negative control: a torn final frame fails the verdict', () => {
  const { lines } = producerLines();
  const ledger = ledgerFromLines(lines);
  const pixels = cleanFinalPixels();
  pixels[250] = { ...pixels[250], ok: false };
  const video = evaluateVideo({ slotLedger: ledger, finalPixels: pixels, ranVideo: true });
  assert.ok(failedNames(video).includes('torn_frames'));
  assert.equal(verdictFor(video, null).qualified, false);
});

test('negative control: an out-of-order identity fails the verdict', () => {
  const { lines } = producerLines();
  const ledger = ledgerFromLines(lines);
  const pixels = cleanFinalPixels();
  const a = pixels[200];
  pixels[200] = pixels[201];
  pixels[201] = a;
  const video = evaluateVideo({ slotLedger: ledger, finalPixels: pixels, ranVideo: true });
  assert.ok(failedNames(video).includes('identity_order'));
  assert.equal(verdictFor(video, null).qualified, false);
});

test('negative control: a zero-delivery second fails the verdict', () => {
  const { lines } = producerLines();
  const ledger = ledgerFromLines(lines);
  const pixels = cleanFinalPixels().filter((entry) => Math.floor(entry.arrivalMs / 1000) !== 5);
  const video = evaluateVideo({ slotLedger: ledger, finalPixels: pixels, ranVideo: true });
  assert.ok(failedNames(video).includes('zero_delivery_seconds'));
  const zero = video.criteria.find((item) => item.name === 'zero_delivery_seconds');
  assert.equal(zero.measured.zeroDeliverySeconds, 1);
  assert.equal(verdictFor(video, null).qualified, false);
});

test('without a final-pixel ledger the delivery criteria are blocked, never guessed', () => {
  const { lines } = producerLines();
  const ledger = ledgerFromLines(lines);
  const video = evaluateVideo({ slotLedger: ledger, finalPixels: null, ranVideo: true });
  const blocked = video.criteria.filter((item) => item.status === 'blocked').map((item) => item.name);
  for (const name of ['delivery_fraction', 'duplicate_identities', 'torn_frames', 'identity_order', 'zero_delivery_seconds']) {
    assert.ok(blocked.includes(name), `${name} not blocked`);
  }
  assert.deepEqual(failedNames(video), []);
  assert.ok(video.blockers.some((blocker) => blocker.includes('no final-pixel identity ledger')));
  const verdict = verdictFor(video, null);
  assert.equal(verdict.qualified, false);
  assert.equal(verdict.verdict, 'NOT QUALIFIED');
  assert.ok(verdict.blockers.some((blocker) => blocker.includes('no final-pixel identity ledger')));
});

test('a clean synthetic audio ledger passes its criteria and is tagged synthetic', () => {
  const packets = [];
  for (let index = 0; index < 100; index += 1) {
    packets.push(verifyTonesPacket(makeTonesPacket({ firstFrame: index * 480, frames: 480 })));
  }
  const audio = evaluateAudio({
    packets,
    production: { windowMs: 1000, totalFrames: 100 * 480 },
    ranAudio: true,
  });
  assert.deepEqual(failedNames(audio), []);
  assert.equal(audio.criteria.find((item) => item.name === 'channel_count').status, 'pass');
  assert.equal(audio.criteria.find((item) => item.name === 'sample_values').status, 'pass');
  assert.equal(audio.criteria.find((item) => item.name === 'cursor_continuity').status, 'pass');
  assert.equal(audio.criteria.find((item) => item.name === 'wall_clock_production').status, 'pass');
  assert.deepEqual(audio.blockers, ['synthetic audio fixture: a synthetic audio_32_tones source cannot qualify the 32 physical channels the acceptance target requires']);
});

test('negative control: a swapped audio channel fails the sample-value criterion', () => {
  const packets = [
    verifyTonesPacket(makeTonesPacket({ firstFrame: 0, frames: 480 })),
    verifyTonesPacket(makeTonesPacket({
      firstFrame: 480, frames: 240,
      mutate: (channel, frame, expected) =>
        (channel === 4 ? AUDIO_SOURCES.audio_32_tones.expected(9, frame)
          : channel === 9 ? AUDIO_SOURCES.audio_32_tones.expected(4, frame)
          : expected),
    })),
  ];
  const audio = evaluateAudio({ packets, production: { windowMs: 500, totalFrames: 720 }, ranAudio: true });
  assert.ok(failedNames(audio).includes('sample_values'));
  assert.equal(verdictFor(null, audio).qualified, false);
});

test('negative control: a cursor gap fails continuity even with intact sample values', () => {
  const packets = [
    verifyTonesPacket(makeTonesPacket({ firstFrame: 0, frames: 480 })),
    verifyTonesPacket(makeTonesPacket({ firstFrame: 1440, frames: 480 })),
  ];
  const audio = evaluateAudio({ packets, production: { windowMs: 500, totalFrames: 960 }, ranAudio: true });
  assert.ok(failedNames(audio).includes('cursor_continuity'));
  assert.equal(audio.criteria.find((item) => item.name === 'sample_values').status, 'pass');
});

test('negative control: server-reported drops fail the verdict', () => {
  const packets = [verifyTonesPacket(makeTonesPacket({ firstFrame: 0, frames: 480, dropped: 3 }))];
  const audio = evaluateAudio({ packets, production: { windowMs: 10, totalFrames: 480 }, ranAudio: true });
  assert.ok(failedNames(audio).includes('server_reported_drops'));
});

test('negative control: a wrong-format packet mixed with valid packets fails the format and sample criteria', () => {
  // A 24-channel packet between valid 32-channel packets: aggregate
  // maxima would still report 32 channels and pass; every nonempty
  // packet must match the fixture format and be fully checked.
  const packets = [
    verifyTonesPacket(makeTonesPacket({ firstFrame: 0, frames: 480 })),
    verifyTonesPacket(makeTonesPacket({ firstFrame: 480, frames: 240, channels: 24 })),
    verifyTonesPacket(makeTonesPacket({ firstFrame: 720, frames: 240 })),
  ];
  const audio = evaluateAudio({
    packets,
    production: { windowMs: 1000, totalFrames: 480 + 240 + 240 },
    ranAudio: true,
  });
  const names = failedNames(audio);
  assert.ok(names.includes('channel_count'));
  assert.ok(names.includes('sample_values'));
  const sampleValues = audio.criteria.find((item) => item.name === 'sample_values');
  assert.ok(sampleValues.measured.packetsNotFullyChecked >= 1, 'mixed packet not reported as unchecked');
  assert.equal(verdictFor(null, audio).qualified, false);

  // A wrong-rate packet fails the rate criterion the same way even with
  // the right channel count.
  const wrongRate = [
    verifyTonesPacket(makeTonesPacket({ firstFrame: 0, frames: 480 })),
    verifyTonesPacket(makeTonesPacket({ firstFrame: 480, frames: 480, rate: 44100 })),
  ];
  const rateAudio = evaluateAudio({
    packets: wrongRate,
    production: { windowMs: 1000, totalFrames: 960 },
    ranAudio: true,
  });
  const rateNames = failedNames(rateAudio);
  assert.ok(rateNames.includes('sample_rate'));
  assert.ok(rateNames.includes('sample_values'));
});

test('negative control: startup and tail underproduction fail the full-window production ledger', () => {
  // The production window is the run's steady-state window, not the
  // first-to-last-data span: frames missing before the first nonempty
  // read (startup stall) or after the last one (tail stall) are missing
  // production. 48000 frames span 1000 ms; a window of 1000 ms with a
  // 100 ms startup gap produces only 90%.
  const packets = [];
  for (let index = 0; index < 90; index += 1) {
    packets.push(verifyTonesPacket(makeTonesPacket({ firstFrame: index * 480, frames: 480 })));
  }
  const startup = evaluateAudio({
    packets,
    production: { windowMs: 1000, totalFrames: 90 * 480 },
    ranAudio: true,
  });
  const startupProduction = startup.criteria.find((item) => item.name === 'wall_clock_production');
  assert.equal(startupProduction.status, 'fail');
  assert.equal(startupProduction.measured.expectedFrames, 48000);
  assert.ok(startupProduction.measured.ratio < 0.99);
  // The cursor stays contiguous: only the production ledger catches it.
  assert.equal(startup.criteria.find((item) => item.name === 'cursor_continuity').status, 'pass');
  assert.equal(verdictFor(null, startup).qualified, false);
});

test('negative control: missed wall-clock production fails despite a contiguous cursor', () => {
  // 80% of the expected 48,000 frames over the wall window: a producer
  // that stalled keeps its cursor contiguous, and only the production
  // ledger catches it (SYNC-CR-022).
  const packets = [];
  for (let index = 0; index < 80; index += 1) {
    packets.push(verifyTonesPacket(makeTonesPacket({ firstFrame: index * 480, frames: 480 })));
  }
  const audio = evaluateAudio({ packets, production: { windowMs: 1000, totalFrames: 80 * 480 }, ranAudio: true });
  const production = audio.criteria.find((item) => item.name === 'wall_clock_production');
  assert.equal(production.status, 'fail');
  assert.ok(production.measured.ratio < 0.99);
  assert.equal(audio.criteria.find((item) => item.name === 'cursor_continuity').status, 'pass');
  assert.equal(verdictFor(null, audio).qualified, false);
});

test('audio paths that ran without evidence fail rather than pass silently', () => {
  const ranWithoutPackets = evaluateAudio({ packets: [], ranAudio: true });
  assert.ok(failedNames(ranWithoutPackets).includes('audio_ledger'));
  const noProduction = evaluateAudio({
    packets: [verifyTonesPacket(makeTonesPacket({ frames: 480 }))],
    production: null,
    ranAudio: true,
  });
  assert.ok(failedNames(noProduction).includes('wall_clock_production'));
  const skipped = evaluateAudio({ packets: null, ranAudio: false });
  assert.deepEqual(skipped.criteria.map((item) => item.name), ['audio_ledger']);
  assert.equal(skipped.criteria[0].status, 'skipped');
});

test('video paths that ran without a slot ledger fail rather than pass silently', () => {
  const ranWithoutLedger = evaluateVideo({ slotLedger: null, ranVideo: true });
  assert.ok(failedNames(ranWithoutLedger).includes('video_ledger'));
  const skipped = evaluateVideo({ slotLedger: null, ranVideo: false });
  assert.deepEqual(skipped.criteria.map((item) => item.name), ['video_ledger']);
  assert.equal(skipped.criteria[0].status, 'skipped');
});

test('the combined verdict stays NOT QUALIFIED on clean evidence and reports every standing blocker', () => {
  const { lines } = producerLines();
  const ledger = ledgerFromLines(lines);
  const video = evaluateVideo({ slotLedger: ledger, finalPixels: cleanFinalPixels(), ranVideo: true });
  const packets = [verifyTonesPacket(makeTonesPacket({ firstFrame: 0, frames: 480 }))];
  const audio = evaluateAudio({ packets, production: { windowMs: 10, totalFrames: 480 }, ranAudio: true });
  const verdict = combinedVerdict({ video, audio });
  assert.equal(verdict.qualified, false);
  assert.equal(verdict.verdict, 'NOT QUALIFIED');
  assert.deepEqual(verdict.failed, []);
  assert.deepEqual(verdict.blockedCriteria, []);
  assert.deepEqual(verdict.pending, ['jitter_within_epsilon']);
  assert.ok(verdict.blockers.some((blocker) => blocker.includes('jitter epsilon not adopted')));
  assert.ok(verdict.blockers.some((blocker) => blocker.includes('synthetic audio fixture')));
  // No submission count leaked into the verdict as a delivery number:
  // the v1 deliveredFps trap is gone from the ledger shape.
  assert.ok(!Object.keys(ledger).some((key) => key.toLowerCase().startsWith('delivered')));
  assert.equal(AUDIO_SOURCES.audio_32_tones.kind, 'synthetic');
});
