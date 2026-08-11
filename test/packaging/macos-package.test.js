import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
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
const app = path.join(packageDirectory, "Sync.app");
const contents = path.join(app, "Contents");
const info = path.join(contents, "Info.plist");

function plist(key) {
  return execFileSync("/usr/bin/plutil", ["-extract", key, "raw", "-o", "-", info], {
    encoding: "utf8",
  }).trim();
}

function walk(directory) {
  return readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const item = path.join(directory, entry.name);
    return entry.isDirectory() && !entry.isSymbolicLink() ? walk(item) : [item];
  });
}

test("packaged Sync app is self-contained and carries its preview metadata", {
  skip: process.platform !== "darwin",
}, () => {
  assert.equal(existsSync(app), true, `missing package at ${app}`);
  assert.equal(plist("CFBundleIdentifier"), "io.noisefactor.sync");
  assert.equal(plist("LSUIElement"), "true");
  assert.match(plist("CFBundleShortVersionString"), /^0\.2\.\d+$/);
  assert.equal(plist("LSMinimumSystemVersion"), "13.0");
  assert.equal(plist("CFBundleIconFile"), "Sync.icns");

  for (const relative of [
    "MacOS/Sync",
    "MacOS/syncd",
    "Frameworks/Syphon.framework",
    "Resources/Sync.icns",
    "Resources/LICENSE.txt",
    "Resources/Third-Party-Notices.txt",
  ]) {
    assert.equal(existsSync(path.join(contents, relative)), true, `missing ${relative}`);
  }
  for (const relative of ["MacOS/Sync", "MacOS/syncd"]) {
    assert.notEqual(statSync(path.join(contents, relative)).mode & 0o111, 0,
                    `${relative} must be executable`);
  }

  const machObjects = walk(contents).filter((candidate) => {
    if (!statSync(candidate).isFile()) return false;
    return execFileSync("/usr/bin/file", ["-b", candidate], { encoding: "utf8" })
      .includes("Mach-O");
  });
  assert.ok(machObjects.length >= 3, "app, helper, and Syphon must be Mach-O");
  for (const object of machObjects) {
    const dependencies = execFileSync("/usr/bin/otool", ["-L", object], {
      encoding: "utf8",
    }).split("\n").slice(1).map((line) => line.trim().split(" ")[0]).filter(Boolean);
    for (const dependency of dependencies) {
      const allowed = dependency.startsWith("/System/Library/") ||
        dependency.startsWith("/usr/lib/") || dependency.startsWith("@rpath/") ||
        dependency.startsWith("@loader_path/") ||
        dependency.startsWith("@executable_path/");
      assert.equal(allowed, true, `${object} has non-bundled dependency ${dependency}`);
    }
  }
});

test("macOS packaging enforces the advertised deployment target", () => {
  const verifier = readFileSync(
    path.join(sourceDirectory, "scripts/verify-macos-bundle.sh"), "utf8",
  );
  assert.match(verifier, /LC_BUILD_VERSION/);
  assert.match(verifier, /LC_VERSION_MIN_MACOSX/);
  assert.match(verifier, /LSMinimumSystemVersion/);
  assert.match(verifier, /newer than the bundle minimum/);
});

test("distributed third-party notices cover every linked native dependency", () => {
  const notices = readFileSync(
    path.join(sourceDirectory, "packaging/macos/Third-Party-Notices.txt"), "utf8",
  );
  assert.match(notices, /Syphon Framework/);
  assert.match(notices, /71351d4b484cd2d1917867f7846a5cdca724552d/);
  assert.match(notices, /libuv/);
  assert.match(notices, /1cfa32ff59c076ffb6ed735bbc8c18361558661f/);
  assert.match(notices, /Copyright \(c\) 2015-present libuv project contributors/);
  assert.match(notices, /OpenSSL/);
  assert.match(notices, /aae016bfd52fcad2bc9657c2c782cfdf73b1ed5f/);
  assert.match(notices, /Apache License[\s\S]*Version 2\.0, January 2004/);
  assert.match(notices, /END OF TERMS AND CONDITIONS/);
});

test("native menu guide opens the shipped public documentation asset", () => {
  const appSource = readFileSync(
    path.join(sourceDirectory, "native/src/platform/macos/app_main.mm"), "utf8",
  );
  assert.match(appSource, /https:\/\/noisedeck\.app\/docs\/Sync\.md/);
  assert.doesNotMatch(appSource, /https:\/\/noisedeck\.app\/docs\/Sync"/);
});

test("managed helper callbacks are owned and sequenced per child process", () => {
  const processSource = readFileSync(
    path.join(sourceDirectory, "native/src/platform/macos/companion_process.mm"),
    "utf8",
  );
  assert.match(processSource, /struct OwnedTaskState/);
  assert.doesNotMatch(processSource, /Impl[\s\S]*StderrCallback stderr_callback;/);
  assert.doesNotMatch(processSource, /Impl[\s\S]*ExitCallback exit_callback;/);
  assert.equal((processSource.match(/waitUntilExit/g) || []).length, 1);
});
