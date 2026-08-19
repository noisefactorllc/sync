import assert from "node:assert/strict";
import { existsSync, readFileSync, readdirSync, statSync } from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const packageDirectory = path.resolve(
  process.env.SYNC_PACKAGE_DIR ?? "build-package/package",
);
const sourceDirectory = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)), "../..",
);
const bundle = path.join(packageDirectory, "Sync");

function source(relative) {
  return readFileSync(path.join(sourceDirectory, relative), "utf8");
}

const stagedOnWindows = {
  skip: process.platform !== "win32" || !existsSync(bundle),
};

test("packaged Sync bundle is self-contained and x64", stagedOnWindows, () => {
  for (const relative of [
    "Sync.exe",
    "syncd.exe",
    "SpoutLibrary.dll",
    "Sync.ico",
    "LICENSE.txt",
    "Third-Party-Notices.txt",
  ]) {
    assert.equal(existsSync(path.join(bundle, relative)), true, `missing ${relative}`);
  }

  const binaries = readdirSync(bundle).filter((entry) => /\.(exe|dll)$/i.test(entry));
  assert.ok(binaries.length >= 3, "tray app, helper, and SpoutLibrary at minimum");
  for (const binary of binaries) {
    const file = path.join(bundle, binary);
    assert.equal(statSync(file).isFile(), true, `${binary} must be a file`);
    const image = readFileSync(file);
    const headerOffset = image.readUInt32LE(0x3c);
    assert.equal(image.readUInt32LE(headerOffset), 0x0000_4550, `${binary} is not a PE image`);
    assert.equal(image.readUInt16LE(headerOffset + 4), 0x8664, `${binary} is not x64`);
  }
});

// The NDI runtime is a user-installed dependency by licence, not by choice.
// Shipping it would be a licensing violation, so the guard against it is
// asserted at three layers and each one is checked here.
test("the NDI runtime is never redistributed", () => {
  const verifier = source("scripts/verify-windows-bundle.ps1");
  assert.match(verifier, /Processing\\\.NDI\\\./);
  assert.match(verifier, /must not be redistributed/);

  const installer = source("packaging/windows/Sync.iss");
  assert.doesNotMatch(installer, /Processing\.NDI/);
  assert.match(installer, /licence does not permit redistribution/);

  const notices = source("packaging/windows/Third-Party-Notices.txt");
  assert.match(notices, /NDI is NOT redistributed with Sync/);
  assert.match(notices, /NDI_RUNTIME_DIR_V6/);
  assert.match(notices, /registered trademark of Vizrt NDI AB/);
  assert.match(notices, /not\s+affiliated with, endorsed by, or sponsored by/);
});

test("staged bundle never contains the NDI runtime", stagedOnWindows, () => {
  for (const entry of readdirSync(bundle)) {
    assert.doesNotMatch(entry, /^Processing\.NDI\./i, `${entry} must not be redistributed`);
  }
});

test("distributed Windows notices cover every bundled dependency", () => {
  const notices = source("packaging/windows/Third-Party-Notices.txt");
  assert.match(notices, /^Spout$/m);
  assert.match(notices, /leadedge\/Spout2/);
  assert.match(notices, /Copyright \(c\) 2014-2024, Lynn Jarvis/);
  assert.match(notices, /^libuv$/m);
  assert.match(notices, /1cfa32ff59c076ffb6ed735bbc8c18361558661f/);
  assert.match(notices, /^OpenSSL$/m);
  assert.match(notices, /aae016bfd52fcad2bc9657c2c782cfdf73b1ed5f/);
  assert.match(notices, /Apache License[\s\S]*Version 2\.0, January 2004/);
  assert.match(notices, /END OF TERMS AND CONDITIONS/);
});

// A pinned revision is what makes the shipped Spout auditable. The placeholder
// is allowed to exist in the repository, but this test names it so it cannot be
// forgotten: a release must replace it with the revision that was actually built.
test("the bundled Spout revision is pinned or explicitly marked unpinned", () => {
  const notices = source("packaging/windows/Third-Party-Notices.txt");
  const pinned = /Pinned source revision: ([0-9a-f]{40})/.exec(notices);
  const placeholder = /Pinned source revision: TODO-PIN-SPOUT-REVISION/.test(notices);
  assert.ok(
    pinned !== null || placeholder,
    "Spout notices must carry a 40-character revision or the explicit placeholder",
  );
});

test("the installer requires every path to be passed in", () => {
  const installer = source("packaging/windows/Sync.iss");
  for (const define of [
    "SyncVersion",
    "SyncBundleDir",
    "SyncOutputDir",
    "SyncSourceDir",
  ]) {
    assert.match(installer, new RegExp(`#ifndef ${define}[\\s\\S]*?#error`),
                 `${define} must be required`);
  }
  assert.match(installer, /OutputBaseFilename=Sync-\{#SyncVersion\}-x64-Setup/);
  assert.match(installer, /ArchitecturesAllowed=x64compatible/);
  assert.match(installer, /MinVersion=10\.0/);
});

// An unresolved dependency means the installed app fails to start on a clean
// machine. That has to be a hard error, never a warning, or the bundle ships
// broken and nothing notices until a user reports it.
test("runtime dependency bundling fails on an unresolved dependency", () => {
  const script = source("packaging/windows/bundle-dependencies.cmake");
  assert.match(script, /GET_RUNTIME_DEPENDENCIES/);
  assert.match(script, /UNRESOLVED_DEPENDENCIES_VAR/);
  assert.match(script, /if\(_unresolved\)[\s\S]*?message\(FATAL_ERROR/);
  assert.match(script, /POST_EXCLUDE_REGEXES[\s\S]*?"\[Ss\]ystem32"/);
});

// The supervision guarantee the macOS smoke test asserts must hold here too:
// a helper that outlives its supervisor keeps port 53979 and goes unmanaged.
test("the Windows smoke test asserts the helper dies with the app", () => {
  const smoke = source("scripts/smoke-windows-app.ps1");
  assert.match(smoke, /helper survived app quit/);
  assert.match(smoke, /CloseMainWindow/);
  assert.match(smoke, /ParentProcessId -eq \$tray\.Id/);
  // Availability must not be asserted by default: CI has no GPU and no NDI
  // runtime, so a default availability assertion would only pass locally.
  assert.match(smoke, /RequireProvider/);
  assert.match(smoke, /Provider AVAILABILITY is deliberately not asserted/);
});

test("the Windows bundle verifier enforces architecture and completeness", () => {
  const verifier = source("scripts/verify-windows-bundle.ps1");
  assert.match(verifier, /SpoutLibrary\.dll/);
  assert.match(verifier, /0x8664/);
  assert.match(verifier, /Third-Party-Notices\.txt/);
  assert.match(verifier, /ProductVersion/);
});

test("packaging scripts refuse relative and missing paths", () => {
  const packager = source("scripts/package-windows.ps1");
  assert.match(packager, /IsPathRooted/);
  assert.match(packager, /ValidateSet\('bundle', 'installer', 'zip'\)/);
  assert.match(packager, /\^\[0-9\]\+\\\.\[0-9\]\+\\\.\[0-9\]\+\$/);
  // The installer and archive must never be built from a stale or absent stage.
  assert.match(packager, /bundle the app before building the installer/);
  assert.match(packager, /bundle the app before building the archive/);
});
