// Strict verdict logic for the combined audio + video qualification
// collector (test/acceptance/combined-audio-video-qualification.mjs).
// Research plan 2026-09-29 Revision 10, Priority 2: the collector must
// separate telemetry from qualification, count submissions, consumer polls
// and final-pixel identities as separate ledgers, keep every complete
// second bucket including zeros, check all-channel audio sample values
// rather than cursors alone, and emit a strict pass/fail verdict. No
// qualification number may be produced from a submission count, a polling
// rate or a summary hash. Everything here is pure: the collector feeds it
// ledgers, the negative controls in
// combined-audio-video-qualification.test.mjs feed it corrupted ones.
//
// The acceptance target this verdict implements (plan section 1): at least
// 99% unique delivered frames - distinct, ordered source identities decoded
// from final rendered pixels against every 60 Hz source slot - with jitter
// within the authoritative epsilon, concurrently with all 32 audio
// channels. A native receiver count, a callback rate, queue acceptance or a
// native-only result is not delivery, so this path's own evidence limits
// (no final-pixel decode, synthetic audio fixture) are recorded as
// blockers rather than folded into a passing number.
//
// The jitter epsilon is deliberately NOT gated: SYNC-CR-005 (no
// authoritative jitter epsilon or absolute video latency bound) is
// unresolved and the 2 ms p99 proposal is unapproved, so jitter is
// reported as a pending criterion with the statistic and never decides
// the verdict. Adopting an epsilon happens by versioned decision, not
// here.

export const VERDICT_SCHEMA = 'sync-combined-qualification-verdict-v1';

// The 99% unique-delivery target of the acceptance contract.
export const DELIVERY_FRACTION_MIN = 0.99;
// Wall-clock audio production floor: deliberately missed production time
// fails source-rate acceptance even when every submitted sample is
// contiguous (SYNC-CR-022). The floor shares the 99% target so a stalled
// producer is caught once it misses more than 1% of the window; misses
// smaller than that are below the measurement floor and are reported, not
// detected.
export const PRODUCTION_RATIO_MIN = 0.99;
// The native fixture synthesizes float32 sines; the reference is computed
// in double here, so float32 rounding dominates the residual.
export const TONE_TOLERANCE = 1e-4;

export const AUDIO_SOURCES = Object.freeze({
  audio_32_tones: Object.freeze({
    kind: 'synthetic',
    channels: 32,
    sampleRate: 48000,
    // native/src/audio_native.cpp FixtureCapture, OrthogonalTones: channel c
    // (0-based) carries sin(2*pi*100*(c+1)*t) with t = frame/48000 over the
    // absolute frame counter, so every (channel, frame) pair has one
    // expected value and a swapped or shifted channel cannot pass.
    expected(channel, frame) {
      return Math.sin(2 * Math.PI * 100 * (channel + 1) * (frame / 48000));
    },
  }),
});

// ---- audio packets ---------------------------------------------------------

// Mirrors browser/client.js decodeAudioPacket over a Node Buffer: a
// 32-byte header (magic, version, channelCount, sampleRate, frameCount,
// firstFrame, droppedFrames) followed by interleaved float32 frames,
// frame-major. The magic "NAUD" is compared big-endian like the client's
// DataView.getUint32(0); every other field is little-endian. Throws on
// anything else.
export function parseAudioPacket(buffer) {
  if (!Buffer.isBuffer(buffer) || buffer.length < 32) {
    throw new Error('audio packet is shorter than its header');
  }
  if (buffer.readUInt32BE(0) !== 0x4e415544 || buffer.readUInt16LE(4) !== 1) {
    throw new Error('unsupported audio packet');
  }
  const channelCount = buffer.readUInt16LE(6);
  const sampleRate = buffer.readUInt32LE(8);
  const frameCount = buffer.readUInt32LE(12);
  if (frameCount > 480 || buffer.length !== 32 + channelCount * frameCount * 4) {
    throw new Error('invalid audio packet length');
  }
  return {
    channelCount,
    sampleRate,
    frameCount,
    firstFrame: Number(buffer.readBigUInt64LE(16)),
    droppedFrames: Number(buffer.readBigUInt64LE(24)),
  };
}

