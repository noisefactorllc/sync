import { execFile, execFileSync, spawnSync } from 'node:child_process';
import { promisify } from 'node:util';

const execFileAsync = promisify(execFile);

function parseResidentKb(text) {
  return Number(text.trim());
}

// Windows has no `ps` and no `vmmap`, so a soak there needs its own pair of
// readings. PowerShell exposes both halves of the same distinction macOS draws:
//
//   WorkingSet64        pages resident right now, shared pages included — the
//                       analogue of RSS, and just as misleading for a leak hunt
//   PrivateMemorySize64 committed bytes this process alone owns — the analogue
//                       of physical footprint, and the number a leak shows up in
//
// Both print bytes, so they are converted here rather than at the call sites.
// `-NoProfile` matters: a user profile that prints anything would land in
// stdout and corrupt the parse.
const WINDOWS_RESIDENT_EXPR = '(Get-Process -Id %PID%).WorkingSet64';
const WINDOWS_PRIVATE_EXPR = '(Get-Process -Id %PID%).PrivateMemorySize64';

// Exported so the exact argv is under test. Every other test here injects the
// `run` seam and therefore never reaches a real command — which means the seam
// tests would pass whether or not this command is correct. Windows also ships
// an MSYS `ps` via Git for Windows that EXISTS and rejects these flags, so
// "does ps run?" is not a usable platform probe either; the branch is on
// process.platform and the command itself is verified here.
export const powershellArgs = (expression, pid) =>
  ['-NoProfile', '-NonInteractive', '-Command', expression.replace('%PID%', String(pid))];

function parseWindowsBytesToKb(text) {
  const bytes = Number(String(text).trim());
  if (!Number.isFinite(bytes) || bytes < 0) return NaN;
  return Math.round(bytes / 1024);
}

function defaultPsRun(pid) {
  return execFileSync('ps', ['-o', 'rss=', '-p', String(pid)], { encoding: 'utf8' });
}

function defaultPsRunAsync(pid) {
  return execFileAsync('ps', ['-o', 'rss=', '-p', String(pid)], { encoding: 'utf8' })
    .then((result) => result.stdout);
}

function defaultWindowsRun(expression) {
  return (pid) => execFileSync('powershell.exe', powershellArgs(expression, pid), { encoding: 'utf8' });
}

function defaultWindowsRunAsync(expression) {
  return (pid) => execFileAsync('powershell.exe', powershellArgs(expression, pid), { encoding: 'utf8' })
    .then((result) => result.stdout);
}

export function residentKb(pid, { run, platform = process.platform } = {}) {
  if (platform === 'win32') {
    return parseWindowsBytesToKb((run ?? defaultWindowsRun(WINDOWS_RESIDENT_EXPR))(pid));
  }
  return parseResidentKb((run ?? defaultPsRun)(pid));
}

// Non-blocking twin of residentKb, built on execFile's callback/promise form
// rather than execFileSync. Same command, same parsing, same number — only
// the call no longer blocks the event loop. See footprintKbAsync for why
// this matters.
export async function residentKbAsync(pid, { run, platform = process.platform } = {}) {
  if (platform === 'win32') {
    return parseWindowsBytesToKb(await (run ?? defaultWindowsRunAsync(WINDOWS_RESIDENT_EXPR))(pid));
  }
  return parseResidentKb(await (run ?? defaultPsRunAsync)(pid));
}

function parseFootprintKb(text) {
  const match = /Physical footprint:\s+([\d.]+)([KMG])/.exec(text);
  if (!match) return null;
  const scale = { K: 1, M: 1024, G: 1024 * 1024 }[match[2]];
  return Math.round(Number(match[1]) * scale);
}

function defaultVmmapRun(pid) {
  return execFileSync('vmmap', ['-summary', String(pid)], { encoding: 'utf8' });
}

function defaultVmmapRunAsync(pid) {
  return execFileAsync('vmmap', ['-summary', String(pid)], { encoding: 'utf8' })
    .then((result) => result.stdout);
}

