import assert from 'node:assert/strict';

export function assertLatencyAcceptance(result, requirements, { transportOnly }) {
  assert.equal(result.configuration.width, requirements.width, 'acceptance width mismatch');
  assert.equal(result.configuration.height, requirements.height, 'acceptance height mismatch');
  assert.equal(result.configuration.fps, requirements.fps, 'acceptance fps mismatch');
  assert.equal(
    result.configuration.durationMs,
    requirements.durationMs,
    'acceptance configured duration mismatch',
  );
  assert.ok(
    result.browser.telemetry.observedDurationMs >= requirements.durationMs * 0.9,
    'acceptance observed duration was too short',
  );
  assert.ok(
    result.browser.telemetry.observedRenderFps >= requirements.minimumObservedRenderFps,
    'acceptance observed render fps was too low',
  );
  assert.equal(result.browser.stats.failed, 0, 'browser frame submission failed');
  assert.ok(
    result.browser.stats.sent >= requirements.minimumSamples,
    'acceptance did not send enough sustained sent frames',
  );
  assert.deepEqual(result.browser.consoleErrors, [], 'browser console errors were reported');
  assert.deepEqual(result.browser.pageErrors, [], 'browser page errors were reported');

  if (transportOnly) return;

  assert.equal(result.receiver.probeExitCode, 0, 'receiver probe exited unsuccessfully');
  assert.equal(result.receiver.probeSignal, null, 'receiver probe was terminated by a signal');
  assert.equal(result.receiver.serverFound, true, 'receiver did not find the sender');
  assert.equal(result.receiver.dimensionMismatches, 0, 'receiver texture dimension mismatch');
  assert.equal(result.receiver.commandErrors, 0, 'receiver Metal command failure');
  assert.equal(result.receiver.probeDropped, 0, 'receiver probe dropped readbacks');
  assert.equal(result.receiver.invalidMarkers, 0, 'receiver observed invalid latency markers');
  assert.ok(
    result.receiver.markers >= requirements.minimumSamples,
    'receiver captured too few unique latency samples',
  );
  assert.ok(
    result.receiver.latencyUs.p95 <= requirements.maximumP95LatencyUs,
    'receiver p95 latency exceeded its threshold',
  );
}
