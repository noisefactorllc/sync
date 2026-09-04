import assert from "node:assert/strict";
import { execFile as execFileCallback } from "node:child_process";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { promisify } from "node:util";

const execFile = promisify(execFileCallback);
const directory = process.env.SYNC_PACKAGE_DIR;

test("Linux package is a complete conservative amd64 daemon artifact",
     { skip: !directory }, async () => {
  const entries = await (await import("node:fs/promises")).readdir(directory);
  const packages = entries.filter(name => /^Sync-.+-linux-amd64\.deb$/.test(name));
  assert.equal(packages.length, 1);
  const archive = path.join(directory, packages[0]);
  const { stdout: fields } = await execFile("dpkg-deb", ["--field", archive]);
  assert.match(fields, /^Package: noisedeck-sync$/m);
  assert.match(fields, /^Architecture: amd64$/m);
  assert.match(fields, /^Section: video$/m);
  assert.match(fields, /^Priority: optional$/m);
  assert.match(fields, /^Depends: .*libssl3t64.*libuv1t64.*v4l2loopback-dkms.*v4l2loopback-utils/m);
  assert.match(fields, /^Suggests: .*avahi-daemon.*pipewire.*wireplumber/m);

  const { stdout: contents } = await execFile("dpkg-deb", ["--contents", archive]);
  for (const expected of [
    "./usr/bin/syncd",
    "./usr/bin/syncctl",
    "./usr/lib/systemd/user/noisedeck-sync.service",
    "./usr/lib/udev/rules.d/70-noisedeck-sync-camera.rules",
    "./usr/share/noisedeck-sync/noisedeck-sync-camera.modprobe",
    "./usr/share/noisedeck-sync/noisedeck-sync-camera.modules-load",
    "./usr/share/doc/noisedeck-sync/copyright",
    "./usr/share/doc/noisedeck-sync/Third-Party-Notices.txt",
  ]) assert.match(contents, new RegExp(expected.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")));
  assert.doesNotMatch(contents, / -> |libndi|\.ko(?:\s|$)|\.desktop|autostart|noisedeck\/|browser/i);

  const extraction = await mkdtemp(path.join(os.tmpdir(), "sync-package-test-"));
  try {
    await execFile("dpkg-deb", ["--extract", archive, extraction]);
    const syncd = await execFile("file", [path.join(extraction, "usr/bin/syncd")]);
    const syncctl = await execFile("file", [path.join(extraction, "usr/bin/syncctl")]);
    assert.match(syncd.stdout, /ELF 64-bit LSB.*x86-64/);
    assert.match(syncctl.stdout, /ELF 64-bit LSB.*x86-64/);
    const unit = await readFile(path.join(
      extraction, "usr/lib/systemd/user/noisedeck-sync.service"), "utf8");
    assert.doesNotMatch(unit, /PrivateDevices|DevicePolicy|DeviceAllow/);
    assert.match(unit, /^ProtectSystem=strict$/m);
    assert.match(unit, /^ProtectHome=read-only$/m);
    assert.match(unit,
      /^ReadWritePaths=%h\/\.config\/noisefactor-sync %t\/noisedeck-sync$/m);
    const rule = await readFile(path.join(
      extraction, "usr/lib/udev/rules.d/70-noisedeck-sync-camera.rules"), "utf8");
    assert.doesNotMatch(rule, /SYMLINK/);
  } finally {
    await rm(extraction, { recursive: true, force: true });
  }

  const controlRoot = await mkdtemp(path.join(os.tmpdir(), "sync-package-control-"));
  try {
    await execFile("dpkg-deb", ["--control", archive, controlRoot]);
    const scripts = (await Promise.all(["postrm"].map(async name => {
      try { return await readFile(path.join(controlRoot, name), "utf8"); }
      catch { return ""; }
    }))).join("\n");
    assert.doesNotMatch(scripts,
      /systemctl\s+(?:start|enable)|modprobe\s+-r|rmmod|v4l2loopback-ctl|curl|wget/);
    assert.match(scripts, /cmp -s/);
  } finally {
    await rm(controlRoot, { recursive: true, force: true });
  }
});