// vmmap is synchronous and takes a few hundred milliseconds, which shows in
// the table as a small dip in frames sent around each sample. Without the
// developer tools it is absent, and RSS stands in.
export function footprintKb(pid, { run, fallback = residentKb, platform = process.platform } = {}) {
  // Windows draws the same distinction macOS does, under different names:
  // private bytes exclude shared pages the way physical footprint does, so a
  // leak shows there while WorkingSet64 (the RSS analogue) stays noisy. Falling
  // through to `fallback` here would have silently reported the WRONG one of
  // the two numbers on Windows, which is exactly the kind of present-but-
  // meaningless reading a soak must never produce.
  if (platform === 'win32') {
    // Guarded exactly as the vmmap read below is. Unguarded, a PowerShell
    // failure propagates out of a function whose whole contract is "degrade to
    // the coarser reading" — and on Windows the fallback throws too, because
    // Git for Windows' MSYS `ps` exists and rejects `-o rss=`.
    let reading;
    try {
      reading = parseWindowsBytesToKb((run ?? defaultWindowsRun(WINDOWS_PRIVATE_EXPR))(pid));
    } catch { reading = NaN; }
    return Number.isFinite(reading) ? reading : fallback(pid, { platform });
  }
  if (platform !== 'darwin') return fallback(pid);
  run = run ?? defaultVmmapRun;
  let text;
  try {
    text = run(pid);
  } catch {
    return fallback(pid);
  }
  const parsed = parseFootprintKb(text);
  return parsed === null ? fallback(pid) : parsed;
}

// Non-blocking twin of footprintKb. A soak run samples the daemon plus every
// Chrome process for its browser profiles — Chromium propagates
// --user-data-dir to renderer, GPU, and utility children, so that is
// upwards of a dozen targets per sample tick. footprintKb's vmmap call is
// synchronous and takes a few hundred milliseconds each; a dozen of those in
// series blocks the event loop for seconds at a time, freezing every timer
// in the harness (including the very stream sampler doing the measuring)
// and manufacturing the sampling gaps a soak analyzer would otherwise blame
// on the system under test. This does the identical work — same command,
// same darwin-vs-fallback branch, same parsing — without blocking.
export async function footprintKbAsync(pid, { run, fallback = residentKbAsync, platform = process.platform } = {}) {
  if (platform === 'win32') {
    let reading;
    try {
      reading = parseWindowsBytesToKb(
        await (run ?? defaultWindowsRunAsync(WINDOWS_PRIVATE_EXPR))(pid));
    } catch { reading = NaN; }
    return Number.isFinite(reading) ? reading : fallback(pid, { platform });
  }
  if (platform !== 'darwin') return fallback(pid);
  run = run ?? defaultVmmapRunAsync;
  let text;
  try {
    text = await run(pid);
  } catch {
    return fallback(pid);
  }
  const parsed = parseFootprintKb(text);
  return parsed === null ? fallback(pid) : parsed;
}

// Runs a command and returns stdout and stderr concatenated (stdout first).
// `leaks` splits its output across both streams — the restriction notice
// ("Process N is not debuggable...") on stderr, the leak-count summary on
// stdout — and `execFileSync`'s return value on the success path is stdout
// only, with stderr left to print straight to the parent's terminal. A
// capture that drops either stream can turn a restricted, unmeasured scan
// into a fabricated clean zero. Exported so this shape is itself under test
// (see process-metrics.test.js), independent of the `leaks` binary.
export function captureCombined(command, args) {
  const result = spawnSync(command, args, { encoding: 'utf8' });
  return `${result.stdout ?? ''}${result.stderr ?? ''}`;
}

function spawnLeaks(pid) {
  // `leaks` exits 1 when it finds something; the report is still useful, on
  // both streams, on every exit path — there is no exception to catch here
  // because spawnSync reports a non-zero exit via `.status`, not by throwing.
  return captureCombined('leaks', ['--quiet', String(pid)]);
}

// `leaks` refuses to inspect the writable memory of a process it isn't
// permitted to debug (hardened runtime without the debug entitlement, no
// passwordless sudo, SIP, ...). When that happens it still prints a
// "0 leaks for 0 total leaked bytes" summary — for whatever readonly memory
// it *could* see — which reads exactly like a genuine clean scan unless this
// restriction marker is checked first. A restricted run measured nothing, so
// it must report unmeasured (null), never a clean zero.
const RESTRICTED = /is not debuggable|security restrictions/i;

export function parseLeaksReport(raw) {
  if (RESTRICTED.test(raw)) return { leaks: null, bytes: null, raw };
  const match = /(\d+) leaks for (\d+) total leaked bytes/.exec(raw);
  if (!match) return { leaks: null, bytes: null, raw };
  return { leaks: Number(match[1]), bytes: Number(match[2]), raw };
}

// `leaks` is a macOS developer tool with no Windows equivalent. Returning the
// same {leaks: null} shape the restricted-process case already returns keeps
// one contract: callers treat a null count as "no leak evidence", which the
// analyzer reports as SKIPPED — declared out of scope up front — rather than as
// a clean scan. Shelling out to a missing binary and letting it throw would
// have surfaced as a run failure instead of an honest absence.
export function runLeaks(pid, { run = spawnLeaks, platform = process.platform } = {}) {
  if (platform !== 'darwin') {
    return {
      leaks: null,
      bytes: null,
      raw: `unreachable-block scanning is macOS-only; no equivalent on ${platform}`,
    };
  }
  return parseLeaksReport(run(pid));
}
