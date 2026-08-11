import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { existsSync, readdirSync, statSync } from "node:fs";
import path from "node:path";
import test from "node:test";

const packageDirectory = path.resolve(
  process.env.SYNC_PACKAGE_DIR ?? "build-package/package",
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
