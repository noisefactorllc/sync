import assert from 'node:assert/strict';
import test from 'node:test';
import {
  captureCombined, footprintKb, footprintKbAsync, parseLeaksReport, powershellArgs, residentKb, residentKbAsync, runLeaks,
} from './process-metrics.mjs';

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
  // Pinned to darwin AND asserting the seam actually ran. Off darwin runLeaks
  // short-circuits without calling `run` at all, so both the pid assertion
  // inside and the null result outside would hold vacuously — the test would
  // report success while proving nothing.
  let invoked = false;
  const result = runLeaks(87332, { platform: 'darwin', run: (pid) => {
    invoked = true;
    assert.equal(pid, 87332);
    return RESTRICTED_OUTPUT;
  } });
  assert.equal(invoked, true, 'the injected run() must actually be reached');
  assert.equal(result.leaks, null);
});

// The bug this round fixed: `execFileSync`'s return value is stdout only, so
// a capture that drops stderr hands the parser exactly this half — and the
// parser, correctly, has no way to tell that apart from a genuine clean
// scan. This test documents that danger at the seam, not inside the parser:
// it proves the parser is fine, and that the capture step is where the
// restriction notice must survive.
test('a seam fed only the stdout half fabricates a clean zero — the capture bug', () => {
  // `platform` is explicit because runLeaks short-circuits off darwin: without
  // it, this test silently stops reaching the parser it exists to exercise on
  // any non-macOS host, and passes for no reason.
  const result = runLeaks(87332, { platform: 'darwin', run: () => RESTRICTED_STDOUT });
  assert.equal(result.leaks, 0);
});

