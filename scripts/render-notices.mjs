#!/usr/bin/env node
// Writes the third-party notices for the render helper (sync-render) to
// stdout: the helper's own dependencies, and every component of the Qt it
// ships, taken from Qt's software bill of materials (the SPDX files Qt
// installs under sbom/). The packagers append this to the bundle's
// Third-Party-Notices.txt, so the notices follow exactly the Qt that ships.
//
// Usage:
//   node scripts/render-notices.mjs --sbom-dir <qt>/sbom \
//     --modules qtbase,qtmultimedia,qtwebsockets,qtsvg \
//     --spdx-texts packaging/licenses/spdx \
//     --component "noisemaker-for-qt=<url>=<license file>" \
//     --component "RtMidi=<url>=<license file>"
//
// A license with no text is an error, never a silent omission: the notices
// must carry every license they name.

import { existsSync, readdirSync, readFileSync } from "node:fs";
import path from "node:path";

function parseArguments(argv) {
  const options = { components: [] };
  for (let index = 0; index < argv.length; index += 2) {
    const [flag, value] = [argv[index], argv[index + 1]];
    if (value === undefined) throw new Error(`missing value for ${flag}`);
    if (flag === "--sbom-dir") options.sbomDir = value;
    else if (flag === "--modules") options.modules = value.split(",").filter(Boolean);
    else if (flag === "--spdx-texts") options.spdxTexts = value;
    else if (flag === "--component") {
      const [name, source, licenseFile] = value.split("=");
      if (!name || !source || !licenseFile) throw new Error(`invalid --component ${value}`);
      options.components.push({ name, source, licenseFile });
    } else throw new Error(`unknown argument ${flag}`);
  }
  for (const required of ["sbomDir", "modules", "spdxTexts"]) {
    if (!options[required]) throw new Error(`--${required.replace(/[A-Z]/g, (c) => `-${c.toLowerCase()}`)} is required`);
  }
  return options;
}

// Build tools and build-time helpers appear in the SBOM but are not
// distributed with the app.
function isShipped(pkg, moduleName) {
  const name = pkg.name ?? "";
  if (name === moduleName) return false;
  if (/^(CMake|Compiler |Linker |Bootstrap)/.test(name)) return false;
  return !["syncqt", "moc", "rcc", "tracepointgen", "tracegen", "cmake_automoc_parser",
    "qtwaylandscanner", "uic", "qlalr"].includes(name);
}

const OPERATORS = new Set(["AND", "OR", "WITH"]);
function licenseIds(expression) {
  if (!expression || expression === "NOASSERTION" || expression === "NONE") return [];
  return (expression.match(/[A-Za-z0-9.+-]+/g) ?? []).filter((token) => !OPERATORS.has(token));
}

function main() {
  const options = parseArguments(process.argv.slice(2));
  const lines = [];
  const add = (...text) => lines.push(...text);
  const used = new Map();
  const extracted = new Map();

  add("Sync render helper (sync-render)", "================================", "");
  for (const component of options.components) {
    add(component.name, `Source: ${component.source}`, "", `${component.name} License:`, "",
        readFileSync(component.licenseFile, "utf8").trimEnd(), "", "");
  }

  let qtVersion = null;
  const entries = [];
  for (const moduleName of options.modules) {
    const file = path.join(options.sbomDir,
      readdirSyncSafe(options.sbomDir).find((name) =>
        name.startsWith(`${moduleName}-`) && name.endsWith(".spdx.json")) ?? "");
    if (!existsSync(file)) throw new Error(`no SPDX SBOM for ${moduleName} in ${options.sbomDir}`);
    const sbom = JSON.parse(readFileSync(file, "utf8"));
    for (const info of sbom.hasExtractedLicensingInfos ?? []) {
      extracted.set(info.licenseId, info.extractedText);
    }
    for (const pkg of sbom.packages ?? []) {
      if (!isShipped(pkg, moduleName)) continue;
      if (/^[A-Z][A-Za-z]+$/.test(pkg.name) && /^\d+\.\d+\.\d+$/.test(pkg.versionInfo ?? "")) {
        qtVersion ??= pkg.versionInfo;
      }
      const expression = pkg.licenseConcluded && pkg.licenseConcluded !== "NOASSERTION"
        ? pkg.licenseConcluded : pkg.licenseDeclared;
      entries.push({ module: moduleName, pkg, expression });
      for (const id of licenseIds(expression)) {
        // Qt is used under the LGPL, never under a commercial licence.
        if (id !== "LicenseRef-Qt-Commercial") used.set(id, true);
      }
    }
  }

  add(`Qt ${qtVersion ?? ""}`.trimEnd());
  add("Source: https://download.qt.io/official_releases/qt/");
  add("", "The render helper uses the Qt libraries under the GNU Lesser General Public",
      "License, version 3 (LGPL-3.0-only), linked dynamically and unmodified. You",
      "may replace them with your own build of the same Qt modules. The Qt",
      "modules and the third-party components they contain are listed below from",
      "Qt's software bill of materials, followed by the full text of every",
      "license they name.", "");
  for (const { module: moduleName, pkg, expression } of entries) {
    add(`- ${pkg.name}${pkg.versionInfo ? ` ${pkg.versionInfo}` : ""} (${moduleName})`);
    add(`  License: ${expression ?? "NOASSERTION"}`);
    if (pkg.copyrightText && pkg.copyrightText !== "NOASSERTION" && pkg.copyrightText !== "NONE") {
      add(`  Copyright: ${pkg.copyrightText.replace(/\s*\n\s*/g, "\n             ")}`);
    }
    const source = pkg.downloadLocation && pkg.downloadLocation !== "NOASSERTION"
      ? pkg.downloadLocation : pkg.homepage;
    if (source && source !== "NOASSERTION") add(`  Source: ${source}`);
  }
  add("", "", "License texts", "-------------", "");

  const missing = [];
  for (const id of [...used.keys()].sort()) {
    let text = extracted.get(id);
    if (text === undefined) {
      const file = path.join(options.spdxTexts, `${id}.txt`);
      if (existsSync(file)) text = readFileSync(file, "utf8");
    }
    if (text === undefined) {
      missing.push(id);
      continue;
    }
    add(id, "-".repeat(id.length), "", text.trimEnd(), "", "");
  }
  if (missing.length > 0) {
    throw new Error(`no license text for: ${missing.join(", ")} ` +
      `(add packaging/licenses/spdx/<id>.txt from spdx/license-list-data)`);
  }
  process.stdout.write(`${lines.join("\n")}\n`);
}

function readdirSyncSafe(directory) {
  try {
    return readdirSync(directory);
  } catch {
    return [];
  }
}


try {
  main();
} catch (error) {
  console.error(`render-notices: ${error.message}`);
  process.exit(1);
}