// Parses one readAudioSource packet and verifies every (channel, frame)
// sample against the tones fixture's deterministic value. Returns a packet
// summary the audio verdict consumes; a swapped channel, a time-shifted
// channel or corrupted samples appear as mismatches, a NaN as nonfinite.
export function verifyTonesPacket(buffer, { fixture = AUDIO_SOURCES.audio_32_tones, tolerance = TONE_TOLERANCE } = {}) {
  const packet = parseAudioPacket(buffer);
  let mismatches = 0;
  let nonfinite = 0;
  let worstAbsError = 0;
  let checked = 0;
  if (packet.channelCount !== fixture.channels || packet.sampleRate !== fixture.sampleRate) {
    // Still parsed: the channel_count and sample_rate criteria report the
    // observed values and fail on their own.
    return {
      ...packet, mismatches: 0, nonfinite: 0, checkedSamples: 0, worstAbsError: 0,
      fixtureMismatch: true,
    };
  }
  for (let frame = 0; frame < packet.frameCount; frame += 1) {
    const absolute = packet.firstFrame + frame;
    for (let channel = 0; channel < fixture.channels; channel += 1) {
      const sample = buffer.readFloatLE(32 + (frame * fixture.channels + channel) * 4);
      if (!Number.isFinite(sample)) {
        nonfinite += 1;
        continue;
      }
      const error = Math.abs(sample - fixture.expected(channel, absolute));
      if (error > worstAbsError) worstAbsError = error;
      if (error > tolerance) mismatches += 1;
      checked += 1;
    }
  }
  return { ...packet, mismatches, nonfinite, checkedSamples: checked, worstAbsError, fixtureMismatch: false };
}

// ---- producer JSONL --------------------------------------------------------

// One record per JSONL line of the paced producer (scaffold
// apps/sync-soak/lib/paced-producer.mjs). Nanosecond fields serialize as
// decimal strings; they are restored as BigInt here.
export function parseProducerRecords(lines) {
  let start = null;
  const slots = [];
  const statsSamples = [];
  let summary = null;
  const errors = [];
  for (const line of lines) {
    const trimmed = line.trim();
    if (!trimmed) continue;
    let record;
    try {
      record = JSON.parse(trimmed);
    } catch {
      errors.push(`unparseable line: ${trimmed.slice(0, 80)}`);
      continue;
    }
    if (record.type === 'start') {
      start = record;
    } else if (record.type === 'summary') {
      summary = record;
    } else if (record.type === 'stats') {
      statsSamples.push({ epochMs: record.epochMs, accepted: record.accepted, dropped: record.dropped });
    } else if (record.type === 'frame' || record.type === 'skip') {
      slots.push({
        seq: record.seq,
        deadlineNs: BigInt(record.deadlineNs),
        submitEpochMs: record.submitEpochMs,
        skipped: record.skipped === true,
      });
    } else if (record.type === 'error' || record.type === 'statsError') {
      errors.push(`${record.type}: ${record.error ?? 'unknown'}`);
    }
  }
  return { start, slots, statsSamples, summary, errors };
}