test('a seam fed both streams combined detects the restriction', () => {
  // Darwin-pinned: off darwin this returns null from the platform check, so it
  // would pass without the restriction ever being detected — the exact thing
  // it is meant to prove.
  let invoked = false;
  const result = runLeaks(87332, {
    platform: 'darwin',
    run: () => { invoked = true; return RESTRICTED_STDOUT + RESTRICTED_STDERR; },
  });
  assert.equal(invoked, true, 'the injected run() must actually be reached');
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

// `platform` is explicit throughout these seam tests. The platform branch is
// evaluated BEFORE the injected `run`, so on Windows this ps output would be
// fed to the bytes-to-KiB converter and come back as 24 rather than 24240.
// Without the override the suite only goes green on darwin.
test('residentKb parses ps rss output', () => {
  const result = residentKb(12345, { platform: 'darwin', run: () => '  24240\n' });
  assert.equal(result, 24240);
});

// This one was worse than a failure: on Windows both sides took the byte
// conversion, both returned 24, and the assertion passed — asserting only that
// two identically-wrong numbers agree. An equality test between two code paths
// has to pin the value as well, or it cannot tell "both right" from "both
// wrong".
test('residentKbAsync parses the same ps output as residentKb', async () => {
  const sync = residentKb(12345, { platform: 'darwin', run: () => '  24240\n' });
  const async_ = await residentKbAsync(12345, { platform: 'darwin', run: async () => '  24240\n' });
  assert.equal(async_, sync);
  assert.equal(async_, 24240);
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

// --- Windows -------------------------------------------------------------
//
// Windows has neither `ps` nor `vmmap`. It does expose both halves of the same
// distinction macOS draws, so a soak there reads the leak-relevant number
// rather than the noisy one.

test('residentKb reads WorkingSet64 on Windows and converts bytes to KiB', () => {
  const seen = [];
  const value = residentKb(4242, { platform: 'win32', run: (pid) => { seen.push(pid); return '  268435456 \n'; } });
  assert.equal(value, 262144);
  assert.deepEqual(seen, [4242]);
});

// The whole point of the split: private bytes exclude shared pages the way
// physical footprint does, so a leak shows there while the working set stays
// noisy. Reporting the working set as "footprint" would be a present-but-
// meaningless number, which is the failure mode this harness keeps designing
// against.
test('footprintKb reads private bytes on Windows, not the working set', () => {
  const value = footprintKb(7, { platform: 'win32', run: () => '104857600' });
  assert.equal(value, 102400);
});

test('footprintKb falls back to the resident reading when Windows gives no usable number', () => {
  const value = footprintKb(7, {
    platform: 'win32',
    run: () => 'Get-Process : Cannot find a process with the process identifier 7.',
    fallback: () => 999,
  });
  assert.equal(value, 999);
});

test('residentKbAsync works the same way on Windows', async () => {
  const value = await residentKbAsync(9, { platform: 'win32', run: async () => '2048' });
  assert.equal(value, 2);
});

test('footprintKbAsync reads private bytes on Windows', async () => {
  const value = await footprintKbAsync(9, { platform: 'win32', run: async () => '1048576' });
  assert.equal(value, 1024);
});

// A missing `leaks` binary must read as declared-absent evidence, never as a
// clean scan and never as a crashed run.
test('runLeaks reports no evidence on non-darwin instead of shelling out', () => {
  let spawned = false;
  const report = runLeaks(1, { platform: 'win32', run: () => { spawned = true; return 'x'; } });
  assert.equal(spawned, false);
  assert.equal(report.leaks, null);
  assert.equal(report.bytes, null);
  assert.match(report.raw, /macOS-only/);
});

// macOS behaviour must be untouched by all of the above.
test('darwin still uses ps and vmmap', () => {
  assert.equal(residentKb(1, { platform: 'darwin', run: () => ' 4096 ' }), 4096);
  assert.equal(
    footprintKb(1, { platform: 'darwin', run: () => 'Physical footprint:  12.5M' }),
    12800);
});

// Every other test in this file injects the `run` seam, so none of them ever
// reaches a real command — they would pass whether or not the command is
// correct. This is the one thing that verifies the argv itself.
test('the PowerShell command asks for the right property and pid', () => {
  const args = powershellArgs('(Get-Process -Id %PID%).PrivateMemorySize64', 4242);
  assert.deepEqual(args, [
    '-NoProfile', '-NonInteractive', '-Command',
    '(Get-Process -Id 4242).PrivateMemorySize64',
  ]);
});

// -NoProfile is load-bearing, not decoration: a user profile that prints
// anything lands in stdout and corrupts the number.
test('the PowerShell command suppresses the user profile and interactivity', () => {
  const args = powershellArgs('(Get-Process -Id %PID%).WorkingSet64', 1);
  assert.ok(args.includes('-NoProfile'), '-NoProfile keeps profile output out of stdout');
  assert.ok(args.includes('-NonInteractive'), '-NonInteractive stops it waiting on a prompt');
});

// Windows ships an MSYS `ps` via Git for Windows that EXISTS and rejects
// `-o rss=`, so the resident read THROWS there rather than returning a number.
// footprintKb's contract is to degrade to the coarser reading, so an unguarded
// throw would escape a function that promises not to.
test('footprintKb degrades rather than throwing when the Windows read fails', () => {
  const value = footprintKb(3, {
    platform: 'win32',
    run: () => { throw new Error('powershell.exe not found'); },
    fallback: () => 4096,
  });
  assert.equal(value, 4096);
});

test('footprintKbAsync degrades the same way', async () => {
  const value = await footprintKbAsync(3, {
    platform: 'win32',
    run: async () => { throw new Error('powershell.exe not found'); },
    fallback: async () => 8192,
  });
  assert.equal(value, 8192);
});

// The fallback must itself take the Windows path, or it lands back on `ps`.
test('the Windows fallback is told the platform, so it does not shell out to ps', () => {
  let fallbackOptions = null;
  footprintKb(3, {
    platform: 'win32',
    run: () => 'not a number',
    fallback: (pid, options) => { fallbackOptions = options; return 1; },
  });
  assert.equal(fallbackOptions?.platform, 'win32');
});
