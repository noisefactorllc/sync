import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const sourceDirectory = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const generator = path.join(sourceDirectory, "scripts/render-notices.mjs");

function fixture(directory, { omitText = null } = {}) {
  const sbomDirectory = path.join(directory, "sbom");
  const textsDirectory = path.join(directory, "spdx");
  mkdirSync(sbomDirectory, { recursive: true });
  mkdirSync(textsDirectory, { recursive: true });
  writeFileSync(path.join(sbomDirectory, "qtbase-6.11.1.spdx.json"), JSON.stringify({
    hasExtractedLicensingInfos: [
      { licenseId: "LicenseRef-Qt-Commercial", extractedText: "commercial terms" },
      { licenseId: "LicenseRef-Example", extractedText: "Example licence text." },
    ],
    packages: [
      { name: "qtbase", versionInfo: "59c81a3c+HEAD", licenseConcluded: "NOASSERTION" },
      { name: "moc", versionInfo: "6.11.1", licenseConcluded: "GPL-3.0-only" },
      { name: "CMake", versionInfo: "3.30.5", licenseConcluded: "NOASSERTION" },
      {
        name: "Core", versionInfo: "6.11.1",
        licenseConcluded: "LicenseRef-Qt-Commercial OR LGPL-3.0-only",
        copyrightText: "Copyright (C) The Qt Company Ltd.",
        downloadLocation: "git://code.qt.io/qt/qtbase.git",
      },
      {
        name: "Core_Attribution_example", versionInfo: "1.0",
        licenseConcluded: "BSD-3-Clause AND LicenseRef-Example",
        copyrightText: "Copyright Example Authors",
      },
    ],
  }));
  for (const id of ["LGPL-3.0-only", "BSD-3-Clause", "GPL-3.0-only"]) {
    if (id !== omitText) writeFileSync(path.join(textsDirectory, `${id}.txt`), `${id} full text\n`);
  }
  const license = path.join(directory, "LICENSE");
  writeFileSync(license, "Helper licence text.\n");
  return { sbomDirectory, textsDirectory, license };
}

function run(directory, inputs) {
  return spawnSync(process.execPath, [
    generator,
    "--sbom-dir", inputs.sbomDirectory,
    "--modules", "qtbase",
    "--spdx-texts", inputs.textsDirectory,
    "--component", `Helper=https://example.test/helper=${inputs.license}`,
  ], { encoding: "utf8" });
}

test("render notices list shipped Qt components with their licence texts", () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), "sync-render-notices-"));
  try {
    const result = run(directory, fixture(directory));
    assert.equal(result.status, 0, result.stderr);
    const notices = result.stdout;
    assert.match(notices, /^Sync render helper \(sync-render\)$/m);
    assert.match(notices, /^Helper\nSource: https:\/\/example\.test\/helper\n\nHelper License:\n\nHelper licence text\.$/m);
    assert.match(notices, /^Qt 6\.11\.1$/m);
    assert.match(notices, /^- Core 6\.11\.1 \(qtbase\)\n {2}License: LicenseRef-Qt-Commercial OR LGPL-3\.0-only\n {2}Copyright: Copyright \(C\) The Qt Company Ltd\.\n {2}Source: git:\/\/code\.qt\.io\/qt\/qtbase\.git$/m);
    assert.match(notices, /^- Core_Attribution_example 1\.0 \(qtbase\)$/m);
    // Build tools and the module's own root entry are not shipped.
    assert.doesNotMatch(notices, /^- (moc|CMake|qtbase) /m);
    // Every licence named by a shipped component, once, in full; the commercial
    // licence is never claimed, and a LicenseRef comes from the SBOM itself.
    for (const id of ["BSD-3-Clause", "LGPL-3.0-only"]) {
      assert.equal(notices.split(`\n${id}\n${"-".repeat(id.length)}\n`).length, 2, id);
    }
    assert.match(notices, /^LicenseRef-Example\n-+\n\nExample licence text\.$/m);
    assert.doesNotMatch(notices, /commercial terms/);
    assert.doesNotMatch(notices, /^GPL-3\.0-only full text/m, "moc's licence is not shipped");
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("render notices fail rather than omit a licence text", () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), "sync-render-notices-"));
  try {
    const result = run(directory, fixture(directory, { omitText: "BSD-3-Clause" }));
    assert.equal(result.status, 1);
    assert.match(result.stderr, /no license text for: BSD-3-Clause/);
    assert.equal(result.stdout, "");
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("every vendored SPDX text is a non-empty file named by its identifier", () => {
  const directory = path.join(sourceDirectory, "packaging/licenses/spdx");
  const names = readdirSync(directory);
  assert.ok(names.includes("LGPL-3.0-only.txt"));
  assert.ok(names.includes("GPL-3.0-only.txt"));
  assert.ok(names.includes("LGPL-2.1-or-later.txt"));
  for (const name of names) assert.match(name, /^[A-Za-z0-9.+-]+\.txt$/);
});