// Builds the submission ledger over every scheduled slot: the denominator
// every delivery fraction is judged against. Second buckets are keyed by
// each slot's absolute deadline (deadline_k = t0 + k*period, never
// re-derived), so every complete second of the window has a bucket even
// when nothing was submitted in it - a zero-delivery second can no longer
// hide as an absent array entry (the E14 defect: 1,794 buckets reported
// for a 1,800-second window). The final, partially covered second is
// reported as a tail with its slot count instead of being silently
// dropped.
export function buildSlotLedger({ start, slots, summary }) {
  if (!start || start.t0Ns === undefined || start.periodNs === undefined) {
    throw new Error('slot ledger needs the producer start record');
  }
  const t0Ns = BigInt(start.t0Ns);
  const periodNs = BigInt(start.periodNs);
  const fps = start.fps;
  const bySeq = new Map();
  let duplicateSeqRecords = 0;
  for (const slot of slots) {
    if (bySeq.has(slot.seq)) duplicateSeqRecords += 1;
    bySeq.set(slot.seq, slot);
  }
  const seqs = [...bySeq.keys()].sort((a, b) => a - b);
  const maxSeq = seqs.length > 0 ? seqs[seqs.length - 1] : -1;
  const missingSeqRecords = maxSeq + 1 - bySeq.size;
  const secondOf = (slot) => Number((slot.deadlineNs - t0Ns) / 1_000_000_000n);
  const maxSecond = maxSeq >= 0 ? secondOf(bySeq.get(maxSeq)) : -1;
  // A second is complete when the run covered its full extent: the
  // summary's measured wall seconds when present, else the last occupied
  // second is conservatively treated as partial.
  const wallSeconds = summary ? Number(summary.seconds) : maxSecond + 1;
  const completeSeconds = Math.max(0, Math.floor(wallSeconds));
  const secondBuckets = Array.from({ length: completeSeconds }, (_, second) => ({ second, submitted: 0 }));
  const tailSlots = [];
  let scheduledSlots = 0;
  let submittedSlots = 0;
  let skippedSlots = 0;
  for (const seq of seqs) {
    const slot = bySeq.get(seq);
    scheduledSlots += 1;
    const second = secondOf(slot);
    if (slot.skipped) skippedSlots += 1;
    else {
      submittedSlots += 1;
      if (second < completeSeconds) secondBuckets[second].submitted += 1;
    }
    if (second >= completeSeconds) tailSlots.push(slot.seq);
  }
  let zeroDeliverySeconds = 0;
  let secondsBelow50 = 0;
  let secondsAtOrAbove59 = 0;
  let longestRunBelow50 = 0;
  let runBelow50 = 0;
  for (const bucket of secondBuckets) {
    if (bucket.submitted === 0) zeroDeliverySeconds += 1;
    if (bucket.submitted < 50) {
      secondsBelow50 += 1;
      runBelow50 += 1;
      if (runBelow50 > longestRunBelow50) longestRunBelow50 = runBelow50;
    } else {
      runBelow50 = 0;
    }
    if (bucket.submitted >= 59) secondsAtOrAbove59 += 1;
  }
  const coveredSeconds = Math.max(wallSeconds, maxSecond + 1);
  return {
    fps,
    periodNs: periodNs.toString(),
    t0Ns: t0Ns.toString(),
    scheduledSlots,
    submittedSlots,
    skippedSlots,
    completeSeconds,
    tailSlotCount: tailSlots.length,
    secondBuckets,
    zeroDeliverySeconds,
    secondsBelow50,
    secondsAtOrAbove59,
    longestRunBelow50,
    duplicateSeqRecords,
    missingSeqRecords,
    // Submission telemetry only: the offered rate at the sender. This is
    // not delivery and no verdict criterion reads it as one.
    submittedPerSecond: coveredSeconds > 0 ? Number((submittedSlots / coveredSeconds).toFixed(3)) : 0,
    offeredFraction: scheduledSlots > 0 ? Number((submittedSlots / scheduledSlots).toFixed(5)) : 0,
  };
}

// ---- verdicts ---------------------------------------------------------------

function criterion(name, status, detail, measured) {
  return measured === undefined ? { name, status, detail } : { name, status, detail, measured };
}

