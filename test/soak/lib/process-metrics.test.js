import assert from 'node:assert/strict';
import test from 'node:test';
import { parseLeaksReport, runLeaks, captureCombined,
  residentKb, residentKbAsync, footprintKb, footprintKbAsync } from './process-metrics.mjs';

// On a host where the daemon isn't debuggable, real `leaks` splits its
// output across two streams: the restriction notice on stderr, and a
// "0 leaks for 0 total leaked bytes" summary — drawn from whatever readonly
// memory it could see — on stdout. RESTRICTED_STDERR / RESTRICTED_STDOUT
// model that split; RESTRICTED_OUTPUT is what a capture of *both* streams
// looks like, which is what production must hand the parser.
const RESTRICTED_STDERR = `Process 87332 is not debuggable. Due to security restrictions, leaks can only
show or save contents of readonly memory of restricted processes.

`;

const RESTRICTED_STDOUT = `Process 87332: 5699 nodes malloced for 448 KB
Process 87332: 0 leaks for 0 total leaked bytes.
`;

const RESTRICTED_OUTPUT = RESTRICTED_STDOUT + RESTRICTED_STDERR;

const CLEAN_OUTPUT = `Process 12345: 5699 nodes malloced for 448 KB
Process 12345: 0 leaks for 0 total leaked bytes.
`;

const LEAKY_OUTPUT = `Process 12345: 5699 nodes malloced for 448 KB
Process 12345: 3 leaks for 128 total leaked bytes.
`;

const NO_SUMMARY_OUTPUT = `Process 12345: crashed while scanning
`;

test('a restricted scan is unmeasured, not a clean zero', () => {
  const result = parseLeaksReport(RESTRICTED_OUTPUT);
  assert.equal(result.leaks, null);
  assert.equal(result.bytes, null);
  assert.equal(result.raw, RESTRICTED_OUTPUT);
});

test('a genuine clean scan with no restriction marker reports zero', () => {
  const result = parseLeaksReport(CLEAN_OUTPUT);
  assert.equal(result.leaks, 0);
  assert.equal(result.bytes, 0);
});

test('a scan that finds leaks reports the counts', () => {
  const result = parseLeaksReport(LEAKY_OUTPUT);
  assert.equal(result.leaks, 3);
  assert.equal(result.bytes, 128);
});

test('output with no summary line at all is unmeasured', () => {
  const result = parseLeaksReport(NO_SUMMARY_OUTPUT);
  assert.equal(result.leaks, null);
  assert.equal(result.bytes, null);
});

// The restriction marker must not be case-sensitive: a future macOS message
// rewording ("Is not debuggable", "Security restrictions", ...) must still
// be caught, not slip through and let a trailing "0 leaks" summary read as
// clean. Note this alone isn't what makes the function safe — a leak count
// is only ever produced by the strict `(\d+) leaks for (\d+) total leaked
// bytes` match, so even a marker miss still requires a well-formed summary
// line to read as anything other than unmeasured.
test('the restriction marker is case-insensitive', () => {
  const shouted = `Process 87332 IS NOT DEBUGGABLE. Due to SECURITY RESTRICTIONS, leaks can only
show or save contents of readonly memory of restricted processes.

Process 87332: 5699 nodes malloced for 448 KB
Process 87332: 0 leaks for 0 total leaked bytes.
`;
  const result = parseLeaksReport(shouted);
  assert.equal(result.leaks, null);
  assert.equal(result.bytes, null);
});

test('runLeaks routes the injected run() output through the same parser', () => {
  const result = runLeaks(87332, { run: (pid) => {
    assert.equal(pid, 87332);
    return RESTRICTED_OUTPUT;
  } });
  assert.equal(result.leaks, null);
});

// The bug this round fixed: `execFileSync`'s return value is stdout only, so
// a capture that drops stderr hands the parser exactly this half — and the
// parser, correctly, has no way to tell that apart from a genuine clean
// scan. This test documents that danger at the seam, not inside the parser:
// it proves the parser is fine, and that the capture step is where the
// restriction notice must survive.
test('a seam fed only the stdout half fabricates a clean zero — the capture bug', () => {
  const result = runLeaks(87332, { run: () => RESTRICTED_STDOUT });
  assert.equal(result.leaks, 0);
});

test('a seam fed both streams combined detects the restriction', () => {
  const result = runLeaks(87332, { run: () => RESTRICTED_STDOUT + RESTRICTED_STDERR });
  assert.equal(result.leaks, null);
});

