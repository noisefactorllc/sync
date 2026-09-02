import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import os from "node:os";
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

// Skipped unless a bundle has actually been staged: the plain build job
// compiles the tree without running the packaging target, and a test that
// asserted otherwise would fail every run for reasons unrelated to the code.
const stagedOnMacos = {
  skip: process.platform !== "darwin" || !existsSync(app),
};

test("packaged Sync app is self-contained and carries its preview metadata",
     stagedOnMacos, () => {
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

test("packaged Syphon exports the Metal server class", stagedOnMacos, () => {
  const frameworkBinary = path.join(
    contents, "Frameworks/Syphon.framework/Syphon",
  );
  const symbols = execFileSync("/usr/bin/nm", ["-gU", frameworkBinary], {
    encoding: "utf8",
  });
  assert.match(symbols, /(?:^|\s)_OBJC_CLASS_\$_SyphonMetalServer$/m);
});

test("macOS bundle verifier accepts Mach-O targets below the advertised minimum",
     stagedOnMacos, () => {
  const temporaryDirectory = mkdtempSync(path.join(os.tmpdir(), "sync-verify-test-"));
  const copiedApp = path.join(temporaryDirectory, "Sync.app");
  try {
    execFileSync("/usr/bin/ditto", [app, copiedApp]);
    execFileSync("/usr/bin/plutil", [
      "-replace",
      "LSMinimumSystemVersion",
      "-string",
      "99.0",
      path.join(copiedApp, "Contents", "Info.plist"),
    ]);
    const result = spawnSync(path.join(sourceDirectory, "scripts/verify-macos-bundle.sh"), [
      copiedApp,
      plist("CFBundleShortVersionString"),
    ], { encoding: "utf8", timeout: 10_000 });
    assert.equal(result.error, undefined);
    assert.equal(result.status, 0, result.stderr);
  } finally {
    rmSync(temporaryDirectory, { recursive: true, force: true });
  }
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

// package-macos.sh refuses to run without its whole toolchain, and the
// build job installs only what the build needs -- so this is skipped unless
// the packaging tools are actually present. dylibbundler is faked by the
// test itself; the rest have to be real.
function hasCommand(name) {
  return spawnSync("/usr/bin/env", ["sh", "-c", `command -v ${name}`],
                   { stdio: "ignore" }).status === 0;
}

const packagingToolsPresent = process.platform === "darwin" &&
  ["ditto", "rsvg-convert", "sips", "iconutil"].every(hasCommand);

test("macOS dependency bundling is non-interactive and uses its pinned search path", {
  skip: !packagingToolsPresent,
}, () => {
  const temporaryDirectory = mkdtempSync(path.join(os.tmpdir(), "sync-package-test-"));
  try {
    const buildDirectory = path.join(temporaryDirectory, "build");
    const appExecutableDirectory = path.join(buildDirectory, "Sync.app", "Contents", "MacOS");
    const framework = path.join(temporaryDirectory, "Syphon.framework");
    const searchDirectory = path.join(temporaryDirectory, "deps", "lib");
    const fakeBin = path.join(temporaryDirectory, "bin");
    const argumentsFile = path.join(temporaryDirectory, "dylibbundler-arguments");
    mkdirSync(appExecutableDirectory, { recursive: true });
    mkdirSync(framework, { recursive: true });
    mkdirSync(searchDirectory, { recursive: true });
    mkdirSync(fakeBin, { recursive: true });
    for (const executable of [
      path.join(appExecutableDirectory, "Sync"),
      path.join(buildDirectory, "syncd"),
      path.join(buildDirectory, "io.noisefactor.sync.camera"),
    ]) {
      writeFileSync(executable, "#!/bin/sh\nexit 0\n", { mode: 0o755 });
    }
    // The packager copies the configured extension plist out of the build
    // directory; the fixture only needs it to exist.
    writeFileSync(path.join(buildDirectory, "SyncCamera-Info.plist"),
                  '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict/></plist>\n');
    writeFileSync(path.join(fakeBin, "dylibbundler"), [
      "#!/bin/sh",
      "printf '%s\\n' \"$@\" > \"$FAKE_DYLIB_ARGS_FILE\"",
      "IFS= read -r answer",
      "[ \"$answer\" = quit ] || exit 91",
      "exit 23",
      "",
    ].join("\n"), { mode: 0o755 });

    const result = spawnSync(path.join(sourceDirectory, "scripts/package-macos.sh"), [
      "bundle",
      buildDirectory,
      sourceDirectory,
      "0.2.3",
      framework,
      searchDirectory,
    ], {
      encoding: "utf8",
      env: {
        ...process.env,
        FAKE_DYLIB_ARGS_FILE: argumentsFile,
        PATH: `${fakeBin}:${process.env.PATH}`,
      },
      timeout: 10_000,
    });
    assert.equal(result.error, undefined);
    assert.equal(result.status, 23, result.stderr);
    const arguments_ = readFileSync(argumentsFile, "utf8").trim().split("\n");
    assert.deepEqual(arguments_.slice(-2), ["-s", searchDirectory]);
  } finally {
    rmSync(temporaryDirectory, { recursive: true, force: true });
  }
});

test("packaged Sync app carries the Sync Camera extension", stagedOnMacos, () => {
  const extension = path.join(
    contents, "Library/SystemExtensions/io.noisefactor.sync.camera.systemextension",
  );
  const executable = path.join(extension, "Contents/MacOS/io.noisefactor.sync.camera");
  assert.equal(existsSync(executable), true, "missing camera extension executable");
  assert.notEqual(statSync(executable).mode & 0o111, 0, "extension must be executable");
  const extensionInfo = path.join(extension, "Contents/Info.plist");
  const extPlist = (key) => execFileSync(
    "/usr/bin/plutil", ["-extract", key, "raw", "-o", "-", extensionInfo], { encoding: "utf8" },
  ).trim();
  assert.equal(extPlist("CFBundleIdentifier"), "io.noisefactor.sync.camera");
  assert.equal(extPlist("CFBundlePackageType"), "SYSX");
  assert.equal(extPlist("CMIOExtension.CMIOExtensionMachServiceName"),
               "TX27BNWUG9.io.noisefactor.sync.camera");
  assert.equal(extPlist("CFBundleShortVersionString"), plist("CFBundleShortVersionString"));
  assert.equal(extPlist("LSMinimumSystemVersion"), plist("LSMinimumSystemVersion"));
  // The daemon feeds the camera as a CoreMediaIO client, which macOS may
  // attribute to the app as a camera use. A missing usage string is a crash.
  assert.match(plist("NSCameraUsageDescription"), /camera/i);
  assert.match(plist("NSSystemExtensionUsageDescription"), /camera/i);
});

test("entitlements for the app and the camera extension are committed and valid", () => {
  const entitlement = (name) => path.join(sourceDirectory, "packaging/macos", name);
  // plutil exists only on macOS; the content checks below run everywhere
  // this suite does, Windows included.
  if (process.platform === "darwin") {
    for (const name of ["Sync.entitlements", "SyncCamera.entitlements"]) {
      const result = spawnSync("/usr/bin/plutil", ["-lint", entitlement(name)], { encoding: "utf8" });
      assert.equal(result.status, 0, result.stderr);
    }
  }
  assert.match(readFileSync(entitlement("Sync.entitlements"), "utf8"),
               /com\.apple\.developer\.system-extension\.install/);
  assert.match(readFileSync(entitlement("SyncCamera.entitlements"), "utf8"),
               /com\.apple\.security\.app-sandbox/);
});