// finalPixels, when a final-pixel identity ledger exists, is the list of
// deliveries observed at the final receiver in arrival order:
//   { sequence, ok, arrivalMs? }
// ok is the decoded marker's integrity verdict (a torn or corrupted final
// frame is ok: false); arrivalMs is the observer's monotonic decode time
// relative to the window origin (the first slot's deadline), which is also
// the jitter clock domain. This harness has no final-pixel decode on its
// path, so the collector passes null and the criterion is blocked, never
// guessed from submissions or queue acceptance.
export function evaluateVideo({ slotLedger, finalPixels, queueAcceptance, ranVideo = true }) {
  const criteria = [];
  const blockers = [];

  if (ranVideo !== true) {
    criteria.push(criterion('video_ledger', 'skipped', 'arm did not run the video path'));
    return { criteria, blockers };
  }
  if (slotLedger === null || slotLedger === undefined) {
    criteria.push(criterion('video_ledger', 'fail', 'the video path ran but produced no slot ledger'));
    blockers.push('video: no producer slot ledger; nothing can be judged');
    return { criteria, blockers };
  }

  const integrityProblems = [
    slotLedger.duplicateSeqRecords > 0 ? `${slotLedger.duplicateSeqRecords} duplicate slot records` : null,
    slotLedger.missingSeqRecords > 0 ? `${slotLedger.missingSeqRecords} slot records missing from the sequence` : null,
  ].filter(Boolean);
  criteria.push(criterion(
    'slot_ledger_integrity',
    integrityProblems.length === 0 ? 'pass' : 'fail',
    integrityProblems.length === 0
      ? 'every scheduled slot has exactly one record'
      : integrityProblems.join('; '),
    { duplicateSeqRecords: slotLedger.duplicateSeqRecords, missingSeqRecords: slotLedger.missingSeqRecords },
  ));

  criteria.push(criterion(
    'zero_submission_seconds',
    slotLedger.zeroDeliverySeconds === 0 ? 'pass' : 'fail',
    slotLedger.zeroDeliverySeconds === 0
      ? `no complete second of ${slotLedger.completeSeconds} submitted zero frames`
      : `${slotLedger.zeroDeliverySeconds} complete second(s) submitted zero frames`,
    { zeroSubmissionSeconds: slotLedger.zeroDeliverySeconds, completeSeconds: slotLedger.completeSeconds },
  ));

  criteria.push(criterion(
    'seconds_below_50',
    slotLedger.secondsBelow50 === 0 ? 'pass' : 'fail',
    slotLedger.secondsBelow50 === 0
      ? `no complete second below 50 submissions (longest run ${slotLedger.longestRunBelow50})`
      : `${slotLedger.secondsBelow50} complete second(s) below 50 submissions, longest run ${slotLedger.longestRunBelow50}`,
    { secondsBelow50: slotLedger.secondsBelow50, longestRunBelow50: slotLedger.longestRunBelow50 },
  ));

  if (finalPixels === null || finalPixels === undefined) {
    blockers.push('no final-pixel identity ledger on this path: queue acceptance '
      + `(${queueAcceptance ?? 'absent'}), submission counts and consumer polls cannot substitute for delivery`);
    criteria.push(criterion('delivery_fraction', 'blocked',
      'delivery is distinct ordered identities decoded from final rendered pixels; none observed'));
    criteria.push(criterion('duplicate_identities', 'blocked', 'no final-pixel ledger'));
    criteria.push(criterion('torn_frames', 'blocked', 'no final-pixel ledger'));
    criteria.push(criterion('identity_order', 'blocked', 'no final-pixel ledger'));
    criteria.push(criterion('zero_delivery_seconds', 'blocked', 'no final-pixel ledger'));
  } else {
    const deliveredSequences = new Set();
    let duplicates = 0;
    let tornFrames = 0;
    let reorderedPairs = 0;
    let lastSequence = null;
    for (const entry of finalPixels) {
      if (entry.ok !== true) {
        tornFrames += 1;
        continue;
      }
      if (deliveredSequences.has(entry.sequence)) duplicates += 1;
      else deliveredSequences.add(entry.sequence);
      if (lastSequence !== null && entry.sequence < lastSequence) reorderedPairs += 1;
      lastSequence = entry.sequence;
    }
    const denominator = slotLedger.scheduledSlots;
    const uniqueDelivered = deliveredSequences.size;
    const fraction = denominator > 0 ? uniqueDelivered / denominator : 0;
    criteria.push(criterion(
      'delivery_fraction',
      fraction >= DELIVERY_FRACTION_MIN ? 'pass' : 'fail',
      `${uniqueDelivered} of ${denominator} scheduled slots delivered uniquely `
        + `(${(fraction * 100).toFixed(3)}% vs the ${(DELIVERY_FRACTION_MIN * 100).toFixed(0)}% target)`,
      { uniqueDelivered, denominator, fraction: Number(fraction.toFixed(5)) },
    ));
    criteria.push(criterion(
      'duplicate_identities',
      duplicates === 0 ? 'pass' : 'fail',
      duplicates === 0 ? 'no identity was delivered twice' : `${duplicates} duplicate final-pixel identit${duplicates === 1 ? 'y was' : 'ies were'} delivered`,
      { duplicates },
    ));
    criteria.push(criterion(
      'torn_frames',
      tornFrames === 0 ? 'pass' : 'fail',
      tornFrames === 0 ? 'every delivered frame decoded a valid marker' : `${tornFrames} delivered frame(s) failed marker integrity`,
      { tornFrames },
    ));
    criteria.push(criterion(
      'identity_order',
      reorderedPairs === 0 ? 'pass' : 'fail',
      reorderedPairs === 0 ? 'identities arrived in source order' : `${reorderedPairs} out-of-order pair(s)`,
      { reorderedPairs },
    ));

    const deliveredPerSecond = new Map();
    const arrivals = [];
    for (const entry of finalPixels) {
      if (entry.ok !== true || !Number.isFinite(entry.arrivalMs)) continue;
      arrivals.push(entry.arrivalMs);
      const second = Math.floor(entry.arrivalMs / 1000);
      if (second < slotLedger.completeSeconds) {
        deliveredPerSecond.set(second, (deliveredPerSecond.get(second) ?? 0) + 1);
      }
    }
    let zeroDeliverySeconds = 0;
    for (let second = 0; second < slotLedger.completeSeconds; second += 1) {
      if ((deliveredPerSecond.get(second) ?? 0) === 0) zeroDeliverySeconds += 1;
    }
    criteria.push(criterion(
      'zero_delivery_seconds',
      zeroDeliverySeconds === 0 ? 'pass' : 'fail',
      zeroDeliverySeconds === 0
        ? `every complete second decoded at least one identity`
        : `${zeroDeliverySeconds} complete second(s) decoded nothing`,
      { zeroDeliverySeconds, completeSeconds: slotLedger.completeSeconds },
    ));

    if (arrivals.length >= 2) {
      const intervals = [];
      for (let index = 1; index < arrivals.length; index += 1) intervals.push(arrivals[index] - arrivals[index - 1]);
      const sorted = [...intervals].sort((a, b) => a - b);
      const at = (p) => sorted[Math.min(sorted.length - 1, Math.max(0, Math.ceil((p / 100) * sorted.length) - 1))];
      const ideal = 1000 / slotLedger.fps;
      const deviations = intervals.map((value) => Math.abs(value - ideal));
      const mean = intervals.reduce((sum, value) => sum + value, 0) / intervals.length;
      const variance = intervals.reduce((sum, value) => sum + (value - mean) ** 2, 0) / intervals.length;
      criteria.push(criterion(
        'jitter_within_epsilon',
        'pending',
        'authoritative jitter epsilon not adopted (SYNC-CR-005); reported, not gated',
        {
          intervals: intervals.length,
          meanMs: Number(mean.toFixed(3)),
          sampleSdMs: Number(Math.sqrt(variance).toFixed(3)),
          p50Ms: at(50), p95Ms: at(95), p99Ms: at(99), maxMs: sorted[sorted.length - 1],
          maxDeviationFromIdealMs: Number(Math.max(...deviations).toFixed(3)),
          idealMs: Number(ideal.toFixed(4)),
          epsilon: null,
        },
      ));
    } else {
      criteria.push(criterion('jitter_within_epsilon', 'pending',
        'fewer than two timestamped arrivals; no jitter statistic and no adopted epsilon (SYNC-CR-005)'));
    }
  }
  blockers.push('jitter epsilon not adopted: closure verdicts stay invalid until the versioned decision exists (SYNC-CR-005)');
  return { criteria, blockers };
}