// Exercises the actual production capture function against a real child
// process (not `leaks`, so this runs anywhere) that writes distinct markers
// to stdout and stderr. If a future change swaps this back to something
// that only captures stdout — e.g. reverting to `execFileSync`'s return
// value — the stderr marker vanishes and this test catches it, instead of
// silently re-enabling a fabricated all-clear.
test('captureCombined preserves both streams, not just stdout', () => {
  const combined = captureCombined(process.execPath, ['-e',
    "process.stdout.write('OUT-MARKER-77'); process.stderr.write('ERR-MARKER-88');"]);
  assert.match(combined, /OUT-MARKER-77/);
  assert.match(combined, /ERR-MARKER-88/);
});

// --- residentKb / residentKbAsync -----------------------------------------
// Same injectable-seam approach as runLeaks: the parsing is exercised
// directly, with no real `ps` invocation, and the sync/async forms are
// asserted to agree on identical input.

test('residentKb parses ps rss output', () => {
  const result = residentKb(12345, { run: () => '  24240\n' });
  assert.equal(result, 24240);
});

test('residentKbAsync parses the same ps output as residentKb', async () => {
  const sync = residentKb(12345, { run: () => '  24240\n' });
  const async_ = await residentKbAsync(12345, { run: async () => '  24240\n' });
  assert.equal(async_, sync);
});

// --- footprintKb / footprintKbAsync ---------------------------------------

const VMMAP_SUMMARY_KB = 'Physical footprint:         18432K\n';
const VMMAP_SUMMARY_MB = 'Physical footprint:         12.5M\n';
const VMMAP_UNPARSEABLE = 'vmmap: no summary available\n';

test('footprintKb reads the vmmap physical footprint on darwin (K scale)', () => {
  const result = footprintKb(111, { platform: 'darwin', run: () => VMMAP_SUMMARY_KB });
  assert.equal(result, 18432);
});

test('footprintKb reads the vmmap physical footprint on darwin (M scale)', () => {
  const result = footprintKb(111, { platform: 'darwin', run: () => VMMAP_SUMMARY_MB });
  assert.equal(result, Math.round(12.5 * 1024));
});

test('footprintKb falls back to residentKb off darwin without calling vmmap', () => {
  let vmmapCalled = false;
  const result = footprintKb(111, {
    platform: 'linux',
    run: () => { vmmapCalled = true; return VMMAP_SUMMARY_KB; },
    fallback: () => 999,
  });
  assert.equal(result, 999);
  assert.equal(vmmapCalled, false);
});

test('footprintKb falls back to residentKb when vmmap throws (absent developer tools)', () => {
  const result = footprintKb(111, {
    platform: 'darwin',
    run: () => { throw new Error('vmmap not found'); },
    fallback: () => 777,
  });
  assert.equal(result, 777);
});

test('footprintKb falls back to residentKb when vmmap output has no summary line', () => {
  const result = footprintKb(111, {
    platform: 'darwin',
    run: () => VMMAP_UNPARSEABLE,
    fallback: () => 555,
  });
  assert.equal(result, 555);
});

test('footprintKbAsync agrees with footprintKb on darwin, identical input', async () => {
  const sync = footprintKb(111, { platform: 'darwin', run: () => VMMAP_SUMMARY_KB });
  const async_ = await footprintKbAsync(111, { platform: 'darwin', run: async () => VMMAP_SUMMARY_KB });
  assert.equal(async_, sync);
});

test('footprintKbAsync falls back off darwin without calling vmmap', async () => {
  let vmmapCalled = false;
  const result = await footprintKbAsync(111, {
    platform: 'linux',
    run: async () => { vmmapCalled = true; return VMMAP_SUMMARY_KB; },
    fallback: async () => 999,
  });
  assert.equal(result, 999);
  assert.equal(vmmapCalled, false);
});

test('footprintKbAsync falls back when vmmap rejects (absent developer tools)', async () => {
  const result = await footprintKbAsync(111, {
    platform: 'darwin',
    run: async () => { throw new Error('vmmap not found'); },
    fallback: async () => 777,
  });
  assert.equal(result, 777);
});

test('footprintKbAsync falls back when vmmap output has no summary line', async () => {
  const result = await footprintKbAsync(111, {
    platform: 'darwin',
    run: async () => VMMAP_UNPARSEABLE,
    fallback: async () => 555,
  });
  assert.equal(result, 555);
});