// packets: summaries from verifyTonesPacket, in read order. production:
// { windowMs, totalFrames } measured over the wall clock between the first
// and last non-empty read. sourceKind comes from the fixture table, so a
// synthetic fixture is tagged and can never qualify physical 32-channel
// integrity.
export function evaluateAudio({ packets, production, fixture = AUDIO_SOURCES.audio_32_tones, ranAudio = true }) {
  const criteria = [];
  const blockers = [];
  if (ranAudio !== true) {
    criteria.push(criterion('audio_ledger', 'skipped', 'arm did not run the audio path'));
    return { criteria, blockers };
  }
  if (packets === null || packets === undefined || packets.length === 0) {
    criteria.push(criterion('audio_ledger', 'fail', 'the audio path ran but no read completed'));
    blockers.push('audio: no packet ledger; nothing can be judged');
    return { criteria, blockers };
  }

  let discontinuities = 0;
  let nextExpected = null;
  let totalFrames = 0;
  let mismatches = 0;
  let nonfinite = 0;
  let checkedSamples = 0;
  let worstAbsError = 0;
  let serverDropped = 0;
  let observedChannels = 0;
  let observedRate = 0;
  let fixtureMismatches = 0;
  for (const packet of packets) {
    observedChannels = Math.max(observedChannels, packet.channelCount);
    observedRate = Math.max(observedRate, packet.sampleRate);
    if (packet.fixtureMismatch) fixtureMismatches += 1;
    if (packet.frameCount === 0) continue;
    totalFrames += packet.frameCount;
    mismatches += packet.mismatches;
    nonfinite += packet.nonfinite;
    checkedSamples += packet.checkedSamples;
    worstAbsError = Math.max(worstAbsError, packet.worstAbsError);
    serverDropped = Math.max(serverDropped, packet.droppedFrames);
    if (nextExpected !== null && packet.firstFrame !== nextExpected) discontinuities += 1;
    nextExpected = packet.firstFrame + packet.frameCount;
  }

  criteria.push(criterion(
    'channel_count',
    observedChannels === fixture.channels ? 'pass' : 'fail',
    `observed ${observedChannels} of ${fixture.channels} expected channels`,
    { observedChannels, expectedChannels: fixture.channels },
  ));
  criteria.push(criterion(
    'sample_rate',
    observedRate === fixture.sampleRate ? 'pass' : 'fail',
    `observed ${observedRate} Hz of ${fixture.sampleRate} expected`,
    { observedRate, expectedRate: fixture.sampleRate },
  ));
  criteria.push(criterion(
    'cursor_continuity',
    discontinuities === 0 ? 'pass' : 'fail',
    discontinuities === 0
      ? 'every read continued the previous cursor exactly'
      : `${discontinuities} cursor discontinuit${discontinuities === 1 ? 'y' : 'ies'}`,
    { discontinuities },
  ));
  criteria.push(criterion(
    'sample_values',
    mismatches === 0 && nonfinite === 0 ? 'pass' : 'fail',
    `${checkedSamples} samples across all ${fixture.channels} channels checked against the fixture values`
      + `; ${mismatches} mismatched, ${nonfinite} nonfinite${fixtureMismatches > 0 ? `; ${fixtureMismatches} packet(s) did not match the expected fixture format` : ''}`
      + `; worst absolute error ${worstAbsError.toExponential(3)}`,
    { checkedSamples, mismatches, nonfinite, worstAbsError },
  ));
  criteria.push(criterion(
    'server_reported_drops',
    serverDropped === 0 ? 'pass' : 'fail',
    serverDropped === 0 ? 'the server reported no dropped audio frames' : `server reported ${serverDropped} dropped frame(s)`,
    { serverDropped },
  ));

  if (production && Number.isFinite(production.windowMs) && production.windowMs > 0) {
    const expectedFrames = (production.windowMs / 1000) * fixture.sampleRate;
    const produced = production.totalFrames ?? totalFrames;
    const ratio = produced / expectedFrames;
    criteria.push(criterion(
      'wall_clock_production',
      ratio >= PRODUCTION_RATIO_MIN ? 'pass' : 'fail',
      `${produced} sample frames read over a ${production.windowMs.toFixed(0)} ms wall window`
        + ` = ${(ratio * 100).toFixed(3)}% of the ${(PRODUCTION_RATIO_MIN * 100).toFixed(0)}% source-rate floor`
        + ' (a contiguous cursor cannot substitute for this ledger)',
      { produced, expectedFrames: Math.round(expectedFrames), ratio: Number(ratio.toFixed(5)) },
    ));
  } else {
    criteria.push(criterion('wall_clock_production', 'fail', 'no wall-clock production window was measured'));
  }

  if (fixture.kind === 'synthetic') {
    blockers.push('synthetic audio fixture: a synthetic audio_32_tones source cannot qualify the 32 physical '
      + 'channels the acceptance target requires');
  }
  return { criteria, blockers };
}

// The overall verdict. qualified is true only when every criterion of both
// paths passes and no blocker stands; on this collector's path that is
// never the case today (no final-pixel ledger, synthetic audio, unadopted
// jitter epsilon), which is the honest outcome the plan asks for.
export function combinedVerdict({ video, audio }) {
  const blockers = [];
  const criteria = [];
  for (const [path, result] of [['video', video], ['audio', audio]]) {
    if (!result) continue;
    for (const blocker of result.blockers) blockers.push(`${path}: ${blocker}`);
    for (const item of result.criteria) criteria.push({ path, ...item });
  }
  const failed = criteria.filter((item) => item.status === 'fail');
  const blocked = criteria.filter((item) => item.status === 'blocked');
  const pending = criteria.filter((item) => item.status === 'pending');
  return {
    schema: VERDICT_SCHEMA,
    qualified: failed.length === 0 && blocked.length === 0 && pending.length === 0 && blockers.length === 0,
    verdict: failed.length === 0 && blocked.length === 0 && pending.length === 0 && blockers.length === 0
      ? 'QUALIFIED'
      : 'NOT QUALIFIED',
    failed: failed.map((item) => item.name),
    blockedCriteria: blocked.map((item) => item.name),
    pending: pending.map((item) => item.name),
    blockers,
    criteria,
  };
}
